#ifndef NDVIDEO_PLAYBACK_CAPTURE_CORE_H
#define NDVIDEO_PLAYBACK_CAPTURE_CORE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CAPTURE_FRAME_CAPACITY 8192U
#define CAPTURE_IO_CAPACITY 512U
enum {
    CAPTURE_INPUT,
    CAPTURE_DECODE,
    CAPTURE_RENDER,
    CAPTURE_PREFETCH,
    CAPTURE_WAIT,
    CAPTURE_IO,
    CAPTURE_BOOKKEEPING,
    CAPTURE_TEXT_FORMAT,
    CAPTURE_COLOR,
    CAPTURE_NIGHT,
    CAPTURE_LCD,
    CAPTURE_SCREENSHOT,
    CAPTURE_AHEAD,
    CAPTURE_AHEAD_COLOR,
    CAPTURE_WAIT_INPUT,
    CAPTURE_WAIT_TOUCHPAD,
    CAPTURE_WRITER_SERVICE,
    CAPTURE_WAIT_IO,
    CAPTURE_STAGE_COUNT
};
enum {
    CAPTURE_IO_SYNC = 1,
    CAPTURE_IO_PREFETCH,
    CAPTURE_IO_SEEK,
    CAPTURE_IO_RESET,
    CAPTURE_IO_REUSE,
    CAPTURE_IO_ASYNC,
    CAPTURE_IO_ASYNC_SCREENSHOT,
    CAPTURE_IO_KIND_COUNT
};
enum {
    CAPTURE_FRAME_PRESENTED = 1,
    CAPTURE_FRAME_TRANSITION = 2,
    CAPTURE_FRAME_OVERFLOW = 4,
    CAPTURE_FRAME_NIGHT = 8,
    CAPTURE_FRAME_SKIP_ENABLED = 16
};

typedef struct {
    uint64_t total;
    uint64_t maximum;
    uint32_t count;
} CaptureTiming;

typedef struct {
    uint64_t at_ticks;
    uint64_t due_ticks;
    uint32_t frame, chunk, interval_ticks, skipped;
    uint32_t stage[CAPTURE_STAGE_COUNT];
    uint32_t ahead_next_frame;
    uint16_t rate_num, rate_den, ahead_before, ahead_after;
    uint8_t flags, night_percent, scale_mode;
} CaptureFrame;

typedef struct {
    uint64_t at_ticks, duration_ticks;
    int32_t chunk;
    uint32_t bytes;
    uint8_t kind;
} CaptureIo;

typedef struct {
    uint16_t rate_num, rate_den, subtitle_track;
    uint8_t paused, frame_skip, scale_mode, night_enabled, night_percent;
    uint8_t subtitle_font, subtitle_placement, memory_overlay;
    int8_t subtitle_size;
} CaptureSettings;

static inline bool capture_settings_equal(const CaptureSettings *a, const CaptureSettings *b)
{
    return a->rate_num == b->rate_num && a->rate_den == b->rate_den &&
           a->subtitle_track == b->subtitle_track && a->paused == b->paused &&
           a->frame_skip == b->frame_skip && a->scale_mode == b->scale_mode &&
           a->night_enabled == b->night_enabled && a->night_percent == b->night_percent &&
           a->subtitle_font == b->subtitle_font && a->subtitle_placement == b->subtitle_placement &&
           a->memory_overlay == b->memory_overlay && a->subtitle_size == b->subtitle_size;
}

static inline void capture_timing_add(CaptureTiming *timing, uint64_t ticks)
{
    timing->total += ticks;
    if (ticks > timing->maximum)
        timing->maximum = ticks;
    ++timing->count;
}

static inline uint32_t capture_ticks32(uint64_t ticks, uint8_t *flags)
{
    if (ticks > UINT32_MAX) {
        *flags |= CAPTURE_FRAME_OVERFLOW;
        return UINT32_MAX;
    }
    return (uint32_t)ticks;
}

/* Convert only while exporting. Splitting the quotient avoids ticks*1000000
 * overflow even after a long recording or a fast hardware timer. */
static inline uint64_t capture_ticks_to_us(uint64_t ticks, uint32_t frequency)
{
    if (!frequency)
        return 0;
    return (ticks / frequency) * 1000000U + ((ticks % frequency) * 1000000U) / frequency;
}

static inline size_t capture_ring_push_index(uint32_t *next, uint32_t *count, uint64_t *overwritten,
                                             uint32_t capacity)
{
    size_t index = *next;
    *next = (*next + 1U == capacity) ? 0U : *next + 1U;
    if (*count < capacity)
        ++*count;
    else
        ++*overwritten;
    return index;
}

static inline size_t capture_ring_oldest(uint32_t next, uint32_t count, uint32_t capacity)
{
    return count == capacity ? next : 0U;
}
#endif
