#ifndef NDVIDEO_RAW_REGION_CACHE_H
#define NDVIDEO_RAW_REGION_CACHE_H
#include "../storage/flashfx_map.h"
#include "../storage/nand_address.h"

#define RAW_REGION_CACHE_SLOTS 3U
#define RAW_REGION_CACHE_UNITS 21U
typedef struct {
    void *context;
    uint32_t (*epoch)(void *);
    uint32_t (*generation)(void *, uint32_t physical_block);
} RawRegionMutations;
typedef struct {
    uint32_t stores, hits, invalidations, rejected;
} RawRegionCacheStats;
typedef struct {
    uint8_t data[FFX_MAP_REGION_BYTES];
    uint16_t units[RAW_REGION_CACHE_UNITS], blocks[RAW_REGION_CACHE_UNITS];
    uint32_t generations[RAW_REGION_CACHE_UNITS], epoch, region, age;
    unsigned count;
    bool valid;
} RawRegionCacheEntry;
typedef struct {
    RawRegionCacheEntry entry[RAW_REGION_CACHE_SLOTS];
    uint32_t geometry[10], age, view_epoch;
    NandAddressMap layout;
    uint8_t remaps[4096];
    const uint8_t *view_state;
    RawRegionMutations mutations;
    RawRegionCacheStats stats;
    bool have_geometry, view_active;
} RawRegionCache;

/* Only the platform's serialized, immutable storage view may call begin,
 * store or lookup. Copies own their bytes and outlive the reader snapshot.
 * Epoch zero disables reuse. Unaligned lower layouts retain ordinary reads. */
bool raw_region_cache_begin_view(RawRegionCache *, const FfxMap *, const NandAddressMap *,
                                 const uint8_t *state, size_t bytes, RawRegionMutations);
void raw_region_cache_end_view(RawRegionCache *);
void raw_region_cache_clear(RawRegionCache *);
/* Store only while retiring a quiescent view, after returned pointers are no
 * longer used. Accept only a COMPLETE independently reconstructed mapping;
 * native borrowed regions and partial loader/copy buffers are ineligible. */
bool raw_region_cache_store(RawRegionCache *, const uint8_t *region, size_t bytes);
const uint8_t *raw_region_cache_lookup(RawRegionCache *, uint32_t region);
#endif
