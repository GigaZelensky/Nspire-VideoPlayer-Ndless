#include "portable_resolver.h"
#include <string.h>
#include <limits.h>
#include "portable_patterns.h"
#define FUNCTION_WORDS 256U
#define CALLS 24U
#define READ_LIMIT 32768U
/* All arithmetic is explicit 32-bit ARM address arithmetic, checked before
 * crossing the callback boundary. Code literals cannot expand data access. */
static bool allowed(const PortableView *v, uint32_t a, uint32_t bytes, unsigned kind)
{
    uint32_t low = kind == PORTABLE_CODE ? v->code_begin : v->ram_begin;
    uint32_t high = kind == PORTABLE_CODE ? v->code_end : v->ram_end;
    return !(a & 3U) && bytes && low < high && a >= low && bytes <= high - low &&
           a <= high - bytes && v->allow_span(v->context, a, bytes, kind);
}
static bool read_at(const PortableView *v, PortableResolved *r, uint32_t a, unsigned kind,
                    uint32_t *word)
{
    if (r->reads >= READ_LIMIT) {
        r->errors |= PORTABLE_LIMIT;
        r->last_rejected = a;
        return false;
    }
    ++r->reads;
    if (!allowed(v, a, 4, kind) || !v->read_word(v->context, a, word)) {
        r->errors |= PORTABLE_ACCESS;
        r->last_rejected = a;
        return false;
    }
    return true;
}
static bool add(uint32_t a, uint32_t b, uint32_t *out)
{
    if (a > UINT32_MAX - b)
        return false;
    *out = a + b;
    return true;
}
static bool bl_target(const PortableView *v, PortableResolved *r, uint32_t at, uint32_t *target)
{
    uint32_t w;
    if (!read_at(v, r, at, PORTABLE_CODE, &w) || (w & 0xff000000U) != 0xeb000000U)
        return false;
    int32_t displacement = (int32_t)(w << 8) >> 6;
    int64_t result = (int64_t)at + 8 + displacement;
    if (result < 0 || result > UINT32_MAX || !allowed(v, (uint32_t)result, 4, PORTABLE_CODE))
        return false;
    *target = (uint32_t)result;
    return true;
}
static bool literal(const PortableView *v, PortableResolved *r, uint32_t at, uint32_t *value)
{
    uint32_t w;
    if (!read_at(v, r, at, PORTABLE_CODE, &w) || (w & 0x0f7f0000U) != 0x051f0000U)
        return false;
    int64_t where =
        (int64_t)at + 8 + ((w & 0x800000U) ? (int64_t)(w & 4095U) : -(int64_t)(w & 4095U));
    if (where < 0 || where > UINT32_MAX)
        return false;
    return read_at(v, r, (uint32_t)where, PORTABLE_CODE, value);
}
static bool data_global(const PortableView *v, uint32_t address, uint32_t bytes)
{
    return allowed(v, address, bytes, PORTABLE_DATA);
}
static bool matches(const PortableView *v, PortableResolved *r, uint32_t at,
                    const uint32_t *pattern, uint32_t count)
{
    if (count > FUNCTION_WORDS || !allowed(v, at, count * 4U, PORTABLE_CODE))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w, p = pattern[i], mask = UINT32_MAX;
        if ((p & 0x0f000000U) == 0x0b000000U)
            mask = 0xff000000U;
        else if ((p & 0x0f7f0000U) == 0x051f0000U)
            mask = 0xfffff000U;
        if (!read_at(v, r, at + i * 4U, PORTABLE_CODE, &w) || (w & mask) != (p & mask)) {
            r->last_rejected = at + i * 4U;
            return false;
        }
    }
    return true;
}
static bool unique(uint32_t *value, uint32_t candidate, PortableResolved *r)
{
    if (*value && *value != candidate) {
        r->errors |= PORTABLE_AMBIGUOUS;
        return false;
    }
    *value = candidate;
    return true;
}
/* Only branches/calls/returns are interpreted. Registers are never executed.
 * A return-predicated path keeps its fallthrough; loops have one visited bit.
 * Unexpected control flow/cap exhaustion rejects the candidate function. */
