#ifndef NDVIDEO_H264_MODULE_API_H
#define NDVIDEO_H264_MODULE_API_H

#include "../codec_module_api.h"
#include "../h264bsd/h264bsd_decoder.h"

enum {
    H264_MODULE_CLIP_BYTES = 1280,
    H264_MODULE_QPC_BYTES = 208,
    H264_MODULE_DEBLOCK_BYTES = 260
};

typedef struct H264ModuleApi {
    uint32_t storage_size, slice_size;
    uint32_t slice_mb_count_offset, slice_mb_address_offset;
    uint32_t pic_size_offset, slice_offset, budget_offset;
    uint32_t error_reason_offset, error_detail_offset, error_bit_offset;
    uint32_t clip_bytes, qpc_bytes, deblock_bytes;
    storage_t *(*alloc)(void);
    void (*free)(storage_t *);
    u32 (*init)(storage_t *, u32);
    void (*decoder_shutdown)(storage_t *);
    u32 (*decode)(storage_t *, u8 *, u32, u32, u32 *);
    void (*set_budget)(storage_t *, u32);
    u8 *(*next_output)(storage_t *, u32 *, u32 *, u32 *);
    u32 (*pic_width)(storage_t *);
    u32 (*pic_height)(storage_t *);
    void (*cropping)(storage_t *, u32 *, u32 *, u32 *, u32 *, u32 *);
    bool (*bind_sram)(void *, size_t, void *, size_t, void *, size_t);
    void (*sram_status)(bool *, bool *, bool *);
} H264ModuleApi;

/* Host only. Reserve in the original SRAM order without loading the decoder.
 * Blocks remain host-owned across module unload/reload. */
bool h264_module_reserve_sram(void);

/* Before resetting/replacing the host SRAM arena, with all codec leases gone. */
bool h264_module_forget_sram(void);

#endif
