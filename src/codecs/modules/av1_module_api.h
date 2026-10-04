#ifndef NDVIDEO_AV1_MODULE_API_H
#define NDVIDEO_AV1_MODULE_API_H
#include "../av1_decoder.h"
typedef struct Av1ModuleApi {
    av1_decoder_t *(*create)(void);
    size_t (*working_memory_size)(void);
    av1_decoder_t *(*create_with_memory)(void *, size_t);
    const void *(*external_memory)(const av1_decoder_t *);
    void (*destroy)(av1_decoder_t *);
    void (*reset)(av1_decoder_t *);
    av1_status_t (*submit_obus)(av1_decoder_t *, const void *, size_t, int64_t);
    av1_status_t (*step)(av1_decoder_t *, unsigned);
    av1_status_t (*step_until)(av1_decoder_t *, unsigned, uint64_t, av1_clock_fn);
    const av1_frame_t *(*get_frame)(const av1_decoder_t *);
    bool (*frame_repeats_previous)(const av1_decoder_t *);
    uint64_t (*frame_serial)(const av1_decoder_t *);
    void (*release_frame)(av1_decoder_t *);
    const char *(*error_string)(const av1_decoder_t *);
    unsigned (*last_step_blocks)(const av1_decoder_t *);
    unsigned (*picture_blocks_done)(const av1_decoder_t *);
    unsigned (*picture_blocks_total)(const av1_decoder_t *);
    unsigned (*block_size)(const av1_decoder_t *);
    bool (*packet_is_independent)(const void *, size_t);
} Av1ModuleApi;
#endif