static bool calls(const PortableView *v, PortableResolved *r, uint32_t entry, uint32_t out[CALLS],
                  unsigned *count)
{
    uint8_t seen[FUNCTION_WORDS] = {0};
    uint16_t pending[FUNCTION_WORDS];
    unsigned n = 1, visited = 0;
    pending[0] = 0;
    *count = 0;
    if (!allowed(v, entry, 4, PORTABLE_CODE) || entry > UINT32_MAX - FUNCTION_WORDS * 4U)
        return false;
    while (n) {
        uint32_t index = pending[--n];
        if (seen[index])
            continue;
        seen[index] = 1;
        ++visited;
        uint32_t pc = entry + index * 4U, w;
        if (!read_at(v, r, pc, PORTABLE_CODE, &w))
            return false;
        bool next = true;
        unsigned cond = w >> 28;
        if (cond == 15U)
            return false;
        if ((w & 0x0e000000U) == 0x0a000000U) {
            int32_t disp = (int32_t)(w << 8) >> 6;
            int64_t target = (int64_t)pc + 8 + disp;
            if (target < 0 || target > UINT32_MAX ||
                !allowed(v, (uint32_t)target, 4, PORTABLE_CODE))
                return false;
            if (w & 0x01000000U) {
                bool found = false;
                for (unsigned j = 0; j < *count; ++j)
                    if (out[j] == (uint32_t)target)
                        found = true;
                if (!found) {
                    if (*count == CALLS) {
                        r->errors |= PORTABLE_LIMIT;
                        return false;
                    }
                    out[(*count)++] = (uint32_t)target;
                }
            } else {
                if (target < entry || target >= entry + FUNCTION_WORDS * 4U)
                    return false;
                uint32_t ix = ((uint32_t)target - entry) / 4U;
                if (!seen[ix]) {
                    if (n == FUNCTION_WORDS) {
                        r->errors |= PORTABLE_LIMIT;
                        return false;
                    }
                    pending[n++] = (uint16_t)ix;
                }
                next = cond != 14U;
            }
        } else if ((w & 0x0ffffff0U) == 0x012fff10U ||
                   ((w & 0x0e100000U) == 0x08100000U && (w & 0x8000U)) ||
                   ((w & 0x0c10f000U) == 0x0410f000U))
            next = cond != 14U;
        /* BLX register returns to the next instruction; BX is handled above. */
        if (next) {
            if (index + 1U == FUNCTION_WORDS || n == FUNCTION_WORDS) {
                r->errors |= PORTABLE_LIMIT;
                return false;
            }
            if (!seen[index + 1U])
                pending[n++] = (uint16_t)(index + 1U);
        }
    }
    return visited != 0;
}
static bool fd_lookup(const PortableView *v, PortableResolved *r, uint32_t at, uint32_t *table)
{
    const uint32_t p[] = {0xe3700001U, 0x159f3004U, 0x17930100U, 0xe12fff1eU};
    return matches(v, r, at, p, 4) && literal(v, r, at + 4U, table) &&
           data_global(v, *table, 64U * 4U);
}
static bool resolve_fd(const PortableView *v, const PortableAnchors *a, PortableResolved *r)
{
    uint32_t funcs[2] = {0}, tables[2] = {0}, roots[2] = {a->fread, a->fwrite};
    for (unsigned k = 0; k < 2; ++k) {
        uint32_t targets[CALLS];
        unsigned count;
        if (!calls(v, r, roots[k], targets, &count))
            return false;
        for (unsigned i = 0; i < count; ++i) {
            uint32_t table;
            if (fd_lookup(v, r, targets[i], &table)) {
                if (!unique(&funcs[k], targets[i], r) || !unique(&tables[k], table, r))
                    return false;
            }
        }
    }
    if (!funcs[0] || funcs[0] != funcs[1] || tables[0] != tables[1]) {
        r->errors |= PORTABLE_INCONSISTENT;
        return false;
    }
    uint32_t used_lookup;
    if (!matches(v, r, a->fread, PATTERN(stream_prefix)) ||
        !bl_target(v, r, a->fread + 28U, &used_lookup) || used_lookup != funcs[0]) {
        r->errors |= PORTABLE_INCONSISTENT;
        return false;
    }
    r->fd_lookup = funcs[0];
    r->fd_table = tables[0];
    return true;
}
static bool resolve_vfs(const PortableView *v, const PortableAnchors *a, PortableResolved *r)
{
    uint32_t top[CALLS], candidate = 0, table = 0;
    unsigned nt;
    if (!calls(v, r, a->fread, top, &nt))
        return false;
    for (unsigned i = 0; i < nt; ++i) {
        uint32_t second[CALLS];
        unsigned ns;
        if (!calls(v, r, top[i], second, &ns))
            continue;
        bool fd = false;
        for (unsigned j = 0; j < ns; ++j)
            if (second[j] == r->fd_lookup)
                fd = true;
        if (!fd)
            continue;
        for (unsigned j = 0; j < ns; ++j) {
            uint32_t nu = second[j], mount, handle, mt, ht;
            if (!matches(v, r, nu, PATTERN(nuread)))
                continue;
            if (!bl_target(v, r, nu + 32U, &mount) || !bl_target(v, r, nu + 60U, &handle) ||
                !matches(v, r, mount, PATTERN(vfsmount)) ||
                !matches(v, r, handle, PATTERN(vfshandle)) || !literal(v, r, mount + 36U, &mt) ||
                !literal(v, r, handle + 28U, &ht) || mt != ht || !data_global(v, mt, 10U * 16U))
                return false;
            if (!unique(&candidate, nu, r) || !unique(&table, mt, r))
                return false;
        }
    }
    if (!candidate)
        return false;
    r->native_read = candidate;
    r->vfs_table = table;
    return true;
}
static bool resolve_heap(const PortableView *v, const PortableAnchors *a, PortableResolved *r)
{
    uint32_t result = 0;
    /* The exported allocator's exact prologue and pool-load/call sequence are
     * shared by both captured ARM library families. No allocator is called. */
    if (!matches(v, r, a->malloc, PATTERN(heap)) || !allowed(v, a->malloc, 128U, PORTABLE_CODE))
        return false;
    for (unsigned offset = 16; offset < 128; offset += 4) {
        uint32_t w;
        if (!read_at(v, r, a->malloc + offset, PORTABLE_CODE, &w))
            return false;
        if ((w & 0xfffff000U) != 0xe59f3000U)
            continue;
        uint32_t global, obj, magic, start, total, free;
        if (!literal(v, r, a->malloc + offset, &global) || !data_global(v, global, 4) ||
            !read_at(v, r, global, PORTABLE_DATA, &obj) || !data_global(v, obj, 48) ||
            !read_at(v, r, obj + 20U, PORTABLE_DATA, &magic) || magic != 0x44594e41U)
            continue;
        if (!read_at(v, r, obj + 32U, PORTABLE_DATA, &start) ||
            !read_at(v, r, obj + 36U, PORTABLE_DATA, &total) ||
            !read_at(v, r, obj + 44U, PORTABLE_DATA, &free))
            return false;
        if (start < v->ram_begin || start >= v->ram_end || !total || total > v->ram_end - start ||
            free > total)
            continue;
        const uint32_t tail[] = {0xe1a02005U, 0xe5930000U, 0xe28d1004U, 0xe1a03004U, 0xeb000000U};
        if (!matches(v, r, a->malloc + offset + 4U, tail, 5))
            continue;
        if (!unique(&result, global, r))
            return false;
    }
    if (!result)
        return false;
    r->heap_global = result;
    return true;
}
uint32_t portable_resolve_exports(const PortableView *v, const PortableAnchors *a,
                                  PortableResolved *r)
{
    if (!r)
        return 0;
    memset(r, 0, sizeof(*r));
    if (!v || !a || !v->allow_span || !v->read_word || v->code_begin >= v->code_end ||
        v->ram_begin >= v->ram_end) {
        r->errors = PORTABLE_BAD_VIEW;
        return 0;
    }
    if (resolve_fd(v, a, r)) {
        r->capabilities |= PORTABLE_FD_TABLE;
        if (resolve_vfs(v, a, r))
            r->capabilities |= PORTABLE_VFS_ROOTS;
    }
    uint32_t global, magic;
    if (matches(v, r, a->current_task, PATTERN(task)) && literal(v, r, a->current_task, &global) &&
        literal(v, r, a->current_task + 0x1cU, &magic) && magic == 0x5441534bU &&
        data_global(v, global, 4)) {
        r->task_global = global;
        r->capabilities |= PORTABLE_TASK_GLOBAL;
    }
    if (resolve_heap(v, a, r))
        r->capabilities |= PORTABLE_HEAP_GLOBAL;
    if (!r->capabilities)
        r->errors |= PORTABLE_NO_MATCH;
    if (!r->errors)
        r->last_rejected = 0;
    return r->capabilities;
}
static bool follow(const PortableView *v, PortableResolved *r, uint32_t base, uint32_t offset,
                   const uint32_t *pattern, uint32_t count, uint32_t *target)
{
    uint32_t pc;
    if (!add(base, offset, &pc))
        return false;
    if (!bl_target(v, r, pc, target)) {
        r->last_rejected = pc;
        return false;
    }
    return matches(v, r, *target, pattern, count);
}
bool portable_resolve_reliance(const PortableView *v, uint32_t read, PortableResolved *r)
{
    uint32_t handle, entry, acquire, find, range, setvol, inode, get, cache, handles, volumes,
        active, cached, cross, magic;
    if (!v || !r || !v->allow_span || !v->read_word)
        return false;
    r->capabilities &= ~PORTABLE_RELIANCE_ROOTS;
    r->reliance_read = r->rel_handles_global = r->rel_volumes_global = 0;
    r->rel_active_volume_global = r->rel_cache_global = 0;
    r->last_rejected = 0;
    if (!matches(v, r, read, PATTERN(relread)) ||
        !follow(v, r, read, 0x88U, PATTERN(relhandle), &handle) ||
        !follow(v, r, handle, 0x50U, PATTERN(relentry), &entry) ||
        !follow(v, r, handle, 0x80U, PATTERN(volacquire), &acquire) ||
        !follow(v, r, acquire, 0x34U, PATTERN(volfind), &find) ||
        !literal(v, r, handle + 0x3cU, &handles) || !literal(v, r, find + 0x14U, &volumes) ||
        !follow(v, r, read, 0x178U, PATTERN(range), &range) ||
        !follow(v, r, range, 0x3cU, PATTERN(setvol), &setvol) ||
        !literal(v, r, setvol + 0x44U, &active) || !literal(v, r, setvol + 0x2cU, &magic) ||
        magic != 0x554c4f56U || !follow(v, r, range, 0x90U, PATTERN(inode), &inode) ||
        !follow(v, r, inode, 0x5cU, PATTERN(cacheget), &get) ||
        !follow(v, r, get, 0x78U, PATTERN(cachefind), &cache) ||
        !literal(v, r, cache + 0x68U, &cached) || !literal(v, r, get + 0xc4U, &cross) ||
        cross != active || !data_global(v, handles, 4) || !data_global(v, volumes, 4) ||
        !data_global(v, active, 4) || !data_global(v, cached, 4)) {
        r->errors |= PORTABLE_NO_MATCH;
        return false;
    }
    static const uint32_t cache_refs[] = {0x17cU, 0x18cU, 0x264U};
    static const uint32_t volume_refs[] = {0xa8U, 0x1c4U, 0x2a4U};
    for (unsigned i = 0; i < 3; ++i) {
        if (!literal(v, r, cache + cache_refs[i], &cross) || cross != cached ||
            !literal(v, r, cache + volume_refs[i], &cross) || cross != active) {
            r->errors |= PORTABLE_INCONSISTENT;
            return false;
        }
    }
    r->reliance_read = read;
    r->rel_handles_global = handles;
    r->rel_volumes_global = volumes;
    r->rel_active_volume_global = active;
    r->rel_cache_global = cached;
    r->capabilities |= PORTABLE_RELIANCE_ROOTS;
    return true;
}
static bool device_profile(const PortableView *v, PortableResolved *r, uint32_t device,
                           uint32_t *slots)
{
    /* Both supplied images have the same device-record ABI. Their only code
     * difference is the literal bound: CX has ten records, CX II forty. */
    uint32_t bound;
    if (!allowed(v, device, sizeof(pattern_devlookup), PORTABLE_CODE))
        return false;
    if (!read_at(v, r, device + 0x34U, PORTABLE_CODE, &bound))
        return false;
    if (bound != 0xe3550009U && bound != 0xe3550027U)
        return false;
    *slots = (bound & 255U) + 1U;
    for (uint32_t i = 0; i < sizeof(pattern_devlookup) / sizeof(uint32_t); ++i) {
        uint32_t actual, expected = pattern_devlookup[i], mask = UINT32_MAX;
        if (i == 13U)
            expected = bound;
        if ((expected & 0x0f000000U) == 0x0b000000U)
            mask = 0xff000000U;
        else if ((expected & 0x0f7f0000U) == 0x051f0000U)
            mask = 0xfffff000U;
        if (!read_at(v, r, device + 4U * i, PORTABLE_CODE, &actual) ||
            (actual & mask) != (expected & mask)) {
            r->last_rejected = device + 4U * i;
            return false;
        }
    }
    return true;
}
bool portable_resolve_flashfx(const PortableView *v, uint32_t read, PortableResolved *r)
{
    uint32_t resolve, device, disk, devices, disks, slots;
    if (!v || !r || !v->allow_span || !v->read_word)
        return false;
    r->capabilities &= ~PORTABLE_FLASHFX_ROOTS;
    r->flashfx_read = r->ffx_disks_global = r->ffx_devices_table = r->ffx_device_slots = 0;
    r->last_rejected = 0;
    if (!matches(v, r, read, PATTERN(ffxread)) ||
        !follow(v, r, read, 0x34U, PATTERN(ffxresolve), &resolve) ||
        !bl_target(v, r, resolve + 0x30U, &device) || !device_profile(v, r, device, &slots) ||
        !follow(v, r, resolve, 0x84U, PATTERN(disklookup), &disk) ||
        !literal(v, r, resolve + 0x70U, &disks) || !literal(v, r, device + 0x3cU, &devices) ||
        !data_global(v, disks, 4) || !data_global(v, devices, slots * 36U)) {
        r->errors |= PORTABLE_NO_MATCH;
        return false;
    }
    r->flashfx_read = read;
    r->ffx_disks_global = disks;
    r->ffx_devices_table = devices;
    r->ffx_device_slots = slots;
    r->capabilities |= PORTABLE_FLASHFX_ROOTS;
    return true;
}
