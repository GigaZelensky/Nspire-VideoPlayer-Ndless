#ifndef NDVIDEO_VIDEO_LOOKAHEAD_H
#define NDVIDEO_VIDEO_LOOKAHEAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Movie;
struct VideoLookahead;

#define VIDEO_LOOKAHEAD_MAX_FRAMES 512U
#define VIDEO_LOOKAHEAD_MAX_BYTES (24U * 1024U * 1024U)
#define VIDEO_LOOKAHEAD_FALLBACK_BYTES (10U * 1024U * 1024U)
#define VIDEO_LOOKAHEAD_HEADROOM_BYTES (6U * 1024U * 1024U)
#define VIDEO_LOOKAHEAD_ALLOCATION_ALLOWANCE 128U
#define VIDEO_LOOKAHEAD_COLOR_ROWS 16U

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
    uint32_t recoveries, recovery_frames;
    uint32_t failed_frame, failure_visible_frame, failure_queued;
    int failure_chunk;
    char failure_reason[128];
    bool active, partial;
} VideoLookaheadStats;

/* Begin only after normal decode has established the displayed frame. The
 * decoder must not be changed behind an active lookahead instance. */
bool video_lookahead_begin(struct Movie *movie);
bool video_lookahead_active(const struct Movie *movie);

/* One macroblock batch or row-band conversion against an absolute deadline.
 * Normal decoding uses ready chunks; error recovery may advance a bounded
 * nonblocking reload. Returns whether work was performed. */
bool video_lookahead_step(struct Movie *movie, uint64_t deadline_ticks);
bool video_lookahead_reloading(const struct Movie *movie);

/* Consume a queued exact frame without decoding. The framebuffer and its
 * allocation are swapped, so this is independent of image dimensions. */
bool video_lookahead_take(struct Movie *movie, uint32_t target_frame);

/* Complete a sequential target into the queue without changing the displayed
 * frame or framebuffer. 1: ready, 2: more cooperative work needed, 0: seek required, -1: failure. */
int video_lookahead_prepare_target(struct Movie *movie, uint32_t target_frame);

/* Queue depth at the first preparation attempt, retained through interrupted
 * waits so capture still identifies foreground misses at eventual commit. */
unsigned video_lookahead_prepared_depth(const struct Movie *movie, uint32_t target_frame);

/* 1: target presented, 0: inactive/nonsequential (normal seek required),
 * 2: more cooperative work needed, -1: decoding failed. Retains partial work. */
int video_lookahead_finish_target(struct Movie *movie, uint32_t target_frame);

/* Present a forward clock-selected frame, retaining later queued frames and
 * any partial decode. Catch-up reconstructs references without coloring
 * pictures already too late to display. Same return convention as above. */
bool video_lookahead_pending_realtime_target(const struct Movie *movie, uint32_t *target);
int video_lookahead_finish_realtime_target(struct Movie *movie, uint32_t target_frame);

/* A seek/recovery cancellation invalidates mutated compressed bytes and
 * partial decoder state before normal decoding. Pause/rate changes retain it. */
void video_lookahead_cancel(struct Movie *movie);
void video_lookahead_destroy(struct Movie *movie);

/* The prefetch horizon follows decoding, independently of presentation. */
uint32_t video_lookahead_next_frame(const struct Movie *movie);
unsigned video_lookahead_queued(const struct Movie *movie);
size_t video_lookahead_memory_bytes(const struct Movie *movie);
void video_lookahead_get_stats(const struct Movie *movie, VideoLookaheadStats *out);

#endif
