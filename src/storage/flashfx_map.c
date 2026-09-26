#include "flashfx_map.h"
#include <string.h>

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (le16(p + 2) << 16);
}

bool ffx_map_init(FfxMap *map, const uint8_t *state, size_t state_bytes, uint32_t owner,
                  uint32_t blocks, const FfxMapRegion regions[FFX_MAP_SLOTS])
{
    if (!map)
        return false;
    memset(map, 0, sizeof(*map));
    if (!state || state_bytes != FFX_MAP_STATE_BYTES || !regions || !owner || !blocks)
        return false;
    uint32_t page = le16(state + 20), unit_pages = le16(state), data_pages = le16(state + 40);
    uint32_t region_bytes = le32(state + 24), capacity = le32(state + 4),
             region_count = le16(state + 536);
    if (page != 2048U || unit_pages != 64U || data_pages != 63U || !region_bytes ||
        region_bytes % page || region_bytes / page > 1024U || !region_count ||
        blocks > capacity / page || !le16(state + 32) || !(state[2632] & 1U))
        return false;
    uint32_t region_pages = region_bytes / page;
    if ((blocks - 1U) / region_pages >= region_count)
        return false;
    for (unsigned i = 0; i < FFX_MAP_SLOTS; ++i) {
        if (regions[i].data && regions[i].bytes != FFX_MAP_REGION_BYTES)
            return false;
        if (!regions[i].data && regions[i].bytes)
            return false;
        map->region[i] = regions[i];
    }
    map->owner = owner;
    map->logical_blocks = blocks;
    map->page_bytes = page;
    map->region_pages = region_pages;
    map->region_count = region_count;
    map->unit_pages = unit_pages;
    map->data_pages = data_pages;
    map->unit_bias = le16(state + 22);
    map->unit_count = le16(state + 32);
    map->initialized = true;
    return true;
}

FfxMapStatus ffx_map_page(const FfxMap *map, uint32_t block, FfxMapAddress *out)
{
    if (!out)
        return FFX_MAP_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!map || !map->initialized)
        return FFX_MAP_BAD_ARGUMENT;
    if (block >= map->logical_blocks)
        return FFX_MAP_BAD_RANGE;
    out->region_id = block / map->region_pages;
    out->page_index = block % map->region_pages;
    const uint8_t *region = NULL;
    for (unsigned i = 0; i < FFX_MAP_SLOTS; ++i) {
        const uint8_t *candidate = map->region[i].data;
        if (!candidate || !le16(candidate) || le32(candidate + 352) != map->owner ||
            le16(candidate + 4) != out->region_id)
            continue;
        if (region)
            return FFX_MAP_BAD_FORMAT; /* Ambiguous active mapping. */
        region = candidate;
    }
    if (!region)
        return FFX_MAP_NEED_REGION;
    const uint8_t *entry = region + 2416U + 6U * out->page_index;
    if (entry[0])
        return FFX_MAP_UNMAPPED;
    uint32_t slot = entry[1], page = le16(entry + 4);
    if (slot >= 21U || page >= map->data_pages || le16(region + 6) > 21U)
        return FFX_MAP_BAD_FORMAT;
    uint32_t unit = le16(region + 16U + 16U * slot);
    if (unit >= map->unit_count)
        return FFX_MAP_BAD_FORMAT;
    uint64_t offset = ((uint64_t)(unit + map->unit_bias) * map->unit_pages + map->unit_pages -
                       map->data_pages + page) *
                      map->page_bytes;
    if (offset > UINT32_MAX - map->page_bytes)
        return FFX_MAP_BAD_RANGE;
    out->media_unit = unit;
    out->media_page = page;
    out->media_byte_offset = (uint32_t)offset;
    out->bytes = map->page_bytes;
    return FFX_MAP_OK;
}
