#ifndef NDVIDEO_FLASHFX_REGION_H
#define NDVIDEO_FLASHFX_REGION_H
#include "flashfx_map.h"
typedef enum {
    FFX_REGION_MORE,
    FFX_REGION_NEED_PAGE,
    FFX_REGION_DONE,
    FFX_REGION_ERROR
} FfxRegionStatus;
typedef struct {
    uint32_t media_offset, column, bytes;
    uint64_t token;
} FfxRegionRequest;
typedef struct {
    uint32_t sequence;
    uint16_t unit;
} FfxRegionUnit;
typedef struct {
    const uint8_t *state;
    uint32_t owner, id, unit_count, cursor, count, slot, page, bitmap_slot, bitmap_page, apply;
    uint32_t tag_column;
    uint64_t serial;
    bool pending, has_bitmap;
    unsigned phase, error;
    FfxRegionRequest request;
    FfxRegionUnit units[21];
    uint8_t bitmap[128];
    uint8_t data[FFX_MAP_REGION_BYTES];
    FfxRegionStatus status;
} FfxRegionLoader;
/* One immutable mounted view. Drain the physical provider before reinitializing.
 * Source state is the verified 2688-byte FlashFX snapshot, including the inline
 * unit-to-region directory. No OS function or cache reconstruction is called. */
bool ffx_region_begin(FfxRegionLoader *, const uint8_t *state, size_t bytes, uint32_t owner,
                      uint32_t region);
bool ffx_region_begin_layout(FfxRegionLoader *, const uint8_t *state, size_t bytes, uint32_t owner,
                             uint32_t region, uint32_t tag_column);
FfxRegionStatus ffx_region_step(FfxRegionLoader *);
bool ffx_region_request(const FfxRegionLoader *, FfxRegionRequest *);
bool ffx_region_supply(FfxRegionLoader *, uint64_t token, const uint8_t *data, size_t bytes);
#endif
