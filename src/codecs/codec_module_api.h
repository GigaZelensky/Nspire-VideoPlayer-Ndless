#ifndef NDVIDEO_CODEC_MODULE_API_H
#define NDVIDEO_CODEC_MODULE_API_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CODEC_MODULE_ABI_VERSION 1U
#define CODEC_MODULE_H264 0U
#define CODEC_MODULE_MPEG4 1U
#define CODEC_MODULE_HEVC 2U
#define CODEC_MODULE_AV1 3U
#define CODEC_MODULE_COUNT 4U

/* Modules use local runtime thunks and request only approved host C symbols.
 * Function symbols return their callable address; data symbols return the
 * address of the object (e.g. &_impure_ptr), not its current value. */
typedef struct CodecHostApi {
    uint32_t abi_version, struct_size;
    void *(*resolve)(const char *name);
    void (*log)(const char *message);
} CodecHostApi;

typedef struct CodecModuleApi {
    uint32_t abi_version, struct_size, codec_id, codec_api_size;
    const void *codec_api;
    /* Called with no host leases; false refuses unload. No host exit handlers. */
    bool (*shutdown)(void);
} CodecModuleApi;
typedef int (*CodecModuleEntry)(const CodecHostApi *host, CodecModuleApi *out);
typedef bool (*CodecModuleProgress)(void *userdata, size_t done, size_t total);

/* Call after relative-path setup; the path is copied. Reconfiguration refuses
 * while any context pins a module. A bare Zehn and wrapped TNS both use an
 * absolute-offset footer at EOF. The host API must outlive loaded modules. */
bool codec_modules_configure(const char *path, const CodecHostApi *host);
/* Set immediately after configure, before any module load. Exact payload
 * digests compiled into the host reject a bundle replaced during this run.
 * NULL/zero leaves matching disabled for standalone loader tests. */
bool codec_modules_set_expected_hashes(const uint8_t hashes[CODEC_MODULE_COUNT][32],
                                       uint32_t present_mask);
/* Called between bounded read/hash/relocation batches; false cancels load.
 * Callback may update UI/input, but must not call any decoder/module API. */
void codec_modules_set_progress(CodecModuleProgress callback, void *userdata);
const CodecModuleApi *codec_module_acquire(uint32_t codec_id);
void codec_module_release(uint32_t codec_id);
bool codec_modules_trim_idle(void);
bool codec_modules_shutdown(void);
/* Empty when no loader error is pending. Successful shutdown/configure clears
 * this error and the progress callback/userdata; install progress afterwards. */
const char *codec_module_error(void);
size_t codec_modules_loaded_bytes(void);
unsigned codec_module_references(uint32_t codec_id);
const CodecHostApi *codec_modules_default_host_api(void);
#ifdef __cplusplus
}
#endif
#endif
