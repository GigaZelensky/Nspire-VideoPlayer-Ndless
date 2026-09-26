#ifndef NDVIDEO_H264_LOOKAHEAD_H
#define NDVIDEO_H264_LOOKAHEAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Movie;
struct H264Lookahead;

#define H264_LOOKAHEAD_MAX_FRAMES 512U
#define H264_LOOKAHEAD_MAX_BYTES (24U * 1024U * 1024U)
#define H264_LOOKAHEAD_FALLBACK_BYTES (10U * 1024U * 1024U)
#define H264_LOOKAHEAD_HEADROOM_BYTES (6U * 1024U * 1024U)
#define H264_LOOKAHEAD_ALLOCATION_ALLOWANCE 128U
#define H264_LOOKAHEAD_COLOR_ROWS 16U

typedef struct {
    unsigned capacity, queued, peak_queued;
    size_t allocated_bytes;
    size_t budget_bytes, reserve_bytes, free_bytes_at_begin, free_bytes_last;
    uint32_t memory_checks, memory_denials, allocation_failures;
    bool memory_known_at_begin, memory_known;
    uint32_t background_frames, foreground_frames, queue_hits, queue_misses;
    uint32_t chunk_waits, cancellations, failures, decode_slices;
    uint32_t max_pump_ticks, max_color_ticks;
    uint32_t max_background_pump_ticks, max_background_color_ticks;
    uint32_t background_slices_8, background_slices_16, background_slices_32;
    uint32_t background_slices_4;
    uint32_t background_color_bands_16, background_color_bands_32;
    uint32_t background_color_bands_64, background_color_bands_tail;
    uint32_t max_background_tail_ticks, color_tail_guard_ticks;
    uint64_t decode_ticks, color_ticks;
    bool active, partial;
} H264LookaheadStats;

/* Begin only after normal decode has established the displayed frame. The
 * decoder must not be changed behind an active lookahead instance. */
bool h264_lookahead_begin(struct Movie *movie);
bool h264_lookahead_active(const struct Movie *movie);

/* One macroblock batch or row-band conversion against an absolute deadline.
 * Never starts a cold/unfinished read. Returns whether work was performed. */
bool h264_lookahead_step(struct Movie *movie, uint64_t deadline_ticks);

/* Consume a queued exact frame without decoding. The framebuffer and its
 * allocation are swapped, so this is independent of image dimensions. */
bool h264_lookahead_take(struct Movie *movie, uint32_t target_frame);

/* Complete a sequential target into the queue without changing the displayed
 * frame or framebuffer. 1: ready, 0: normal seek required, -1: failure. */
int h264_lookahead_prepare_target(struct Movie *movie, uint32_t target_frame);

/* Queue depth at the first preparation attempt, retained through interrupted
 * waits so capture still identifies foreground misses at eventual commit. */
unsigned h264_lookahead_prepared_depth(const struct Movie *movie, uint32_t target_frame);

/* 1: target presented, 0: inactive/nonsequential (normal seek required),
 * -1: decoding failed. Finishes existing partial work; never decodes twice. */
int h264_lookahead_finish_target(struct Movie *movie, uint32_t target_frame);

/* A seek/recovery cancellation invalidates mutated compressed bytes and
 * partial decoder state before normal decoding. Pause/rate changes retain it. */
void h264_lookahead_cancel(struct Movie *movie);
void h264_lookahead_destroy(struct Movie *movie);

/* The prefetch horizon follows decoding, independently of presentation. */
uint32_t h264_lookahead_next_frame(const struct Movie *movie);
unsigned h264_lookahead_queued(const struct Movie *movie);
size_t h264_lookahead_memory_bytes(const struct Movie *movie);
void h264_lookahead_get_stats(const struct Movie *movie, H264LookaheadStats *out);

#endif
