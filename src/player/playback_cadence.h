#ifndef NDVIDEO_PLAYBACK_CADENCE_H
#define NDVIDEO_PLAYBACK_CADENCE_H
#include <stdbool.h>
#include <stdint.h>

/* Observe completed presentations, never the rebased decoder clock. Reset
 * only the baseline on pause/seek/rate changes; accumulated measurements stay.
 * L retains the original whole-frame lateness criterion: accumulate timing
 * error until playback falls one complete interval behind, then rebase this
 * observer. Per-interval jitter remains a separate detailed diagnostic. */
typedef struct {
    bool valid;
    uint32_t frame, events, missed_intervals, max_missed_intervals;
    uint64_t presented, elapsed, expected, excess, max_excess, max_gap;
    uint64_t lag_elapsed, lag_expected, max_lag;
    uint32_t over_budget_intervals;
} PlaybackCadence;

static inline uint64_t playback_cadence_present(PlaybackCadence *c, uint64_t now, uint32_t frame,
                                                uint64_t interval)
{
    uint64_t late = 0;
    if (c->valid && frame == c->frame)
        return 0; /* UI-only redraw. */
    if (c->valid && frame > c->frame && now >= c->presented && interval) {
        uint64_t gap = now - c->presented;
        uint64_t expected = (uint64_t)(frame - c->frame) * interval;
        c->elapsed += gap;
        c->expected += expected;
        c->lag_elapsed += gap;
        c->lag_expected += expected;
        if (gap > c->max_gap)
            c->max_gap = gap;
        if (gap > expected) {
            late = gap - expected;
            c->excess += late;
            if (late > c->max_excess)
                c->max_excess = late;
            if (late > 1U)
                ++c->over_budget_intervals;
        }
        uint64_t behind = c->lag_elapsed > c->lag_expected ? c->lag_elapsed - c->lag_expected : 0;
        uint32_t skipped = frame - c->frame - 1U;
        if (behind >= interval || skipped) {
            ++c->events;
            uint32_t missed = skipped + (uint32_t)(behind / interval);
            c->missed_intervals += missed;
            if (missed > c->max_missed_intervals)
                c->max_missed_intervals = missed;
            uint64_t lag = behind + (uint64_t)skipped * interval;
            if (lag > c->max_lag)
                c->max_lag = lag;
            c->lag_elapsed = c->lag_expected = 0;
        }
    } else
        c->lag_elapsed = c->lag_expected = 0;
    c->valid = true;
    c->presented = now;
    c->frame = frame;
    return late;
}
#endif
