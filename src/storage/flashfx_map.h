#ifndef NDVIDEO_FLASHFX_MAP_H
#define NDVIDEO_FLASHFX_MAP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FFX_MAP_REGION_BYTES 0x2170U
#define FFX_MAP_STATE_BYTES 2688U
#define FFX_MAP_SLOTS 3U
/* Independent resolver for the verified FlashFX media layout. No OS calls,
 * hardware accesses, allocation, polling or writes to the source snapshots.
 * Caller retains immutable state/regions for the entire view lifetime. After
 * OS writes/relocation, drain I/O and replace the view before another lookup.
 * Result offsets belong to the FlashFX media disk, NOT raw NAND rows. */
typedef struct {
    const uint8_t *data;
    size_t bytes;
} FfxMapRegion;
typedef struct {
    uint32_t owner, logical_blocks, page_bytes, region_pages, region_count;
    uint32_t unit_pages, data_pages, unit_bias, unit_count;
    FfxMapRegion region[FFX_MAP_SLOTS];
    bool initialized;
} FfxMap;
typedef enum {
    FFX_MAP_OK,
    FFX_MAP_NEED_REGION,
    FFX_MAP_UNMAPPED,
    FFX_MAP_BAD_ARGUMENT,
    FFX_MAP_BAD_FORMAT,
    FFX_MAP_BAD_RANGE
} FfxMapStatus;
typedef struct {
    uint32_t region_id, page_index, media_unit, media_page, media_byte_offset, bytes;
} FfxMapAddress;

/* Only the observed logical-I/O factor=1, disk shift=0, 2048-byte page layout
 * is accepted. Higher layers must validate those logical driver parameters. */
bool ffx_map_init(FfxMap *, const uint8_t *state, size_t state_bytes, uint32_t owner,
                  uint32_t logical_blocks, const FfxMapRegion regions[FFX_MAP_SLOTS]);
FfxMapStatus ffx_map_page(const FfxMap *, uint32_t logical_block, FfxMapAddress *);
#endif
