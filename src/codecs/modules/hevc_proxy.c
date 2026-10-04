#if defined(NDVIDEO_CODEC_MODULES) && NDVIDEO_CODEC_MODULES && !defined(NDVIDEO_BUILD_MODULE)
#include "../codec_module_api.h"
#include "hevc_module_api.h"

/* Cached only while contexts hold host leases. Clear before the last release
 * so a later eviction/reload can never leave a stale table in resident code. */
static const HevcModuleApi *live_api;
static unsigned live_contexts;

static const HevcModuleApi *acquire_api(void)
{
    const CodecModuleApi *module = codec_module_acquire(CODEC_MODULE_HEVC);
    if (!module)
        return NULL;
    if (module->abi_version != CODEC_MODULE_ABI_VERSION ||
        module->struct_size < sizeof(CodecModuleApi) ||
        module->codec_id != CODEC_MODULE_HEVC || !module->codec_api ||
        module->codec_api_size != sizeof(HevcModuleApi)) {
        codec_module_release(CODEC_MODULE_HEVC);
        return NULL;
    }
    const HevcModuleApi *api = (const HevcModuleApi *)module->codec_api;
    if (!api->create || !api->working_memory_size || !api->create_with_memory ||
        !api->external_memory || !api->destroy || !api->reset || !api->submit_annexb ||
        !api->step || !api->step_until || !api->get_frame || !api->release_frame ||
        !api->flush || !api->error_string || !api->last_step_ctus || !api->total_ctus ||
        !api->picture_ctus_done || !api->picture_ctus_total || !api->ctu_size) {
        codec_module_release(CODEC_MODULE_HEVC);
        return NULL;
    }
    return api;
}

hevc_decoder_t *hevc_create(void)
{
    const HevcModuleApi *api = acquire_api();
    if (!api)
        return NULL;
    hevc_decoder_t *decoder = api->create();
    if (!decoder) {
        codec_module_release(CODEC_MODULE_HEVC);
    } else {
        live_api = api;
        ++live_contexts;
    }
    return decoder;
}

hevc_decoder_t *hevc_create_with_memory(void *memory, size_t bytes)
{
    const HevcModuleApi *api = acquire_api();
    if (!api)
        return NULL;
    hevc_decoder_t *decoder = api->create_with_memory(memory, bytes);
    if (!decoder) {
        codec_module_release(CODEC_MODULE_HEVC);
    } else {
        live_api = api;
        ++live_contexts;
    }
    return decoder;
}

size_t hevc_working_memory_size(void)
{
    const HevcModuleApi *api = acquire_api();
    if (!api)
        return 0;
    size_t result = api->working_memory_size();
    codec_module_release(CODEC_MODULE_HEVC);
    return result;
}

void hevc_destroy(hevc_decoder_t *decoder)
{
    if (!decoder || !live_api)
        return;
    live_api->destroy(decoder);
    if (!--live_contexts)
        live_api = NULL;
    codec_module_release(CODEC_MODULE_HEVC);
}

const void *hevc_external_memory(const hevc_decoder_t *decoder)
{
    return live_api ? live_api->external_memory(decoder) : NULL;
}

void hevc_reset(hevc_decoder_t *decoder)
{
    if (live_api)
        live_api->reset(decoder);
}

hevc_status_t hevc_submit_annexb(hevc_decoder_t *decoder, const void *data, size_t bytes,
                               int64_t pts)
{
    return live_api ? live_api->submit_annexb(decoder, data, bytes, pts) : HEVC_ERROR;
}

hevc_status_t hevc_step(hevc_decoder_t *decoder, unsigned budget)
{
    return live_api ? live_api->step(decoder, budget) : HEVC_ERROR;
}

hevc_status_t hevc_step_until(hevc_decoder_t *decoder, unsigned budget, uint64_t deadline,
                             hevc_clock_fn clock)
{
    return live_api ? live_api->step_until(decoder, budget, deadline, clock) : HEVC_ERROR;
}

const hevc_frame_t *hevc_get_frame(const hevc_decoder_t *decoder)
{
    return live_api ? live_api->get_frame(decoder) : NULL;
}

void hevc_release_frame(hevc_decoder_t *decoder)
{
    if (live_api)
        live_api->release_frame(decoder);
}

hevc_status_t hevc_flush(hevc_decoder_t *decoder)
{
    return live_api ? live_api->flush(decoder) : HEVC_ERROR;
}

const char *hevc_error_string(const hevc_decoder_t *decoder)
{
    if (decoder && live_api)
        return live_api->error_string(decoder);
    const char *load_error = codec_module_error();
    return load_error && *load_error ? load_error : "HEVC decoder allocation failed";
}

unsigned hevc_last_step_ctus(const hevc_decoder_t *decoder)
{
    return live_api ? live_api->last_step_ctus(decoder) : 0;
}

uint64_t hevc_total_ctus(const hevc_decoder_t *decoder)
{
    return live_api ? live_api->total_ctus(decoder) : 0;
}

unsigned hevc_picture_ctus_done(const hevc_decoder_t *decoder)
{
    return live_api ? live_api->picture_ctus_done(decoder) : 0;
}

unsigned hevc_picture_ctus_total(const hevc_decoder_t *decoder)
{
    return live_api ? live_api->picture_ctus_total(decoder) : 0;
}

unsigned hevc_ctu_size(const hevc_decoder_t *decoder)
{
    return live_api ? live_api->ctu_size(decoder) : 0;
}
#endif
