#ifndef NDVIDEO_RAW_FILE_READER_H
#define NDVIDEO_RAW_FILE_READER_H
#include "reliance_reader.h"
#include "flashfx_region.h"
#include "nand_address.h"
#include "nand_page_reader.h"
typedef enum {
    RAW_FILE_IDLE,
    RAW_FILE_PENDING,
    RAW_FILE_DONE,
    RAW_FILE_CANCELED,
    RAW_FILE_ERROR
} RawFileStatus;
typedef struct {
    uint32_t block;
    const uint8_t *data;
} RawFileOverlay;
/* Diagnostic counters only: increment on a successful SPI request start,
 * never on readiness polls. Bounded histograms accept larger region IDs via
 * an overflow bucket without restricting the reader's validated geometry. */
enum {
    RAW_READ_PAYLOAD,
    RAW_READ_REL_METADATA,
    RAW_READ_FFX_HEADER,
    RAW_READ_FFX_TAG,
    RAW_READ_FFX_BITMAP,
    RAW_READ_KIND_COUNT
};
#define RAW_FILE_REGION_HISTOGRAM_SLOTS 64U
typedef struct {
    uint32_t physical[RAW_READ_KIND_COUNT], regions_started;
    uint32_t region[RAW_FILE_REGION_HISTOGRAM_SLOTS], region_other;
} RawFileCosts;
static inline void raw_file_costs_add(RawFileCosts *to, const RawFileCosts *from)
{
    for (unsigned i = 0; i < RAW_READ_KIND_COUNT; ++i)
        to->physical[i] += from->physical[i];
    to->regions_started += from->regions_started;
    for (unsigned i = 0; i < RAW_FILE_REGION_HISTOGRAM_SLOTS; ++i)
        to->region[i] += from->region[i];
    to->region_other += from->region_other;
}
typedef struct {
    RelReader file;
    FfxMap map;
    FfxRegionLoader loader;
    NandAddressMap layout;
    NandAddressJob address;
    NandPageReader nand;
    uint8_t state[FFX_MAP_STATE_BYTES], cache[3][FFX_MAP_REGION_BYTES];
    uint8_t workspace[REL_CACHE_SLOTS * 2048], io[2112];
    const RawFileOverlay *overlay;
    uint32_t overlay_count;
    const RawFileOverlay *metadata_overlay;
    uint32_t metadata_overlay_count;
    uint32_t ages[3], age, copy_slot, copy_position, media_offset, column, bytes;
    uint32_t page_reads, regions_loaded, overlay_reads, metadata_reads;
    RawFileCosts costs;
    uint64_t token;
    unsigned physical, source, error;
    bool loading, copying, canceled, initialized;
    RawFileStatus status;
} RawFileReader;
/* Immutable mounted view captured by the platform adapter. Overlay only
 * contains dirty native-cache blocks needed to preserve that view; clean file
 * data must use flash. Drain the provider before discarding/reinitializing. */
bool raw_file_init(RawFileReader *, const uint8_t *, size_t, uint32_t owner, uint32_t blocks,
                   uint32_t index_block, uint32_t inode, NandPageConfig, NandAddressMap,
                   const RawFileOverlay *, uint32_t overlay_count);
bool raw_file_begin(RawFileReader *, uint64_t offset, void *, uint32_t bytes);
/* Optional borrowed clean metadata from the SAME immutable mounted view.
 * Never serves ordinary video data; reset on every native-storage handoff. */
bool raw_file_metadata_cache(RawFileReader *, const RawFileOverlay *, uint32_t count);
void raw_file_cancel(RawFileReader *);
RawFileStatus raw_file_step(RawFileReader *, uint32_t now, uint32_t timeout);
#endif
