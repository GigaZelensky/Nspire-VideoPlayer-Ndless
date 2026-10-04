#include "h264_module_api.h"
#include "module_runtime.h"
#include "../h264bsd/h264bsd_sram.h"
#include <stddef.h>

#if defined(NDVIDEO_BUILD_H264_MODULE) && NDVIDEO_BUILD_H264_MODULE
extern size_t h264bsdClipTableBytes(void);
extern size_t h264bsdQpCTableBytes(void);
extern size_t h264bsdDeblockingTableBytes(void);
extern bool h264bsdBindSramTables(void *, size_t, void *, size_t, void *, size_t);

static unsigned live_contexts;
static void *bound_clip, *bound_qpc, *bound_deblock;
static bool have_binding;

static storage_t *module_alloc(void)
{
    storage_t *p = h264bsdAlloc();
    if (p) {
        ++live_contexts;
    }
    return p;
}

static void module_free(storage_t *p)
{
    if (!p) {
        return;
    }
    h264bsdFree(p);
    if (live_contexts) {
        --live_contexts;
    }
}

static bool module_bind_sram(void *clip, size_t clip_bytes, void *qpc, size_t qpc_bytes,
                             void *deblock, size_t deblock_bytes)
{
    if (live_contexts && (!have_binding || clip != bound_clip ||
                         qpc != bound_qpc || deblock != bound_deblock)) {
        return false;
    }
    if (!h264bsdBindSramTables(clip, clip_bytes, qpc, qpc_bytes, deblock, deblock_bytes)) {
        return false;
    }
    bound_clip = clip;
    bound_qpc = qpc;
    bound_deblock = deblock;
    have_binding = true;
    return true;
}

static bool module_shutdown(void)
{
    if (live_contexts) {
        return false;
    }
    (void)h264bsdBindSramTables(NULL, 0, NULL, 0, NULL, 0);
    have_binding = false;
    bound_clip = bound_qpc = bound_deblock = NULL;
    module_runtime_shutdown();
    return true;
}

static const H264ModuleApi api = {
    sizeof(storage_t), sizeof(sliceStorage_t),
    offsetof(sliceStorage_t, numDecodedMbs), offsetof(sliceStorage_t, currMbAddr),
    offsetof(storage_t, picSizeInMbs), offsetof(storage_t, slice),
    offsetof(storage_t, macroblockBudget),
    offsetof(storage_t, errorReason), offsetof(storage_t, errorDetail),
    offsetof(storage_t, errorBit),
    H264_MODULE_CLIP_BYTES, H264_MODULE_QPC_BYTES, H264_MODULE_DEBLOCK_BYTES,
    module_alloc, module_free, h264bsdInit, h264bsdShutdown, h264bsdDecode,
    h264bsdSetMacroblockBudget, h264bsdNextOutputPicture, h264bsdPicWidth,
    h264bsdPicHeight, h264bsdCroppingParams, module_bind_sram, h264bsdGetSramStatus
};

int module_entry(const CodecHostApi *host, CodecModuleApi *out)
{
    if (!out || !module_runtime_bind(host)) {
        return -1;
    }
    if (h264bsdClipTableBytes() != H264_MODULE_CLIP_BYTES ||
        h264bsdQpCTableBytes() != H264_MODULE_QPC_BYTES ||
        h264bsdDeblockingTableBytes() != H264_MODULE_DEBLOCK_BYTES) {
        module_runtime_shutdown();
        return -1;
    }
    *out = (CodecModuleApi){CODEC_MODULE_ABI_VERSION, sizeof(*out),
        CODEC_MODULE_H264, sizeof(api), &api, module_shutdown};
    return 0;
}
#endif
