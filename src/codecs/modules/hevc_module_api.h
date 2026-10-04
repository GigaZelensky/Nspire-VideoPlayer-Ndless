#ifndef NDVIDEO_HEVC_MODULE_API_H
#define NDVIDEO_HEVC_MODULE_API_H
#include "../hevc_decoder.h"

/* C-only module boundary. Frame planes, error strings and contexts remain
 * module-owned and are valid only while their decoder pins the module. */
typedef struct HevcModuleApi {
    hevc_decoder_t *(*create)(void);
    size_t (*working_memory_size)(void);
    hevc_decoder_t *(*create_with_memory)(void *, size_t);
    const void *(*external_memory)(const hevc_decoder_t *);
    void (*destroy)(hevc_decoder_t *);
    void (*reset)(hevc_decoder_t *);
    hevc_status_t (*submit_annexb)(hevc_decoder_t *, const void *, size_t, int64_t);
    hevc_status_t (*step)(hevc_decoder_t *, unsigned);
    hevc_status_t (*step_until)(hevc_decoder_t *, unsigned, uint64_t, hevc_clock_fn);
    const hevc_frame_t *(*get_frame)(const hevc_decoder_t *);
    void (*release_frame)(hevc_decoder_t *);
    hevc_status_t (*flush)(hevc_decoder_t *);
    const char *(*error_string)(const hevc_decoder_t *);
    unsigned (*last_step_ctus)(const hevc_decoder_t *);
    uint64_t (*total_ctus)(const hevc_decoder_t *);
    unsigned (*picture_ctus_done)(const hevc_decoder_t *);
    unsigned (*picture_ctus_total)(const hevc_decoder_t *);
    unsigned (*ctu_size)(const hevc_decoder_t *);
} HevcModuleApi;
#endif
