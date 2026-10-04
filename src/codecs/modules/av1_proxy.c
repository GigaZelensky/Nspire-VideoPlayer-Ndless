#if NDVIDEO_CODEC_MODULES && NDVIDEO_WITH_AV1 && !defined(NDVIDEO_BUILD_MODULE)
#include "av1_module_api.h"
#include "../codec_module_api.h"
static const Av1ModuleApi *live_api;
static unsigned live_contexts;
static const Av1ModuleApi *acquire_api(void)
{
    const CodecModuleApi *m = codec_module_acquire(CODEC_MODULE_AV1);
    if (!m)
        return NULL;
    const Av1ModuleApi *a = (const Av1ModuleApi *)m->codec_api;
    if (m->abi_version != CODEC_MODULE_ABI_VERSION || m->struct_size < sizeof(*m) ||
        m->codec_id != CODEC_MODULE_AV1 || m->codec_api_size != sizeof(*a) || !a || !a->create ||
        !a->working_memory_size || !a->create_with_memory || !a->external_memory || !a->destroy ||
        !a->reset || !a->submit_obus || !a->step || !a->step_until || !a->get_frame ||
        !a->frame_repeats_previous || !a->frame_serial || !a->release_frame || !a->error_string ||
        !a->last_step_blocks || !a->picture_blocks_done || !a->picture_blocks_total ||
        !a->block_size || !a->packet_is_independent) {
        codec_module_release(CODEC_MODULE_AV1);
        return NULL;
    }
    return a;
}

av1_decoder_t *av1_create(void)
{
    const Av1ModuleApi *a = acquire_api();
    if (!a)
        return NULL;
    av1_decoder_t *d = a->create();
    if (d) {
        live_api = a;
        live_contexts++;
    } else
        codec_module_release(CODEC_MODULE_AV1);
    return d;
}

av1_decoder_t *av1_create_with_memory(void *p, size_t n)
{
    const Av1ModuleApi *a = acquire_api();
    if (!a)
        return NULL;
    av1_decoder_t *d = a->create_with_memory(p, n);
    if (d) {
        live_api = a;
        live_contexts++;
    } else
        codec_module_release(CODEC_MODULE_AV1);
    return d;
}

size_t av1_working_memory_size(void)
{
    const Av1ModuleApi *a = acquire_api();
    if (!a)
        return 0;
    size_t n = a->working_memory_size();
    codec_module_release(CODEC_MODULE_AV1);
    return n;
}

void av1_destroy(av1_decoder_t *d)
{
    if (!d || !live_api)
        return;
    live_api->destroy(d);
    if (!--live_contexts)
        live_api = NULL;
    codec_module_release(CODEC_MODULE_AV1);
}

const void *av1_external_memory(const av1_decoder_t *d)
{
    return live_api ? live_api->external_memory(d) : NULL;
}

void av1_reset(av1_decoder_t *d)
{
    if (live_api)
        live_api->reset(d);
}

av1_status_t av1_submit_obus(av1_decoder_t *d, const void *p, size_t n, int64_t pts)
{
    return live_api ? live_api->submit_obus(d, p, n, pts) : AV1_ERROR;
}

av1_status_t av1_step(av1_decoder_t *d, unsigned b)
{
    return live_api ? live_api->step(d, b) : AV1_ERROR;
}

av1_status_t av1_step_until(av1_decoder_t *d, unsigned b, uint64_t t, av1_clock_fn c)
{
    return live_api ? live_api->step_until(d, b, t, c) : AV1_ERROR;
}

const av1_frame_t *av1_get_frame(const av1_decoder_t *d)
{
    return live_api ? live_api->get_frame(d) : NULL;
}

bool av1_frame_repeats_previous(const av1_decoder_t *d)
{
    return live_api && live_api->frame_repeats_previous(d);
}

uint64_t av1_frame_serial(const av1_decoder_t *d)
{
    return live_api ? live_api->frame_serial(d) : 0;
}

void av1_release_frame(av1_decoder_t *d)
{
    if (live_api)
        live_api->release_frame(d);
}

const char *av1_error_string(const av1_decoder_t *d)
{
    if (d && live_api)
        return live_api->error_string(d);
    const char *error = codec_module_error();
    return error[0] ? error : "AV1 decoder allocation failed";
}

unsigned av1_last_step_blocks(const av1_decoder_t *d)
{
    return live_api ? live_api->last_step_blocks(d) : 0;
}

unsigned av1_picture_blocks_done(const av1_decoder_t *d)
{
    return live_api ? live_api->picture_blocks_done(d) : 0;
}

unsigned av1_picture_blocks_total(const av1_decoder_t *d)
{
    return live_api ? live_api->picture_blocks_total(d) : 0;
}

unsigned av1_block_size(const av1_decoder_t *d)
{
    return live_api ? live_api->block_size(d) : 64;
}

bool av1_packet_is_independent(const void *p, size_t n)
{
    const Av1ModuleApi *a = acquire_api();
    if (!a)
        return false;
    bool yes = a->packet_is_independent(p, n);
    codec_module_release(CODEC_MODULE_AV1);
    return yes;
}
#endif
