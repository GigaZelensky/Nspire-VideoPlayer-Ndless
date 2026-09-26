#ifndef NDVIDEO_PLAYBACK_RENDER_GATE_H
#define NDVIDEO_PLAYBACK_RENDER_GATE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;
    bool effects_active;
    uint32_t frame;
    uint32_t presented_ms;
    uint64_t render_ticks;
} PlaybackRenderGate;

/* A pointer/chrome redraw just before a video deadline would transfer the old
 * image and then immediately transfer the new one. Keep the old gate/revision
 * state untouched and include that UI change in the upcoming presentation.
 * Input handling itself continues normally. First/changed frames and urgent
 * interactions always draw; the caller excludes paused and non-smooth modes. */
static inline bool playback_defer_ui_render(const PlaybackRenderGate *gate, uint32_t frame,
                                            uint64_t now_ticks, uint64_t due_ticks,
                                            uint32_t tick_hz, bool smooth_playing, bool urgent)
{
    uint64_t estimate, margin, remaining;
    if (!smooth_playing || urgent || !gate->valid || frame != gate->frame || !tick_hz)
        return false;
    if (now_ticks >= due_ticks)
        return true;
    estimate = gate->render_ticks ? gate->render_ticks : ((uint64_t)tick_hz * 8U + 999U) / 1000U;
    margin = ((uint64_t)tick_hz + 999U) / 1000U;
    remaining = due_ticks - now_ticks;
    return remaining <= estimate || remaining - estimate <= margin;
}

/* Timed on every real render, independently of debug collection. Retain a
 * decaying high-water estimate so heavier chrome gets enough deadline room. */
static inline void playback_render_observed(PlaybackRenderGate *gate, uint64_t elapsed_ticks)
{
    uint64_t decayed = gate->render_ticks - gate->render_ticks / 16U;
    gate->render_ticks = elapsed_ticks > decayed ? elapsed_ticks : decayed;
}

/* Touchpad motion can be clamped at an edge or round to the same screen
 * pixel. It still counts as activity, but cannot change the drawn cursor. */
static inline bool playback_pointer_pixels_changed(bool was_visible, int old_x, int old_y,
                                                   bool visible, int x, int y)
{
    return was_visible != visible || (visible && (x != old_x || y != old_y));
}

static inline bool playback_timed_badge_visible(uint32_t now_ms, uint32_t started_ms,
                                                uint32_t until_ms, uint32_t exit_ms)
{
    return started_ms && until_ms && (int32_t)(now_ms - (until_ms + exit_ms)) < 0;
}

static inline bool playback_timed_badge_animating(uint32_t now_ms, uint32_t started_ms,
                                                  uint32_t until_ms, uint32_t entrance_ms,
                                                  uint32_t exit_ms)
{
    return playback_timed_badge_visible(now_ms, started_ms, until_ms, exit_ms) &&
           ((uint32_t)(now_ms - started_ms) < entrance_ms ||
            (uint32_t)(now_ms - until_ms) < exit_ms);
}

/* Live video refreshes the memory HUD with every frame already. When the
 * image is static, four updates per second keep counters useful without
 * turning a diagnostic badge into continuous full-screen LCD transfers. */
static inline bool playback_memory_refresh_due(const PlaybackRenderGate *gate, uint32_t now_ms,
                                               bool enabled)
{
    return enabled && (!gate->valid || (uint32_t)(now_ms - gate->presented_ms) >= 250U);
}

/* Input and decoded frames are immediate. Timed chrome can animate at 60 Hz;
 * a clean unchanged frame (including a settled pause badge) needs no redraw.
 * Always draw the final state when an effect disappears. */
static inline bool playback_render_needed(PlaybackRenderGate *gate, uint32_t frame, uint32_t now_ms,
                                          bool input_changed, bool effects_active)
{
    if (!gate->valid || frame != gate->frame || input_changed ||
        effects_active != gate->effects_active ||
        (effects_active && (uint32_t)(now_ms - gate->presented_ms) >= 16U)) {
        gate->valid = true;
        gate->frame = frame;
        gate->effects_active = effects_active;
        gate->presented_ms = now_ms;
        return true;
    }
    return false;
}
#endif
