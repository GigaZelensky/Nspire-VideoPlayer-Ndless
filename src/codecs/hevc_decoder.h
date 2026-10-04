#ifndef NDVIDEO_HEVC_DECODER_H
#define NDVIDEO_HEVC_DECODER_H

#include <stddef.h>
#include <stdint.h>
#include "video_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hevc_decoder hevc_decoder_t;
typedef uint64_t (*hevc_clock_fn)(void);

typedef enum hevc_status {
    HEVC_ERROR = -1,
    HEVC_NEED_INPUT = 0,
    HEVC_PROGRESS = 1,
    HEVC_FRAME_READY = 2,
    HEVC_END = 3
} hevc_status_t;

typedef VideoFrame hevc_frame_t;

/* Single-owner, single-core decoder. No OS threads or SIMD.
 * Accepts Main 8-bit 4:2:0, <=320x240 coded pixels, CTU32/64, I/P pictures,
 * no tiles/WPP. One complete Annex-B access unit may be pending at a time.
 * Submit copies input and marks its end-of-frame. A held output blocks step
 * and submit until released; references remain owned by the decoder. Releasing
 * the output permits submitting the next access unit immediately.
 */
hevc_decoder_t *hevc_create(void);
/* Optional 32-byte-aligned working storage, retained until destroy. Decoders
 * stepped serially may share it: only scratch is shared, never references or
 * entropy state. Otherwise hevc_create uses ordinary heap storage. */
size_t hevc_working_memory_size(void);
hevc_decoder_t *hevc_create_with_memory(void *memory, size_t bytes);
const void *hevc_external_memory(const hevc_decoder_t *decoder);
void hevc_destroy(hevc_decoder_t *decoder);
void hevc_reset(hevc_decoder_t *decoder);
hevc_status_t hevc_submit_annexb(hevc_decoder_t *decoder,
                               const void *data, size_t bytes, int64_t pts);
/* Decode at most budget_ctus complete coding tree units. 0 means 1.
 * Header parsing and optional end-of-picture filters are additional work.
 * Call repeatedly on PROGRESS. NEED_INPUT permits the next submit.
 */
hevc_status_t hevc_step(hevc_decoder_t *decoder, unsigned budget_ctus);
/* As hevc_step, with an optional monotonic tick deadline checked before each
 * complete CTU. A NULL clock disables the deadline. A non-NULL clock with an
 * expired deadline yields PROGRESS without starting another CTU, including
 * when no CTUs have yet been decoded. Headers and picture finalization are
 * not interrupted, and an in-flight CTU may finish beyond the deadline.
 * The clock runs synchronously and must not reenter or mutate the decoder.
 * The decoder never retains the clock or deadline after this call returns.
 */
hevc_status_t hevc_step_until(hevc_decoder_t *decoder, unsigned budget_ctus,
                              uint64_t deadline_ticks, hevc_clock_fn clock);
const hevc_frame_t *hevc_get_frame(const hevc_decoder_t *decoder);
void hevc_release_frame(hevc_decoder_t *decoder);
hevc_status_t hevc_flush(hevc_decoder_t *decoder);
const char *hevc_error_string(const hevc_decoder_t *decoder);
unsigned hevc_last_step_ctus(const hevc_decoder_t *decoder);
uint64_t hevc_total_ctus(const hevc_decoder_t *decoder);
unsigned hevc_picture_ctus_done(const hevc_decoder_t *decoder);
unsigned hevc_picture_ctus_total(const hevc_decoder_t *decoder);
unsigned hevc_ctu_size(const hevc_decoder_t *decoder);

#ifdef __cplusplus
}
#endif
#endif
