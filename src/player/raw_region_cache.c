#include "raw_region_cache.h"
#include <string.h>

static uint32_t half(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t word(const uint8_t *p)
{
    return half(p) | (half(p + 2) << 16);
}
static void invalidate_entry(RawRegionCache *c, RawRegionCacheEntry *e)
{
    if (e->valid)
        ++c->stats.invalidations;
    e->valid = false;
}
void raw_region_cache_end_view(RawRegionCache *c)
{
    if (c) {
        c->view_active = false;
        c->view_state = NULL;
    }
}
void raw_region_cache_clear(RawRegionCache *c)
{
    if (!c)
        return;
    for (unsigned i = 0; i < RAW_REGION_CACHE_SLOTS; ++i)
        invalidate_entry(c, &c->entry[i]);
    raw_region_cache_end_view(c);
    c->have_geometry = false;
}
static void geometry(const FfxMap *m, uint32_t out[10])
{
    out[0] = m->owner;
    out[1] = m->logical_blocks;
    out[2] = m->page_bytes;
    out[3] = m->region_pages;
    out[4] = m->region_count;
    out[5] = m->unit_pages;
    out[6] = m->data_pages;
    out[7] = m->unit_bias;
    out[8] = m->unit_count;
    out[9] = m->initialized;
}
static bool same_layout(const RawRegionCache *c, const NandAddressMap *m)
{
    const NandAddressMap *a = &c->layout;
    return a->media_bytes == m->media_bytes && a->device_bytes == m->device_bytes &&
           a->before_remap_bytes == m->before_remap_bytes &&
           a->physical_base_bytes == m->physical_base_bytes &&
           a->remap_limit_bytes == m->remap_limit_bytes && a->remap_count == m->remap_count &&
           a->remap_enabled == m->remap_enabled &&
           (!m->remap_count || !memcmp(c->remaps, m->remaps, m->remap_count * 4U));
}
static unsigned members(const RawRegionCache *c, uint32_t region,
                        uint16_t out[RAW_REGION_CACHE_UNITS])
{
    unsigned count = 0;
    for (unsigned unit = 0; unit < c->geometry[8]; ++unit)
        if (half(c->view_state + 538U + 2U * unit) == region) {
            if (count == RAW_REGION_CACHE_UNITS)
                return 0;
            out[count++] = (uint16_t)unit;
        }
    return count;
}
static bool physical_block(const RawRegionCache *c, uint16_t unit, uint16_t *block)
{
    uint64_t offset = ((uint64_t)unit + c->geometry[7]) * 131072U;
    NandAddressJob first, last;
    if (offset > UINT32_MAX - 63U * 2048U ||
        !nand_address_begin(&first, &c->layout, (uint32_t)offset) ||
        !nand_address_begin(&last, &c->layout, (uint32_t)offset + 63U * 2048U))
        return false;
    for (unsigned n = 0; n < 17U && first.status == NAND_ADDRESS_MORE; ++n)
        nand_address_step(&first, 64U);
    for (unsigned n = 0; n < 17U && last.status == NAND_ADDRESS_MORE; ++n)
        nand_address_step(&last, 64U);
    if (first.status != NAND_ADDRESS_READY || last.status != NAND_ADDRESS_READY ||
        (first.raw_page & 63U) || last.raw_page != first.raw_page + 63U ||
        first.raw_page / 64U >= 1024U)
        return false;
    *block = (uint16_t)(first.raw_page / 64U);
    return true;
}
static bool unchanged(RawRegionCache *c, RawRegionCacheEntry *e)
{
    uint16_t current[RAW_REGION_CACHE_UNITS];
    if (!e->valid || !c->view_epoch || e->epoch != c->view_epoch ||
        c->mutations.epoch(c->mutations.context) != c->view_epoch)
        return false;
    unsigned count = members(c, e->region, current);
    if (count != e->count || !count || memcmp(current, e->units, count * sizeof(current[0])))
        return false;
    /* Store proved each unit's first/last physical page. begin_view discards
     * every proof on any geometry/layout/remap-byte change; the owned layout
     * is immutable here. With identical ordered members the translation is
     * identical, so only its physical mutation generations need rereading. */
    for (unsigned i = 0; i < count; ++i) {
        if (e->blocks[i] >= 1024U ||
            c->mutations.generation(c->mutations.context, e->blocks[i]) != e->generations[i])
            return false;
    }
    return c->mutations.epoch(c->mutations.context) == c->view_epoch;
}
bool raw_region_cache_begin_view(RawRegionCache *c, const FfxMap *m, const NandAddressMap *layout,
                                 const uint8_t *state, size_t bytes, RawRegionMutations mutations)
{
    uint32_t current[10];
    if (!c)
        return false;
    raw_region_cache_end_view(c);
    if (!m || !m->initialized || !layout || !state || bytes != FFX_MAP_STATE_BYTES ||
        !mutations.epoch || !mutations.generation || !m->unit_count || m->unit_count > 1024U ||
        538U + 2U * m->unit_count > bytes || half(state + 32) != m->unit_count ||
        m->unit_pages != 64U || m->page_bytes != 2048U || m->data_pages != 63U ||
        layout->remap_count > 1024U || (layout->remap_count && !layout->remaps) ||
        ((layout->before_remap_bytes | layout->physical_base_bytes) & 131071U)) {
        ++c->stats.rejected;
        raw_region_cache_clear(c);
        return false;
    }
    geometry(m, current);
    if (!c->have_geometry || memcmp(c->geometry, current, sizeof(current)) ||
        !same_layout(c, layout))
        raw_region_cache_clear(c);
    memcpy(c->geometry, current, sizeof(current));
    c->layout = *layout;
    if (layout->remap_count)
        memcpy(c->remaps, layout->remaps, layout->remap_count * 4U);
    c->layout.remaps = c->remaps;
    c->have_geometry = true;
    c->mutations = mutations;
    c->view_epoch = mutations.epoch(mutations.context);
    c->view_state = state;
    if (!c->view_epoch) {
        ++c->stats.rejected;
        raw_region_cache_clear(c);
        return false;
    }
    c->view_active = true;
    for (unsigned i = 0; i < RAW_REGION_CACHE_SLOTS; ++i)
        if (c->entry[i].valid && !unchanged(c, &c->entry[i]))
            invalidate_entry(c, &c->entry[i]);
    return true;
}
static uint32_t age(RawRegionCache *c)
{
    if (c->age == UINT32_MAX) {
        for (unsigned i = 0; i < RAW_REGION_CACHE_SLOTS; ++i)
            c->entry[i].age = 0;
        c->age = 0;
    }
    return ++c->age;
}
bool raw_region_cache_store(RawRegionCache *c, const uint8_t *data, size_t bytes)
{
    uint16_t units[RAW_REGION_CACHE_UNITS], blocks[RAW_REGION_CACHE_UNITS];
    uint32_t generations[RAW_REGION_CACHE_UNITS];
    if (!c || !c->view_active || !data || bytes != FFX_MAP_REGION_BYTES)
        return false;
    uint32_t id = half(data + 4), epoch = c->mutations.epoch(c->mutations.context);
    if (!epoch || epoch != c->view_epoch || half(data) != 1U ||
        word(data + 352) != c->geometry[0] || id >= c->geometry[4])
        goto reject;
    unsigned count = members(c, id, units);
    if (!count || half(data + 6) != count)
        goto reject;
    for (unsigned i = 0; i < count; ++i) {
        unsigned matches = 0;
        for (unsigned j = 0; j < count; ++j)
            matches += half(data + 16U + 16U * j) == units[i];
        if (matches != 1U || !physical_block(c, units[i], &blocks[i]))
            goto reject;
        for (unsigned j = 0; j < i; ++j)
            if (blocks[j] == blocks[i])
                goto reject;
        generations[i] = c->mutations.generation(c->mutations.context, blocks[i]);
    }
    unsigned victim = 0;
    for (unsigned i = 0; i < RAW_REGION_CACHE_SLOTS; ++i) {
        if (c->entry[i].valid && c->entry[i].region == id) {
            victim = i;
            break;
        }
        if (!c->entry[i].valid ||
            (c->entry[victim].valid && c->entry[i].age < c->entry[victim].age))
            victim = i;
    }
    RawRegionCacheEntry *e = &c->entry[victim];
    e->valid = false;
    memmove(e->data, data, bytes);
    memcpy(e->units, units, count * sizeof(units[0]));
    memcpy(e->blocks, blocks, count * sizeof(blocks[0]));
    memcpy(e->generations, generations, count * sizeof(generations[0]));
    e->count = count;
    e->epoch = epoch;
    e->region = id;
    e->age = age(c);
    e->valid = true;
    if (!unchanged(c, e)) {
        invalidate_entry(c, e);
        goto reject;
    }
    ++c->stats.stores;
    return true;
reject:
    ++c->stats.rejected;
    return false;
}
const uint8_t *raw_region_cache_lookup(RawRegionCache *c, uint32_t region)
{
    if (!c || !c->view_active)
        return NULL;
    for (unsigned i = 0; i < RAW_REGION_CACHE_SLOTS; ++i) {
        RawRegionCacheEntry *e = &c->entry[i];
        if (!e->valid || e->region != region)
            continue;
        if (!unchanged(c, e)) {
            invalidate_entry(c, e);
            return NULL;
        }
        e->age = age(c);
        ++c->stats.hits;
        return e->data;
    }
    return NULL;
}
