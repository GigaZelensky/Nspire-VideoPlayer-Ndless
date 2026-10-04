#include "mpeg4_module_api.h"
#include "module_runtime.h"
#include "../xvid/utils/mem_align.h"

#if defined(NDVIDEO_BUILD_MPEG4_MODULE) && NDVIDEO_BUILD_MPEG4_MODULE
static unsigned live_contexts;
static bool initialized;
static void *bound_sram;
static unsigned bound_sram_size;

static bool module_global_init(void *base, unsigned size)
{
    if (initialized) {
        return base == bound_sram && size == bound_sram_size;
    }
    if (!mpeg4_xvid_global_init(base, size)) {
        return false;
    }
    bound_sram = base;
    bound_sram_size = size;
    initialized = true;
    return true;
}

static bool module_create(void **out, int width, int height)
{
    if (!initialized || !mpeg4_xvid_create(out, width, height)) {
        return false;
    }
    ++live_contexts;
    return true;
}

static void module_destroy(void *handle)
{
    if (!handle) {
        return;
    }
    mpeg4_xvid_destroy(handle);
    if (live_contexts) {
        --live_contexts;
    }
}

static bool module_shutdown(void)
{
    if (live_contexts || !xvid_module_release_globals()) {
        return false;
    }
    module_runtime_shutdown();
    return true;
}

static const Mpeg4ModuleApi api = {
    module_global_init, module_create, module_destroy,
    mpeg4_xvid_decode_frame, mpeg4_xvid_last_error
};

int module_entry(const CodecHostApi *host, CodecModuleApi *out)
{
    if (!out || !module_runtime_bind(host)) {
        return -1;
    }
    *out = (CodecModuleApi){CODEC_MODULE_ABI_VERSION, sizeof(*out),
        CODEC_MODULE_MPEG4, sizeof(api), &api, module_shutdown};
    return 0;
}
#endif
