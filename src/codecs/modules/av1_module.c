#if defined(NDVIDEO_BUILD_AV1_MODULE)
#include "av1_module_api.h"
#include "module_runtime.h"
#include "../av1/dav1d/src/ndless_scratch.h"
static unsigned live_contexts;
static bool initialized, retired;
static av1_decoder_t *create(void)
{
    av1_decoder_t *d = av1_create();
    if (d)
        ++live_contexts;
    return d;
}

static av1_decoder_t *create_with_memory(void *p, size_t n)
{
    av1_decoder_t *d = av1_create_with_memory(p, n);
    if (d)
        ++live_contexts;
    return d;
}

static void destroy(av1_decoder_t *d)
{
    if (d) {
        av1_destroy(d);
        if (live_contexts)
            --live_contexts;
    }
}

static bool shutdown(void)
{
    if (live_contexts || dav1d_ndless_scratch)
        return false;
    if (initialized)
        module_runtime_shutdown();
    initialized = false;
    retired = true;
    return true;
}
static const Av1ModuleApi api = {create,
                                 av1_working_memory_size,
                                 create_with_memory,
                                 av1_external_memory,
                                 destroy,
                                 av1_reset,
                                 av1_submit_obus,
                                 av1_step,
                                 av1_step_until,
                                 av1_get_frame,
                                 av1_frame_repeats_previous,
                                 av1_frame_serial,
                                 av1_release_frame,
                                 av1_error_string,
                                 av1_last_step_blocks,
                                 av1_picture_blocks_done,
                                 av1_picture_blocks_total,
                                 av1_block_size,
                                 av1_packet_is_independent};
int module_entry(const CodecHostApi *host, CodecModuleApi *out)
{
    if (!out || retired)
        return -1;
    if (!initialized) {
        if (!module_runtime_bind(host))
            return -1;
        initialized = true;
    }
    out->abi_version = CODEC_MODULE_ABI_VERSION;
    out->struct_size = sizeof(*out);
    out->codec_id = CODEC_MODULE_AV1;
    out->codec_api_size = sizeof(api);
    out->codec_api = &api;
    out->shutdown = shutdown;
    return 0;
}
#endif
