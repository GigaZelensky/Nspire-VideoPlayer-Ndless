#include "portable_storage_snapshot.h"
#include <string.h>
#include "portable_storage_patterns.h"
#define VIEW_WORD_LIMIT 32768U
#define REGION_BYTES 8560U
static uint32_t le16(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (le16(p + 2) << 16);
}
static bool span(const PortableView *v, uint32_t a, uint32_t bytes, unsigned kind)
{
    uint32_t low = kind == PORTABLE_CODE ? v->code_begin : v->ram_begin;
    uint32_t end = kind == PORTABLE_CODE ? v->code_end : v->ram_end;
    return !(a & 3U) && bytes && low < end && a >= low && bytes <= end - low && a <= end - bytes &&
           v->allow_span(v->context, a, bytes, kind);
}
static bool words(const PortableView *v, PortableStorageSnapshot *s, uint32_t a, uint32_t *out,
                  uint32_t n, unsigned kind)
{
    if (!n || n > 4096U || s->reads > VIEW_WORD_LIMIT - n) {
        s->fault = a;
        return false;
    }
    ++s->spans;
    if (!span(v, a, n * 4U, kind)) {
        s->fault = a;
        return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
        ++s->reads;
        if (!v->read_word(v->context, a + 4U * i, &out[i])) {
            s->fault = a + 4U * i;
            return false;
        }
    }
    return true;
}
static bool word(const PortableView *v, PortableStorageSnapshot *s, uint32_t a, uint32_t *out)
{
    return words(v, s, a, out, 1, PORTABLE_DATA);
}
bool portable_storage_copy(const PortableView *v, uint32_t a, void *destination, uint32_t bytes)
{
    if (!v || !v->allow_span || !v->read_word || !destination || (bytes & 3U) ||
        !span(v, a, bytes, PORTABLE_DATA))
        return false;
    uint8_t *p = destination;
    for (uint32_t i = 0; i < bytes; i += 4) {
        uint32_t w;
        if (!v->read_word(v->context, a + i, &w))
            return false;
        p[i] = (uint8_t)w;
        p[i + 1] = (uint8_t)(w >> 8);
        p[i + 2] = (uint8_t)(w >> 16);
        p[i + 3] = (uint8_t)(w >> 24);
    }
    return true;
}
static bool copy(const PortableView *v, PortableStorageSnapshot *s, uint32_t a, uint8_t *p,
                 uint32_t bytes)
{
    if ((bytes & 3U) || !bytes || bytes / 4U > VIEW_WORD_LIMIT - s->reads) {
        s->fault = a;
        return false;
    }
    s->reads += bytes / 4U;
    ++s->spans;
    if (!portable_storage_copy(v, a, p, bytes)) {
        s->fault = a;
        return false;
    }
    return true;
}
static bool code(const PortableView *v, PortableStorageSnapshot *s, uint32_t a, const uint32_t *p,
                 uint32_t n)
{
    if (n > 512U || !span(v, a, n * 4U, PORTABLE_CODE)) {
        s->fault = a;
        return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t w, expected = p[i], mask = UINT32_MAX;
        if (!words(v, s, a + 4U * i, &w, 1, PORTABLE_CODE))
            return false;
        if ((expected & 0xffff0000U) == 0xf0000000U) {
            expected = a + (expected & 65535U);
        } else if ((expected & 0x0f000000U) == 0x0b000000U)
            mask = 0xff000000U;
        else if ((expected & 0x0f7f0000U) == 0x051f0000U)
            mask = 0xfffff000U;
        if ((w & mask) != (expected & mask)) {
            s->fault = a + 4U * i;
            return false;
        }
    }
    return true;
}
static bool branch(const PortableView *v, PortableStorageSnapshot *s, uint32_t a, uint32_t *target)
{
    uint32_t w;
    if (!words(v, s, a, &w, 1, PORTABLE_CODE) || (w & 0xff000000U) != 0xeb000000U)
        return false;
    int64_t x = (int64_t)a + 8 + ((int32_t)(w << 8) >> 6);
    if (x < 0 || x > UINT32_MAX || !span(v, (uint32_t)x, 4, PORTABLE_CODE)) {
        s->fault = a;
        return false;
    }
    *target = (uint32_t)x;
    return true;
}
static bool literal(const PortableView *v, PortableStorageSnapshot *s, uint32_t at, uint32_t *value)
{
    uint32_t w;
    if (!words(v, s, at, &w, 1, PORTABLE_CODE) || (w & 0x0f7f0000U) != 0x051f0000U)
        return false;
    int64_t target =
        (int64_t)at + 8 + ((w & 0x800000U) ? (int64_t)(w & 4095U) : -(int64_t)(w & 4095U));
    if (target < 0 || target > UINT32_MAX) {
        s->fault = at;
        return false;
    }
    return words(v, s, (uint32_t)target, value, 1, PORTABLE_CODE);
}
static bool follow(const PortableView *v, PortableStorageSnapshot *s, uint32_t a, uint32_t off,
                   const uint32_t *p, uint32_t n, uint32_t *out)
{
    if (a > UINT32_MAX - off) {
        s->fault = a;
        return false;
    }
    return branch(v, s, a + off, out) && code(v, s, *out, p, n);
}
static bool physical(const PortableView *v, PortableStorageSnapshot *s, const uint32_t driver[8],
                     const uint32_t media_ops[24], const uint32_t context_ops[24])
{
    if (!code(v, s, driver[0], STORAGE_PATTERN(upper)) ||
        !code(v, s, media_ops[12], STORAGE_PATTERN(media)))
        return false;
    uint32_t callback = context_ops[2], helper, tag, media_read, mapped_read, bbm;
    /* Bind the geometry, offset, table and cached-map fields to the actual
     * native read path, rather than accepting merely plausible RAM values. */
    if (!follow(v, s, media_ops[12], 0x278U, STORAGE_PATTERN(media_read), &media_read) ||
        !follow(v, s, media_read, 0x88U, STORAGE_PATTERN(mapped_read), &mapped_read) ||
        !follow(v, s, mapped_read, 0x108U, STORAGE_PATTERN(bbm), &bbm))
        return false;
    s->physical_callback = callback;
    if (s->kind == NAND_PAGE_CX_PL351) {
        uint32_t ecc, calc, raw, fim[19], controller[3];
        if (!code(v, s, callback, STORAGE_PATTERN(cx_fim)) ||
            !follow(v, s, callback, 0x168U, STORAGE_PATTERN(cx_ecc), &ecc) ||
            !follow(v, s, ecc, 0x90U, STORAGE_PATTERN(cx_calc), &calc) ||
            !follow(v, s, callback, 0xecU, STORAGE_PATTERN(cx_raw), &raw) ||
            !follow(v, s, callback, 0x12cU, STORAGE_PATTERN(tag), &tag) ||
            !words(v, s, s->fim, fim, 19, PORTABLE_DATA) || fim[15] != 12U || fim[17] != 2U ||
            fim[18] != 8U || (fim[10] & 65535U) != 2048U ||
            !words(v, s, fim[14], controller, 3, PORTABLE_DATA) || controller[0] != 0x8fff1000U ||
            controller[1] != 0x81000000U || !(controller[2] & 65535U))
            return false;
        s->spare = NAND_SPARE_CX_FLASHFX256_TAG12;
        s->tag_column = 2060U;
        s->ecc_first = 2;
        s->ecc_second = 8;
    } else if (s->kind == NAND_PAGE_CX2_SPI) {
        uint32_t forward, magic, command;
        if (!code(v, s, callback, STORAGE_PATTERN(spi_fim)) ||
            !follow(v, s, callback, 0x1fcU, STORAGE_PATTERN(spi_forward), &forward) ||
            !follow(v, s, forward, 0x34U, STORAGE_PATTERN(spi_request), &helper) ||
            !follow(v, s, callback, 0x24cU, STORAGE_PATTERN(tag), &tag) ||
            !literal(v, s, forward + 0x28U, &magic) || magic != 0x20464658U ||
            !literal(v, s, helper + 0x14U, &command) || command != 1001U)
            return false;
        s->spare = NAND_SPARE_CX2_ONDIE_TAG4;
        s->tag_column = 2052U;
    } else
        return false;
    return true;
}
static bool collect_cache(const PortableView *v, PortableResolved *r, PortableStorageSnapshot *s)
{
    uint32_t node;
    if (!word(v, s, r->rel_cache_global, &node))
        return false;
    uint32_t checkpoint = node, power = 1, distance = 0;
    while (node) {
        uint32_t f[6];
        if (s->cache_nodes == 1024U || !words(v, s, node, f, 6, PORTABLE_DATA))
            return false;
        ++s->cache_nodes;
        if ((f[0] & 1U) && f[3] == s->volume) {
            if (!span(v, f[5], s->block_bytes, PORTABLE_DATA)) {
                s->fault = f[5];
                return false;
            }
            PortableStorageBlock b = {f[4], f[5], f[0]};
            if (f[4] >= s->block_count)
                return false;
            if (f[0] & 2U) {
                if (s->dirty_count == PORTABLE_STORAGE_DIRTY_MAX)
                    return false;
                s->dirty[s->dirty_count++] = b;
            } else if (s->clean_count < PORTABLE_STORAGE_CLEAN_MAX)
                s->clean[s->clean_count++] = b;
        }
        node = f[2];
        if (node && node == checkpoint)
            return false;
        if (++distance == power) {
            checkpoint = node;
            distance = 0;
            power *= 2U;
        }
    }
    /* Conflicting cache data for one logical block is not a coherent view. */
    for (unsigned i = 0; i < s->dirty_count; ++i) {
        for (unsigned j = 0; j < i; ++j)
            if (s->dirty[i].number == s->dirty[j].number)
                return false;
        for (unsigned j = 0; j < s->clean_count; ++j)
            if (s->dirty[i].number == s->clean[j].number)
                return false;
    }
    for (unsigned i = 0; i < s->clean_count; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (s->clean[i].number == s->clean[j].number)
                return false;
    return true;
}
int portable_storage_capture(const PortableView *v, PortableResolved *r, uint32_t stream,
                             uint32_t expected, NandPageKind kind, PortableStorageSnapshot *s)
{
    if (!s)
        return PORTABLE_STORAGE_ARGUMENT;
    memset(s, 0, sizeof(*s));
    s->status = PORTABLE_STORAGE_ARGUMENT;
    if (!v || !r || !v->allow_span || !v->read_word ||
        (kind != NAND_PAGE_CX_PL351 && kind != NAND_PAGE_CX2_SPI))
        return s->status;
    s->kind = kind;
    s->stream = stream;
#define REQUIRE(test, stage)                                                                       \
    do {                                                                                           \
        if (!(test)) {                                                                             \
            s->status = (stage);                                                                   \
            return s->status;                                                                      \
        }                                                                                          \
    } while (0)
    REQUIRE((r->capabilities & (PORTABLE_FD_TABLE | PORTABLE_VFS_ROOTS)) ==
                (PORTABLE_FD_TABLE | PORTABLE_VFS_ROOTS),
            PORTABLE_STORAGE_EXPORTS);
    uint32_t f[4];
    REQUIRE(words(v, s, stream, f, 4, PORTABLE_DATA) && (f[3] & 1U) && f[1] < 64U,
            PORTABLE_STORAGE_STREAM);
    REQUIRE(span(v, r->fd_table, 64U * 4U, PORTABLE_DATA) &&
                span(v, r->vfs_table, 10U * 16U, PORTABLE_DATA),
            PORTABLE_STORAGE_VFS);
    s->posix_fd = f[1];
    REQUIRE(word(v, s, r->fd_table + f[1] * 4U, &s->native_fd) && s->native_fd < 10U,
            PORTABLE_STORAGE_VFS);
    uint32_t slot[4], mount[8], ops[6];
    REQUIRE(words(v, s, r->vfs_table + s->native_fd * 16U, slot, 4, PORTABLE_DATA),
            PORTABLE_STORAGE_VFS);
    s->handle = slot[0];
    s->mount = slot[1];
    REQUIRE(words(v, s, s->mount, mount, 8, PORTABLE_DATA) && slot[3] == mount[7],
            PORTABLE_STORAGE_VFS);
    s->operations = mount[0];
    REQUIRE(words(v, s, mount[0], ops, 6, PORTABLE_DATA), PORTABLE_STORAGE_VFS);
    REQUIRE(portable_resolve_reliance(v, ops[5], r), PORTABLE_STORAGE_RELIANCE);
    uint32_t table, head[3], file_entry, live, fields[5], vol_fields[2];
    REQUIRE(word(v, s, r->rel_handles_global, &table) && words(v, s, table, head, 3, PORTABLE_DATA),
            PORTABLE_STORAGE_HANDLE);
    uint32_t count = head[2] & 65535U;
    REQUIRE(count && count <= 64U && s->handle < count && head[1] <= UINT32_MAX - 872U * count,
            PORTABLE_STORAGE_HANDLE);
    file_entry = head[1] + 872U * s->handle;
    REQUIRE(words(v, s, file_entry + 576U, fields, 5, PORTABLE_DATA) &&
                words(v, s, file_entry + 856U, vol_fields, 2, PORTABLE_DATA) && vol_fields[1],
            PORTABLE_STORAGE_HANDLE);
    s->inode = fields[0];
    s->position = fields[2];
    s->file_bytes = fields[3];
    s->volume_id = vol_fields[0] & 65535U;
    REQUIRE(s->inode && s->position <= s->file_bytes &&
                (expected == PORTABLE_STORAGE_ANY_POSITION || expected == s->position),
            PORTABLE_STORAGE_HANDLE);
    uint32_t node, seen[32], nodes = 0;
    REQUIRE(word(v, s, r->rel_volumes_global, &node), PORTABLE_STORAGE_VOLUME);
    while (node && nodes < 32U) {
        uint32_t h[3], tail[2];
        for (unsigned i = 0; i < nodes; ++i)
            REQUIRE(seen[i] != node, PORTABLE_STORAGE_VOLUME);
        seen[nodes++] = node;
        REQUIRE(words(v, s, node, h, 3, PORTABLE_DATA) && node <= UINT32_MAX - 548U &&
                    words(v, s, node + 540U, tail, 2, PORTABLE_DATA),
                PORTABLE_STORAGE_VOLUME);
        if ((tail[0] & 65535U) == s->volume_id) {
            REQUIRE(tail[1], PORTABLE_STORAGE_VOLUME);
            s->volume = h[2];
            break;
        }
        node = h[1];
    }
    REQUIRE(s->volume && word(v, s, r->rel_active_volume_global, &live) && live == s->volume,
            PORTABLE_STORAGE_VOLUME);
    uint32_t volume[28], io[7], io_driver[6];
    REQUIRE(words(v, s, s->volume, volume, 28, PORTABLE_DATA) && volume[0] == 0x554c4f56U &&
                volume[2],
            PORTABLE_STORAGE_VOLUME);
    s->block_bytes = volume[5];
    REQUIRE(s->block_bytes == 2048U && copy(v, s, volume[23], s->meta, 64) &&
                !memcmp(s->meta, "META", 4),
            PORTABLE_STORAGE_VOLUME);
    REQUIRE(words(v, s, volume[27], io, 7, PORTABLE_DATA) &&
                words(v, s, io[0], io_driver, 6, PORTABLE_DATA) && io[5] == 1U && !io[6],
            PORTABLE_STORAGE_VOLUME);
    s->block_count = io[2];
    s->index_block = le32(s->meta + 8);
    REQUIRE(s->block_count && s->index_block && s->index_block < s->block_count,
            PORTABLE_STORAGE_VOLUME);
    REQUIRE(collect_cache(v, r, s), PORTABLE_STORAGE_CACHE);
    REQUIRE(portable_resolve_flashfx(v, io_driver[2], r), PORTABLE_STORAGE_FLASHFX);
    uint32_t device_id = io_driver[5] & 65535U, device_slot[9], disk_id, registry, disks[10],
             disk[18], state_ptr;
    REQUIRE(
        device_id < r->ffx_device_slots &&
            words(v, s, r->ffx_devices_table + 36U * device_id, device_slot, 9, PORTABLE_DATA) &&
            (device_slot[0] & 1U) && word(v, s, device_slot[4], &disk_id),
        PORTABLE_STORAGE_FLASHFX);
    disk_id &= 65535U;
    REQUIRE(disk_id < 8U && word(v, s, r->ffx_disks_global, &registry) &&
                words(v, s, registry, disks, 10, PORTABLE_DATA),
            PORTABLE_STORAGE_FLASHFX);
    s->disk = disks[2U + disk_id];
    REQUIRE(words(v, s, s->disk, disk, 18, PORTABLE_DATA) && !(disk[8] >> 16) &&
                word(v, s, disk[3], &state_ptr),
            PORTABLE_STORAGE_FLASHFX);
    s->state = state_ptr;
    REQUIRE(copy(v, s, state_ptr, s->state_data, sizeof(s->state_data)), PORTABLE_STORAGE_FLASHFX);
    const uint8_t *sd = s->state_data;
    uint32_t units = le16(sd + 32), bias = le16(sd + 22), region_bytes = le32(sd + 24),
             regions = le16(sd + 536);
    REQUIRE(le16(sd) == 64U && le16(sd + 20) == 2048U && le16(sd + 40) == 63U && units &&
                units <= 1024U && bias <= 1024U - units && region_bytes &&
                !(region_bytes % 2048U) && region_bytes / 2048U <= 1024U && regions &&
                s->block_count <= le32(sd + 4) / 2048U &&
                (s->block_count - 1U) / (region_bytes / 2048U) < regions && (sd[2632] & 1U),
            PORTABLE_STORAGE_FLASHFX);
    uint32_t region_cache = le32(sd + 2592), driver[8];
    REQUIRE(words(v, s, le32(sd + 12), driver, 8, PORTABLE_DATA), PORTABLE_STORAGE_FLASHFX);
    REQUIRE(region_cache <= UINT32_MAX - 396U - 3U * REGION_BYTES, PORTABLE_STORAGE_FLASHFX);
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t address = region_cache + 396U + i * REGION_BYTES, h[2], owner;
        REQUIRE(span(v, address, REGION_BYTES, PORTABLE_DATA) &&
                    words(v, s, address, h, 2, PORTABLE_DATA) && word(v, s, address + 352U, &owner),
                PORTABLE_STORAGE_FLASHFX);
        s->region[i] = (PortableStorageRegion){address, h[1] & 65535U,
                                               (h[0] & 65535U) != 0 && owner == s->state};
        if (s->region[i].active) {
            REQUIRE(s->region[i].id < regions, PORTABLE_STORAGE_FLASHFX);
            ++s->regions_active;
        }
    }
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < i; ++j)
            REQUIRE(!s->region[i].active || !s->region[j].active ||
                        s->region[i].id != s->region[j].id,
                    PORTABLE_STORAGE_FLASHFX);
    uint32_t media[16], device[24], context[32], media_ops[24], context_ops[24];
    REQUIRE(words(v, s, le32(sd + 2600), media, 16, PORTABLE_DATA) &&
                words(v, s, media[1], device, 24, PORTABLE_DATA),
            PORTABLE_STORAGE_FLASHFX);
    s->context = device[1];
    REQUIRE(words(v, s, s->context, context, 32, PORTABLE_DATA) &&
                words(v, s, device[2], media_ops, 24, PORTABLE_DATA) &&
                words(v, s, context[13], context_ops, 24, PORTABLE_DATA),
            PORTABLE_STORAGE_FLASHFX);
    REQUIRE(context[6] == 131072U && (context[7] & 65535U) == 2048U && context[4] &&
                context[4] <= 1024U,
            PORTABLE_STORAGE_MAPPING);
    s->fim = context[1];
    REQUIRE(s->fim >= s->context && s->fim <= s->context + sizeof(context) - 4U && !(s->fim & 3U),
            PORTABLE_STORAGE_MAPPING);
    REQUIRE(physical(v, s, driver, media_ops, context_ops), PORTABLE_STORAGE_PHYSICAL);
    s->layout.device_bytes = context[4] * 131072U;
    s->layout.media_bytes = (units + bias) * 131072U;
    s->layout.physical_base_bytes = context[(s->fim - s->context) / 4U];
    uint64_t lower = (uint64_t)device[10] * device[5];
    REQUIRE(lower <= UINT32_MAX / 2048U && media[4] <= UINT32_MAX / 2048U - lower,
            PORTABLE_STORAGE_MAPPING);
    uint64_t before = ((uint64_t)media[4] + lower) * 2048U;
    REQUIRE(before <= UINT32_MAX, PORTABLE_STORAGE_MAPPING);
    s->layout.before_remap_bytes = (uint32_t)before;
    if (context[3] & 0x80000000U) {
        uint32_t bbm[24];
        REQUIRE(context[2], PORTABLE_STORAGE_MAPPING);
        REQUIRE(words(v, s, context[2], bbm, 24, PORTABLE_DATA), PORTABLE_STORAGE_MAPPING);
        uint32_t geometry[4];
        REQUIRE(words(v, s, bbm[16], geometry, 4, PORTABLE_DATA) && geometry[3] == context[6] &&
                    bbm[0] == s->layout.device_bytes,
                PORTABLE_STORAGE_MAPPING);
        s->layout.remap_enabled = (bbm[20] & 1U) != 0;
        s->layout.remap_limit_bytes = bbm[3];
        s->layout.remap_count = bbm[6] & 65535U;
        REQUIRE(s->layout.remap_count <= 1024U && s->layout.remap_count <= (bbm[6] >> 16),
                PORTABLE_STORAGE_MAPPING);
        if (s->layout.remap_count) {
            REQUIRE(copy(v, s, bbm[5], s->remaps, 4U * s->layout.remap_count),
                    PORTABLE_STORAGE_MAPPING);
            s->layout.remaps = s->remaps;
            for (unsigned i = 0; i < s->layout.remap_count; ++i)
                REQUIRE(le16(s->remaps + 4U * i) < context[4] &&
                            le16(s->remaps + 4U * i + 2U) < context[4],
                        PORTABLE_STORAGE_MAPPING);
        }
    }
    /* Native mapper may return its cached source/target without scanning.
     * Our table-only map is equivalent only if that reachable cached result
     * agrees with the first matching table entry (or identity on no match). */
    if ((context[3] & 0x80000000U) && s->layout.remap_enabled) {
        uint32_t cached[2];
        REQUIRE(words(v, s, context[2] + 28U, cached, 2, PORTABLE_DATA), PORTABLE_STORAGE_MAPPING);
        if (cached[0] != UINT32_MAX &&
            (uint64_t)cached[0] * context[6] < s->layout.remap_limit_bytes) {
            REQUIRE(cached[0] < context[4], PORTABLE_STORAGE_MAPPING);
            uint32_t expected_target = cached[0];
            for (unsigned i = 0; i < s->layout.remap_count; ++i)
                if (le16(s->remaps + 4U * i) == cached[0]) {
                    expected_target = le16(s->remaps + 4U * i + 2U);
                    break;
                }
            REQUIRE(cached[1] == expected_target, PORTABLE_STORAGE_MAPPING);
        }
    }
    NandAddressJob check;
    REQUIRE(nand_address_begin(&check, &s->layout, 0), PORTABLE_STORAGE_MAPPING);
    s->status = PORTABLE_STORAGE_OK;
    return s->status;
#undef REQUIRE
}
