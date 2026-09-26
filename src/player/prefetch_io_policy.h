#ifndef NDVIDEO_PREFETCH_IO_POLICY_H
#define NDVIDEO_PREFETCH_IO_POLICY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define PREFETCH_IO_QUANTUM_BYTES 512U
#define PREFETCH_IO_DEADLINE_MAX_BYTES 8192U
#define PREFETCH_IO_GUARD_MS 1U
#define PREFETCH_IO_INITIAL_BYTES_PER_MS 1024U
#define PREFETCH_IO_OVERLOAD_SLICE_MS 4U
#define PREFETCH_IO_OVERLOAD_MAX_BYTES 4096U
#define PREFETCH_ASYNC_URGENT_FRAMES 16U

/* A decoded frame's successor already needs the CPU when spare_ms is zero.
 * Keep speculative reads out of that path while contiguous ready frames give
 * us room to finish them during the next presentation wait. Low runway still
 * earns service under sustained decode overload, and pause can fill freely. */
static inline bool prefetch_async_should_work(bool paused, uint32_t ready_frames, uint32_t spare_ms)
{
    return paused || spare_ms != 0U || ready_frames <= PREFETCH_ASYNC_URGENT_FRAMES;
}

/* 32768 Hz service budgets. A small, bounded slice is still necessary under
 * decode overload; otherwise short chunks exhaust lookahead while reads stall. */
static inline unsigned prefetch_async_budget_ticks(uint32_t ready_frames, uint32_t spare_ms)
{
    unsigned budget = ready_frames <= PREFETCH_ASYNC_URGENT_FRAMES ? 64U : 8U;
    if (spare_ms) {
        unsigned spare = spare_ms > 4U ? 128U : spare_ms * 32U;
        if (spare > budget)
            budget = spare;
    }
    return budget;
}

/* Setup/input checks and a READY copy consume the same presentation slack as
 * storage service. Reserve 1 ms for the final bounded step/publication rather
 * than giving the reader the original budget after setup has already spent it.
 * Zero spare is the explicit low-runway overload path, not an expired deadline. */
static inline unsigned prefetch_async_remaining_ticks(uint32_t ready_frames, uint32_t spare_ms,
                                                      uint32_t elapsed_ms)
{
    unsigned budget = prefetch_async_budget_ticks(ready_frames, spare_ms);
    if (!spare_ms)
        return budget;
    if (elapsed_ms >= spare_ms || spare_ms - elapsed_ms <= 1U)
        return 0U;
    uint32_t remaining = spare_ms - elapsed_ms - 1U;
    unsigned limit = remaining > 4U ? 128U : remaining * 32U;
    return budget < limit ? budget : limit;
}

/* This bounds a cooperative read request, not an OS/filesystem stall inside
 * fread. Keep headroom, and round non-tail requests down to whole sectors. */
static inline size_t prefetch_io_read_size(uint32_t bytes_per_ms, size_t remaining,
                                           int32_t time_left_ms)
{
    uint32_t available_ms;
    uint32_t bytes;

    if (time_left_ms <= (int32_t)PREFETCH_IO_GUARD_MS || remaining == 0) {
        return 0;
    }
    if (bytes_per_ms == 0) {
        bytes_per_ms = PREFETCH_IO_INITIAL_BYTES_PER_MS;
    }
    available_ms = (uint32_t)time_left_ms - PREFETCH_IO_GUARD_MS;
    bytes = bytes_per_ms > PREFETCH_IO_DEADLINE_MAX_BYTES / available_ms
                ? PREFETCH_IO_DEADLINE_MAX_BYTES
                : bytes_per_ms * available_ms;
    bytes -= bytes % PREFETCH_IO_QUANTUM_BYTES;
    if (bytes == 0) {
        return 0;
    }
    return remaining < bytes ? remaining : bytes;
}

/* With an urgent next chunk and no CPU slack, spread a small amount of I/O
 * across frames. A single-sector probe lets a pessimistic post-stall rate
 * recover instead of deferring everything to a synchronous chunk boundary. */
static inline size_t prefetch_io_maintenance_read_size(uint32_t bytes_per_ms, size_t remaining,
                                                       int32_t time_left_ms)
{
    size_t bytes;
    if (remaining > PREFETCH_IO_OVERLOAD_MAX_BYTES) {
        remaining = PREFETCH_IO_OVERLOAD_MAX_BYTES;
    }
    bytes = prefetch_io_read_size(bytes_per_ms, remaining, time_left_ms);
    if (bytes == 0 && time_left_ms > (int32_t)PREFETCH_IO_GUARD_MS) {
        bytes = remaining < PREFETCH_IO_QUANTUM_BYTES ? remaining : PREFETCH_IO_QUANTUM_BYTES;
    }
    return bytes;
}

/* Slow observations reduce the estimate immediately. Faster observations
 * recover gradually; a tiny/cached tail must not corrupt the learned rate. */
static inline uint32_t prefetch_io_update_rate(uint32_t previous, uint32_t bytes,
                                               uint32_t elapsed_ms)
{
    uint32_t sample;
    uint32_t difference;

    if (bytes < PREFETCH_IO_QUANTUM_BYTES) {
        return previous;
    }
    if (previous == 0) {
        previous = PREFETCH_IO_INITIAL_BYTES_PER_MS;
    }
    sample = elapsed_ms == UINT32_MAX ? 1U : bytes / (elapsed_ms + 1U);
    if (sample == 0) {
        sample = 1U;
    }
    if (sample < previous) {
        return elapsed_ms == 0 ? previous : sample;
    }
    difference = sample - previous;
    return previous + difference / 4U + (difference % 4U != 0U);
}

#endif
