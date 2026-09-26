#include "flashfx_region.h"
#include "flashfx_metadata.h"
#include <string.h>
enum { SCAN, HEADER, TAGS, BITMAP, APPLY };
static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static void put16(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}
static void put32(uint8_t *p, uint32_t n)
{
    put16(p, n);
    put16(p + 2, n >> 16);
}
static bool fail(FfxRegionLoader *r, unsigned error)
{
    r->error = error;
    r->pending = false;
    r->status = FFX_REGION_ERROR;
    return false;
}
static FfxRegionStatus request(FfxRegionLoader *r, unsigned unit, unsigned page, unsigned column,
                               unsigned bytes)
{
    uint64_t offset = ((uint64_t)unit + le16(r->state + 22)) * 131072U + (uint64_t)page * 2048U;
    if (offset > UINT32_MAX || r->serial == UINT64_MAX) {
        fail(r, 2);
        return r->status;
    }
    r->request = (FfxRegionRequest){(uint32_t)offset, column, bytes, ++r->serial};
    r->pending = true;
    r->status = FFX_REGION_NEED_PAGE;
    return r->status;
}
static FfxRegionStatus done(FfxRegionLoader *r)
{
    put16(r->data, 1);
    r->status = FFX_REGION_DONE;
    return r->status;
}
bool ffx_region_begin(FfxRegionLoader *r, const uint8_t *state, size_t bytes, uint32_t owner,
                      uint32_t id)
{
    return ffx_region_begin_layout(r, state, bytes, owner, id, 2052U);
}
bool ffx_region_begin_layout(FfxRegionLoader *r, const uint8_t *state, size_t bytes, uint32_t owner,
                             uint32_t id, uint32_t tag_column)
{
    if (!r)
        return false;
    memset(r, 0, sizeof(*r));
    r->status = FFX_REGION_ERROR;
    if ((tag_column != 2052U && tag_column != 2060U) || !state || bytes != FFX_MAP_STATE_BYTES ||
        !owner || id >= le16(state + 536) || le16(state) != 64 || le16(state + 20) != 2048 ||
        le16(state + 40) != 63 || le16(state + 32) > 1024 || 538U + 2U * le16(state + 32) > bytes)
        return false;
    r->state = state;
    r->owner = owner;
    r->id = id;
    r->tag_column = tag_column;
    r->unit_count = le16(state + 32);
    r->phase = SCAN;
    memset(r->data + 2416, 255, 6144);
    put16(r->data + 4, id);
    put32(r->data + 352, owner);
    r->status = FFX_REGION_MORE;
    return true;
}
FfxRegionStatus ffx_region_step(FfxRegionLoader *r)
{
    if (!r)
        return FFX_REGION_ERROR;
    if (r->pending || r->status == FFX_REGION_ERROR || r->status == FFX_REGION_DONE)
        return r->status;
    if (r->phase == SCAN) {
        for (unsigned n = 0; n < 64U && r->cursor < r->unit_count; ++n) {
            unsigned unit = r->cursor++;
            if (le16(r->state + 538U + 2U * unit) == r->id) {
                if (r->count == 21) {
                    fail(r, 3);
                    return r->status;
                }
                r->phase = HEADER;
                return request(r, unit, 0, 0, 2112);
            }
        }
        if (r->cursor < r->unit_count)
            return FFX_REGION_MORE;
        if (!r->count) {
            fail(r, 4);
            return r->status;
        }
        put16(r->data + 6, r->count);
        for (unsigned i = 0; i < r->count; ++i) {
            put32(r->data + 12U + 16U * i, r->units[i].sequence);
            put16(r->data + 16U + 16U * i, r->units[i].unit);
        }
        r->phase = TAGS;
        return FFX_REGION_MORE;
    }
    if (r->phase == TAGS) {
        if (r->slot < r->count)
            return request(r, r->units[r->slot].unit, r->page + 1U, r->tag_column, 4U);
        if (!r->has_bitmap)
            return done(r);
        r->phase = BITMAP;
        return request(r, r->units[r->bitmap_slot].unit, r->bitmap_page + 1U, 0, 2048);
    }
    if (r->phase == APPLY) {
        for (unsigned n = 0; n < 64U && r->apply < 1024U; ++n, ++r->apply) {
            unsigned i = r->apply;
            uint8_t *entry = r->data + 2416U + 6U * i;
            if ((r->bitmap[i >> 3] & (1U << (i & 7U))) && entry[0] == 0) {
                uint32_t sequence = r->units[entry[1]].sequence,
                         marker = r->units[r->bitmap_slot].sequence;
                if (sequence < marker || (sequence == marker && le16(entry + 4) <= r->bitmap_page))
                    entry[0] = 1;
            }
        }
        if (r->apply == 1024U)
            return done(r);
        return FFX_REGION_MORE;
    }
    fail(r, 5);
    return r->status;
}
bool ffx_region_request(const FfxRegionLoader *r, FfxRegionRequest *out)
{
    if (!r || !out || !r->pending)
        return false;
    *out = r->request;
    return true;
}
bool ffx_region_supply(FfxRegionLoader *r, uint64_t token, const uint8_t *data, size_t bytes)
{
    if (!r || !r->pending || token != r->request.token)
        return false;
    if (!data || bytes != r->request.bytes)
        return fail(r, 6);
    r->pending = false;
    r->status = FFX_REGION_MORE;
    if (r->phase == HEADER) {
        uint32_t sequence;
        if (!ffx_decode_unit_header_layout(data, bytes, &sequence, r->tag_column))
            return fail(r, 7);
        unsigned unit = r->cursor - 1U, i = r->count++;
        while (i && r->units[i - 1U].sequence > sequence) {
            r->units[i] = r->units[i - 1U];
            --i;
        }
        r->units[i] = (FfxRegionUnit){sequence, (uint16_t)unit};
        r->phase = SCAN;
        return true;
    }
    if (r->phase == TAGS) {
        uint16_t tag;
        if (ffx_decode_tag(data, &tag) && (tag >> 14) == 1U) {
            unsigned index = tag & 0x3fffU;
            if ((index & 0xfff0U) == 0x1ff0U) {
                if (!(index & 15U)) {
                    r->has_bitmap = true;
                    r->bitmap_slot = r->slot;
                    r->bitmap_page = r->page;
                }
            } else {
                if (index >= 1024U)
                    return fail(r, 8);
                uint8_t *entry = r->data + 2416U + 6U * index;
                entry[0] = 0;
                entry[1] = (uint8_t)r->slot;
                put16(entry + 2, index);
                put16(entry + 4, r->page);
            }
        }
        if (++r->page == 63U) {
            r->page = 0;
            ++r->slot;
        }
        return true;
    }
    if (r->phase == BITMAP) {
        memcpy(r->bitmap, data, sizeof(r->bitmap));
        r->phase = APPLY;
        return true;
    }
    return fail(r, 5);
}
