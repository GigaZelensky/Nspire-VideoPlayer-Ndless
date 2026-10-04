#include "h264_module_api.h"
#include "../h264bsd/h264bsd_sram.h"
#include "../../sram.h"
#include <stddef.h>

#if defined(NDVIDEO_CODEC_MODULES) && NDVIDEO_CODEC_MODULES && NDVIDEO_WITH_H264
static const H264ModuleApi *active_api;
static void *clip_block, *qpc_block, *deblock_block;
static bool reserve_attempted, clip_ready, qpc_ready, deblock_ready;

bool h264_module_reserve_sram(void)
{
    if (!reserve_attempted && sram_is_enabled()) {
        reserve_attempted = true;
        clip_block = sram_alloc(H264_MODULE_CLIP_BYTES, 32);
        qpc_block = sram_alloc(H264_MODULE_QPC_BYTES, 32);
        deblock_block = sram_alloc(H264_MODULE_DEBLOCK_BYTES, 32);
    }
    return clip_block && qpc_block && deblock_block;
}

bool h264_module_forget_sram(void)
{
    if (codec_module_references(CODEC_MODULE_H264)) {
        return false;
    }
    /* The host must trim idle modules before replacing the arena. */
    if (!codec_modules_trim_idle()) {
        return false;
    }
    active_api = NULL;
    clip_block = qpc_block = deblock_block = NULL;
    reserve_attempted = clip_ready = qpc_ready = deblock_ready = false;
    return true;
}

static const H264ModuleApi *acquire_api(void)
{
    const CodecModuleApi *module = codec_module_acquire(CODEC_MODULE_H264);
    if (!module) {
        return NULL;
    }
    const H264ModuleApi *a = (const H264ModuleApi *)module->codec_api;
    if (module->abi_version != CODEC_MODULE_ABI_VERSION ||
        module->struct_size < sizeof(*module) ||
        module->codec_id != CODEC_MODULE_H264 || module->codec_api_size < sizeof(*a) || !a ||
        a->storage_size != sizeof(storage_t) || a->slice_size != sizeof(sliceStorage_t) ||
        a->slice_mb_count_offset != offsetof(sliceStorage_t, numDecodedMbs) ||
        a->slice_mb_address_offset != offsetof(sliceStorage_t, currMbAddr) ||
        a->pic_size_offset != offsetof(storage_t, picSizeInMbs) ||
        a->slice_offset != offsetof(storage_t, slice) ||
        a->budget_offset != offsetof(storage_t, macroblockBudget) ||
        a->error_reason_offset != offsetof(storage_t, errorReason) ||
        a->error_detail_offset != offsetof(storage_t, errorDetail) ||
        a->error_bit_offset != offsetof(storage_t, errorBit) ||
        a->clip_bytes != H264_MODULE_CLIP_BYTES || a->qpc_bytes != H264_MODULE_QPC_BYTES ||
        a->deblock_bytes != H264_MODULE_DEBLOCK_BYTES || !a->alloc || !a->free || !a->init ||
        !a->decoder_shutdown || !a->decode || !a->set_budget || !a->next_output ||
        !a->pic_width || !a->pic_height || !a->cropping || !a->bind_sram || !a->sram_status) {
        codec_module_release(CODEC_MODULE_H264);
        return NULL;
    }
    h264_module_reserve_sram();
    if (!a->bind_sram(clip_block, clip_block ? H264_MODULE_CLIP_BYTES : 0,
                      qpc_block, qpc_block ? H264_MODULE_QPC_BYTES : 0,
                      deblock_block, deblock_block ? H264_MODULE_DEBLOCK_BYTES : 0)) {
        codec_module_release(CODEC_MODULE_H264);
        return NULL;
    }
    a->sram_status(&clip_ready, &qpc_ready, &deblock_ready);
    return a;
}

bool h264bsdInitSramTables(void)
{
    const H264ModuleApi *a = acquire_api();
    if (!a) {
        return false;
    }
    bool okay = clip_ready && qpc_ready && deblock_ready;
    codec_module_release(CODEC_MODULE_H264);
    return okay;
}

void h264bsdGetSramStatus(bool *clip, bool *qpc, bool *deblock)
{
    /* Diagnostics must never cause a codec module to load. */
    if (clip) {
        *clip = clip_ready;
    }
    if (qpc) {
        *qpc = qpc_ready;
    }
    if (deblock) {
        *deblock = deblock_ready;
    }
}

storage_t *h264bsdAlloc(void)
{
    const H264ModuleApi *a = acquire_api();
    if (!a) {
        return NULL;
    }
    storage_t *p = a->alloc();
    if (!p) {
        codec_module_release(CODEC_MODULE_H264);
        return NULL;
    }
    active_api = a;
    return p;
}

void h264bsdFree(storage_t *p)
{
    if (!p || !active_api) {
        return;
    }
    active_api->free(p);
    codec_module_release(CODEC_MODULE_H264);
    if (!codec_module_references(CODEC_MODULE_H264)) {
        active_api = NULL;
    }
}

u32 h264bsdInit(storage_t *p, u32 reorder)
{
    return p && active_api ? active_api->init(p, reorder) : 1U;
}

void h264bsdShutdown(storage_t *p)
{
    if (p && active_api) {
        active_api->decoder_shutdown(p);
    }
}

u32 h264bsdDecode(storage_t *p, u8 *data, u32 bytes, u32 id, u32 *used)
{
    if (!p || !active_api) {
        if (used) {
            *used = 0;
        }
        return H264BSD_ERROR;
    }
    return active_api->decode(p, data, bytes, id, used);
}

void h264bsdSetMacroblockBudget(storage_t *p, u32 budget)
{
    if (p && active_api) {
        active_api->set_budget(p, budget);
    }
}

u8 *h264bsdNextOutputPicture(storage_t *p, u32 *id, u32 *idr, u32 *errors)
{
    return p && active_api ? active_api->next_output(p, id, idr, errors) : NULL;
}

u32 h264bsdPicWidth(storage_t *p)
{
    return p && active_api ? active_api->pic_width(p) : 0;
}

u32 h264bsdPicHeight(storage_t *p)
{
    return p && active_api ? active_api->pic_height(p) : 0;
}

void h264bsdCroppingParams(storage_t *p, u32 *flag, u32 *left, u32 *width,
                          u32 *top, u32 *height)
{
    if (p && active_api) {
        active_api->cropping(p, flag, left, width, top, height);
    } else {
        if (flag) {
            *flag = 0;
        }
        if (left) {
            *left = 0;
        }
        if (width) {
            *width = 0;
        }
        if (top) {
            *top = 0;
        }
        if (height) {
            *height = 0;
        }
    }
}
#endif
