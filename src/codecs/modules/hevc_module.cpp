#if defined(NDVIDEO_BUILD_HEVC_MODULE)
#include "../codec_module_api.h"
#include "hevc_module_api.h"
#include "hevc_module_runtime.h"
#include "module_runtime.h"

extern "C" {
extern HevcModuleInitializer __init_array_start[] __attribute__((weak));
extern HevcModuleInitializer __init_array_end[] __attribute__((weak));
extern HevcModuleInitializer __fini_array_start[] __attribute__((weak));
extern HevcModuleInitializer __fini_array_end[] __attribute__((weak));
void *__dso_handle __attribute__((weak)) = &__dso_handle;
}
/* GCC libstdc++ deliberately leaves its emergency exception arena to the
 * process. An unloadable module must release it explicitly while C thunks live. */
namespace __gnu_cxx {
void __freeres();
}

namespace {
const CodecHostApi *bound_host;
unsigned contexts;
unsigned busy;
bool ready;
bool retired;

hevc_decoder_t *module_create()
{
    if (!ready)
        return NULL;
    ++busy;
    hevc_decoder_t *decoder = hevc_create();
    if (decoder)
        ++contexts;
    --busy;
    return decoder;
}

hevc_decoder_t *module_create_with_memory(void *memory, size_t bytes)
{
    if (!ready)
        return NULL;
    ++busy;
    hevc_decoder_t *decoder = hevc_create_with_memory(memory, bytes);
    if (decoder)
        ++contexts;
    --busy;
    return decoder;
}

void module_destroy(hevc_decoder_t *decoder)
{
    if (!decoder || !ready)
        return;
    ++busy;
    hevc_destroy(decoder);
    --contexts;
    --busy;
}

bool module_shutdown()
{
    if (busy || contexts)
        return false;
    if (!ready)
        return true;
    ++busy;
    hevc_module_runtime_finish(__fini_array_start, __fini_array_end);
    __gnu_cxx::__freeres();
    module_runtime_shutdown();
    ready = false;
    retired = true;
    bound_host = NULL;
    --busy;
    return true;
}

const HevcModuleApi functions = {
    module_create, hevc_working_memory_size, module_create_with_memory,
    hevc_external_memory, module_destroy, hevc_reset, hevc_submit_annexb,
    hevc_step, hevc_step_until, hevc_get_frame, hevc_release_frame,
    hevc_flush, hevc_error_string, hevc_last_step_ctus, hevc_total_ctus,
    hevc_picture_ctus_done, hevc_picture_ctus_total, hevc_ctu_size
};
}

extern "C" int module_entry(const CodecHostApi *host, CodecModuleApi *out)
{
    if (!host || !out || host->abi_version != CODEC_MODULE_ABI_VERSION ||
        host->struct_size < sizeof(CodecHostApi) || !host->resolve || retired || busy)
        return -1;
    if (!ready) {
        ++busy;
        if (!module_runtime_bind(host)) {
            module_runtime_shutdown();
            --busy;
            return -2;
        }
        if (!hevc_module_runtime_start(__init_array_start, __init_array_end)) {
            hevc_module_runtime_finish(__fini_array_start, __fini_array_end);
            __gnu_cxx::__freeres();
            module_runtime_shutdown();
            retired = true;
            --busy;
            return -3;
        }
        ready = true;
        bound_host = host;
        --busy;
    } else if (bound_host != host) {
        return -4;
    }
    out->abi_version = CODEC_MODULE_ABI_VERSION;
    out->struct_size = sizeof(*out);
    out->codec_id = CODEC_MODULE_HEVC;
    out->codec_api_size = sizeof(functions);
    out->codec_api = &functions;
    out->shutdown = module_shutdown;
    return 0;
}
#endif
