#ifndef NDVIDEO_PLAYBACK_PACING_H
#define NDVIDEO_PLAYBACK_PACING_H

#include <stdint.h>

/* HEVC/AV1 admission already includes the measured coding-unit/conversion
 * cost and at least 1 ms of margin. Keep a short outer return allowance,
 * rather than reserving a second 2 ms after every guarded operation. */
static inline uint32_t playback_planar_return_guard(uint32_t tick_hz)
{
    return tick_hz / 2000U + (tick_hz % 2000U != 0U);
}

/* Smooth playback prepares a frame before its presentation deadline. Bound
 * the lead at low playback rates so UI polling/animations still run between
 * frames; the deadline wait itself remains interruptible by input. */
static inline uint64_t playback_decode_due(uint64_t present_due, uint64_t interval,
                                           uint32_t tick_hz, uint32_t decode_cost_ms)
{
    /* Dense 24 fps frames can need more than the old fixed 32 ms lead even
     * when decode + display fits inside 41.7 ms. Start earlier using the
     * recent foreground cost instead of idling and then arriving late. */
    uint32_t lead_ms = 32U;
    if (decode_cost_ms > 30U)
        lead_ms = decode_cost_ms >= 62U ? 64U : decode_cost_ms + 2U;
    uint64_t lead = (uint64_t)tick_hz * lead_ms / 1000U;
    if (lead > interval)
        lead = interval;
    return present_due > lead ? present_due - lead : 0;
}

/* Keep phase across overshoot within the existing 1 ms input-poll granularity.
 * Permanently rebasing each tiny wake delay slowly reduces playback rate even
 * when there is ample CPU time. Larger decode overruns still rebase fully:
 * recovery cannot intentionally shorten a render interval by more than 1 ms.
 * Input-interrupted early presentation must not advance the timeline. */
static inline uint64_t playback_present_anchor(uint64_t due, uint64_t ready, uint32_t tick_hz)
{
    uint64_t jitter_ticks = tick_hz / 1000U;
    return ready > due && ready - due > jitter_ticks ? ready : due;
}

#endif
