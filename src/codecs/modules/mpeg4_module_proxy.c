#include "mpeg4_module_api.h"
#include <stdio.h>

#if defined(NDVIDEO_CODEC_MODULES) && NDVIDEO_CODEC_MODULES && NDVIDEO_WITH_MPEG4
static const Mpeg4ModuleApi *active_api;
static void *saved_sram;
static unsigned saved_sram_size;
static char saved_error[160];

static void save_error(const char *message)
{
    snprintf(saved_error, sizeof(saved_error), "%s", message ? message : "MPEG-4 module unavailable");
}

static const Mpeg4ModuleApi *acquire_api(void)
{
    const CodecModuleApi *module = codec_module_acquire(CODEC_MODULE_MPEG4);
    if (!module) {
        save_error(codec_module_error());
        return NULL;
    }
    const Mpeg4ModuleApi *api = (const Mpeg4ModuleApi *)module->codec_api;
    if (module->abi_version != CODEC_MODULE_ABI_VERSION || module->struct_size < sizeof(*module) ||
        module->codec_id != CODEC_MODULE_MPEG4 || module->codec_api_size < sizeof(*api) ||
        !api || !api->global_init || !api->create || !api->destroy || !api->decode || !api->last_error) {
        save_error("MPEG-4 module ABI mismatch");
        codec_module_release(CODEC_MODULE_MPEG4);
        return NULL;
    }
    /* Replayed on every acquisition: the loader may have replaced the image. */
    if (!api->global_init(saved_sram, saved_sram_size)) {
        save_error(api->last_error());
        codec_module_release(CODEC_MODULE_MPEG4);
        /* Failed initialization may own only part of the global tables.
         * Unload the unpinned image so a retry starts with fresh static state. */
        codec_modules_trim_idle();
        return NULL;
    }
    return api;
}

bool mpeg4_xvid_global_init(void *base, unsigned size)
{
    if (base != saved_sram || size != saved_sram_size) {
        if (codec_module_references(CODEC_MODULE_MPEG4) || !codec_modules_trim_idle()) {
            save_error("MPEG-4 SRAM changed while module is in use");
            return false;
        }
        saved_sram = base;
        saved_sram_size = size;
    }
    const Mpeg4ModuleApi *api = acquire_api();
    if (!api) {
        return false;
    }
    codec_module_release(CODEC_MODULE_MPEG4);
    saved_error[0] = '\0';
    return true;
}

bool mpeg4_module_forget_sram(void)
{
    if (codec_module_references(CODEC_MODULE_MPEG4) || !codec_modules_trim_idle()) {
        return false;
    }
    active_api = NULL;
    saved_sram = NULL;
    saved_sram_size = 0;
    return true;
}

bool mpeg4_xvid_create(void **out, int width, int height)
{
    if (!out) {
        save_error("MPEG-4 output handle missing");
        return false;
    }
    *out = NULL;
    const Mpeg4ModuleApi *api = acquire_api();
    if (!api) {
        return false;
    }
    if (!api->create(out, width, height)) {
        save_error(api->last_error());
        codec_module_release(CODEC_MODULE_MPEG4);
        return false;
    }
    active_api = api;
    saved_error[0] = '\0';
    return true;
}

void mpeg4_xvid_destroy(void *handle)
{
    if (!handle || !active_api) {
        return;
    }
    active_api->destroy(handle);
    codec_module_release(CODEC_MODULE_MPEG4);
    if (!codec_module_references(CODEC_MODULE_MPEG4)) {
        active_api = NULL;
    }
}

bool mpeg4_xvid_reset(void **handle, int width, int height)
{
    if (!handle) {
        save_error("MPEG-4 reset handle missing");
        return false;
    }
    mpeg4_xvid_destroy(*handle);
    *handle = NULL;
    return mpeg4_xvid_create(handle, width, height);
}

bool mpeg4_xvid_decode_frame(void *handle, const uint8_t *data, size_t bytes,
                            uint16_t *rgb, int width, int height,
                            bool output, bool discontinuity)
{
    if (!handle || !active_api) {
        save_error("MPEG-4 decoder missing");
        return false;
    }
    if (!active_api->decode(handle, data, bytes, rgb, width, height, output, discontinuity)) {
        save_error(active_api->last_error());
        return false;
    }
    saved_error[0] = '\0';
    return true;
}

const char *mpeg4_xvid_last_error(void)
{
    /* Never return a pointer into an image which the idle cache may unload. */
    return saved_error[0] ? saved_error : "xvid error";
}
#endif
