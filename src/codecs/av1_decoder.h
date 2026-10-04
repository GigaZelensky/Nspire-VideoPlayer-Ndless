#ifndef NDVIDEO_AV1_DECODER_H
#define NDVIDEO_AV1_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "video_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct av1_decoder av1_decoder_t;
typedef uint64_t (*av1_clock_fn)(void);
typedef VideoFrame av1_frame_t;
typedef enum {
    AV1_ERROR = -1,
    AV1_NEED_INPUT = 0,
    AV1_PROGRESS = 1,
    AV1_FRAME_READY = 2
} av1_status_t;

/* One visible Main 8-bit 4:2:0 frame per OBU packet. Input is copied; output
 * remains private until reconstruction finishes and stays held until release.
 * Each step completes at most the requested number of superblocks. */
av1_decoder_t *av1_create(void);
/* Optional 32-byte-aligned scratch, borrowed until destroy. Serial decoder
 * instances may share it; pictures, references and entropy remain private. */
size_t av1_working_memory_size(void);
av1_decoder_t *av1_create_with_memory(void *memory, size_t bytes);
const void *av1_external_memory(const av1_decoder_t *decoder);
void av1_destroy(av1_decoder_t *decoder);
void av1_reset(av1_decoder_t *decoder);
av1_status_t av1_submit_obus(av1_decoder_t *decoder, const void *data, size_t bytes, int64_t pts);
av1_status_t av1_step(av1_decoder_t *decoder, unsigned budget);
/* Optional absolute deadline in clock() ticks, checked before each superblock.
 * Zero deadline or NULL clock keeps the legacy budget-only behavior. Header,
 * row filtering and frame completion remain indivisible; progress may be zero. */
av1_status_t av1_step_until(av1_decoder_t *decoder, unsigned budget,
                            uint64_t deadline_ticks, av1_clock_fn clock);
const av1_frame_t *av1_get_frame(const av1_decoder_t *decoder);
/* Exact visible YUV equality with the preceding completed output of this
 * decoder. Reset clears this identity; serial remains monotonic across reset. */
bool av1_frame_repeats_previous(const av1_decoder_t *decoder);
uint64_t av1_frame_serial(const av1_decoder_t *decoder);
void av1_release_frame(av1_decoder_t *decoder);
const char *av1_error_string(const av1_decoder_t *decoder);
unsigned av1_last_step_blocks(const av1_decoder_t *decoder);
unsigned av1_picture_blocks_done(const av1_decoder_t *decoder);
unsigned av1_picture_blocks_total(const av1_decoder_t *decoder);
unsigned av1_block_size(const av1_decoder_t *decoder);
bool av1_packet_is_independent(const void *data, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
