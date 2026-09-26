#include "reliance_reader.h"
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p)
{
    return le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static bool bad(RelReader *r, RelError error)
{
    r->error = error;
    return false;
}
static uint8_t *slot_data(RelReader *r, unsigned slot)
{
    return r->workspace + slot * r->block_bytes;
}
static bool overlaps(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return na && nb && (x <= y ? y - x < na : x - y < nb);
}

bool rel_reader_init(RelReader *r, uint32_t bytes, uint32_t count, uint32_t index, uint32_t inode,
                     void *workspace, size_t capacity)
{
    if (!r)
        return false;
    memset(r, 0, sizeof(*r));
    if (bytes < 512 || bytes > 4096 || (bytes & (bytes - 1U)) || !count || !index ||
        index >= count || !inode || !workspace || capacity < (size_t)REL_CACHE_SLOTS * bytes ||
        (uintptr_t)workspace > UINTPTR_MAX - (size_t)REL_CACHE_SLOTS * bytes ||
        overlaps(r, sizeof(*r), workspace, (size_t)REL_CACHE_SLOTS * bytes))
        return bad(r, REL_BAD_ARGUMENT);
    r->block_bytes = bytes;
    r->block_count = count;
    r->index_block = index;
    r->inode_id = inode;
    r->workspace = workspace;
    r->fanout = (bytes - 64U) / 4U;
    while ((1U << r->shift) != bytes)
        ++r->shift;
    r->initialized = true;
    return true;
}
void rel_reader_cancel(RelReader *r)
{
    if (!r)
        return;
    r->active = false;
    r->pending = false;
    r->mapped = false;
}
bool rel_reader_begin(RelReader *r, uint64_t offset, void *destination, uint32_t bytes)
{
    if (!r || !r->initialized)
        return false;
    rel_reader_cancel(r);
    r->error = REL_OK;
    if ((bytes && !destination) || offset > UINT64_MAX - bytes ||
        (uintptr_t)destination > UINTPTR_MAX - bytes ||
        overlaps(destination, bytes, r->workspace, (size_t)REL_CACHE_SLOTS * r->block_bytes) ||
        overlaps(destination, bytes, r, sizeof(*r)))
        return bad(r, REL_BAD_ARGUMENT);
    r->offset = offset;
    r->destination = destination;
    r->requested = bytes;
    r->copied = 0;
    r->range_ready = false;
    r->active = true;
    return true;
}

/* Slots 0/1 pin the index/file inodes. Five temporary slots fit two distinct
 * index-data pages, a DBLI, an INDI and data without resolution thrashing. */
static const uint8_t *fetch(RelReader *r, uint32_t block, int pinned, bool metadata)
{
    if (!block || block >= r->block_count) {
        bad(r, REL_BAD_FORMAT);
        return NULL;
    }
    unsigned first = pinned >= 0 ? (unsigned)pinned : 2U,
             last = pinned >= 0 ? first + 1U : REL_CACHE_SLOTS;
    for (unsigned i = first; i < last; ++i)
        if (r->cache[i].valid && r->cache[i].block == block) {
            r->cache[i].stamp = ++r->clock;
            return slot_data(r, i);
        }
    unsigned victim = first;
    uint32_t oldest = 0;
    for (unsigned i = first; i < last; ++i) {
        if (!r->cache[i].valid) {
            victim = i;
            break;
        }
        uint32_t age = r->clock - r->cache[i].stamp;
        if (age >= oldest) {
            oldest = age;
            victim = i;
        }
    }
    if (r->serial == UINT64_MAX) {
        bad(r, REL_BAD_ARGUMENT);
        return NULL;
    }
    r->cache[victim].valid = false;
    r->pending_slot = victim;
    r->request.block = block;
    r->request.bytes = r->block_bytes;
    r->request.token = ++r->serial;
    r->pending_metadata = metadata;
    r->pending = true;
    return NULL;
}
bool rel_reader_request(const RelReader *r, RelRequest *out)
{
    if (!r || !out || !r->active || !r->pending)
        return false;
    *out = r->request;
    return true;
}
bool rel_reader_supply(RelReader *r, uint64_t token, const void *data, uint32_t bytes)
{
    if (!r || !r->active || !r->pending || token != r->request.token)
        return false;
    r->pending = false;
    if (!data || bytes != r->block_bytes ||
        overlaps(data, bytes, slot_data(r, r->pending_slot), bytes))
        return bad(r, REL_IO_ERROR);
    memcpy(slot_data(r, r->pending_slot), data, bytes);
    RelCacheSlot *slot = &r->cache[r->pending_slot];
    slot->block = r->request.block;
    slot->stamp = ++r->clock;
    slot->valid = true;
    return true;
}
bool rel_reader_fail(RelReader *r, uint64_t token)
{
    if (!r || !r->active || !r->pending || token != r->request.token)
        return false;
    r->pending = false;
    r->error = REL_IO_ERROR;
    return true;
}

static bool inode_info(RelReader *r, const uint8_t *p, uint32_t expected, uint64_t *length,
                       uint32_t *mode)
{
    if (le32(p) != 0x444f4e49U || le32(p + 4) != expected)
        return bad(r, REL_BAD_FORMAT);
    *length = le64(p + 8);
    *mode = le32(p + 40) & 3U;
    uint64_t capacity = r->block_bytes - 64U;
    if (*mode) {
        capacity = (uint64_t)r->fanout * r->block_bytes;
        for (unsigned i = 1; i < *mode; ++i)
            capacity *= r->fanout;
    }
    return *length <= capacity || bad(r, REL_BAD_FORMAT);
}
static bool load_index(RelReader *r)
{
    if (r->index_ready)
        return true;
    const uint8_t *p = fetch(r, r->index_block, 0, true);
    if (!p)
        return false;
    if (!inode_info(r, p, 1, &r->index_length, &r->index_mode))
        return false;
    /* The target's index is inline/direct. Recursive indirect index bootstrap
     * is deliberately refused until independently validated. */
    if (r->index_mode > 1)
        return bad(r, REL_UNSUPPORTED);
    r->index_ready = true;
    return true;
}
static bool index_reference(RelReader *r, uint32_t id, uint32_t *block)
{
    if (!id)
        return bad(r, REL_BAD_FORMAT);
    if (id == 1) {
        *block = r->index_block;
        return true;
    }
    uint64_t offset = (uint64_t)id * 4U;
    if (offset > r->index_length || r->index_length - offset < 4U)
        return bad(r, REL_BAD_FORMAT);
    const uint8_t *p = slot_data(r, 0);
    if (!r->index_mode)
        p += 64U + (size_t)offset;
    else {
        uint32_t number = le32(p + 64U + 4U * (uint32_t)(offset >> r->shift));
        p = fetch(r, number, -1, true);
        if (!p)
            return false;
        p += (uint32_t)offset & (r->block_bytes - 1U);
    }
    *block = le32(p);
    return (*block && *block < r->block_count) || bad(r, REL_BAD_FORMAT);
}
static bool load_file(RelReader *r)
{
    if (r->file_ready)
        return true;
    if (!index_reference(r, r->inode_id, &r->file_block))
        return false;
    const uint8_t *p = fetch(r, r->file_block, 1, true);
    if (!p)
        return false;
    if (!inode_info(r, p, r->inode_id, &r->file_length, &r->file_mode))
        return false;
    r->file_ready = true;
    return true;
}
static bool map_data(RelReader *r, uint32_t page)
{
    if (r->mapped && r->mapped_page == page)
        return true;
    uint32_t span = 1;
    for (unsigned i = 1; i < r->file_mode; ++i)
        span *= r->fanout;
    uint32_t root = page / span, remainder = page % span;
    if (root >= r->fanout)
        return bad(r, REL_BAD_FORMAT);
    uint32_t pointer = le32(slot_data(r, 1) + 64U + root * 4U);
    for (unsigned depth = r->file_mode - 1U; depth; --depth) {
        uint32_t block;
        if (!index_reference(r, pointer, &block))
            return false;
        const uint8_t *p = fetch(r, block, -1, true);
        if (!p)
            return false;
        uint32_t magic = depth == 2 ? 0x494c4244U : 0x49444e49U;
        if (le32(p) != magic || le32(p + 4) != pointer)
            return bad(r, REL_BAD_FORMAT);
        span /= r->fanout;
        uint32_t index = remainder / span;
        remainder %= span;
        pointer = le32(p + 64U + 4U * index);
    }
    if (!pointer || pointer >= r->block_count)
        return bad(r, REL_BAD_FORMAT);
    r->mapped = true;
    r->mapped_page = page;
    r->mapped_block = pointer;
    return true;
}

RelStatus rel_reader_step(RelReader *r, uint32_t budget)
{
    if (!r || !r->initialized)
        return REL_ERROR;
    if (r->error)
        return REL_ERROR;
    if (!r->active)
        return REL_IDLE;
    if (r->pending)
        return REL_NEED_BLOCK;
    if (!budget) {
        bad(r, REL_BAD_ARGUMENT);
        return REL_ERROR;
    }
    if (!load_index(r) || !load_file(r))
        return r->error ? REL_ERROR : REL_NEED_BLOCK;
    if (!r->range_ready) {
        if (r->offset > r->file_length || r->requested > r->file_length - r->offset) {
            bad(r, REL_BAD_RANGE);
            return REL_ERROR;
        }
        r->range_ready = true;
    }
    if (r->copied == r->requested) {
        r->active = false;
        return REL_DONE;
    }
    uint64_t offset = r->offset + r->copied;
    uint32_t remaining = r->requested - r->copied, within = 0;
    const uint8_t *data;
    if (!r->file_mode) {
        data = slot_data(r, 1) + 64U + (size_t)offset;
    } else {
        uint32_t page = (uint32_t)(offset >> r->shift);
        if (!map_data(r, page))
            return r->error ? REL_ERROR : REL_NEED_BLOCK;
        data = fetch(r, r->mapped_block, -1, false);
        if (!data)
            return r->error ? REL_ERROR : REL_NEED_BLOCK;
        within = (uint32_t)offset & (r->block_bytes - 1U);
        data += within;
    }
    uint32_t amount = remaining < budget ? remaining : budget;
    if (r->file_mode && amount > r->block_bytes - within)
        amount = r->block_bytes - within;
    memcpy(r->destination + r->copied, data, amount);
    r->copied += amount;
    if (r->copied == r->requested) {
        r->active = false;
        return REL_DONE;
    }
    return REL_PROGRESS;
}
