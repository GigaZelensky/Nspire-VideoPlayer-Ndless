#ifndef NDVIDEO_NIGHT_MODE_MATH_H
#define NDVIDEO_NIGHT_MODE_MATH_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define NIGHT_MODE_DEFAULT_PERCENT 50U
#define NIGHT_MODE_STEP_PERCENT 10
#define NIGHT_MODE_LUT_SIZE 2048U

typedef struct {
    bool enabled;
    unsigned percent;
    bool previous_toggle;
    bool previous_up;
    bool previous_down;
    int repeat_direction;
    uint32_t repeat_at;
} NightModeState;

/* Red stays unchanged; suppress blue most and green slightly for a warm tint.
 * Rebuild only on strength changes. The complete table fits in 4 KiB. */
static inline void night_mode_build_lut_q8(uint16_t *table, unsigned percent_q8)
{
    uint16_t blue[32];
    unsigned g, b;
    if (percent_q8 > 25600U)
        percent_q8 = 25600U;
    /* Only 96 channel scalings per rebuild, including during a fade. */
    for (b = 0; b < 32U; ++b)
        blue[b] = (uint16_t)((b * (256000U - percent_q8 * 8U) + 128000U) / 256000U);
    for (g = 0; g < 64U; ++g) {
        unsigned green = ((g * (256000U - percent_q8 * 2U) + 128000U) / 256000U) << 5;
        for (b = 0; b < 32U; ++b)
            table[g * 32U + b] = (uint16_t)(green | blue[b]);
    }
}

static inline void night_mode_build_lut(uint16_t *table, unsigned percent)
{
    night_mode_build_lut_q8(table, (percent > 100U ? 100U : percent) * 256U);
}

static inline void night_mode_filter_row(uint16_t *restrict dst, const uint16_t *restrict src,
                                         size_t count, const uint16_t *restrict table)
{
    size_t x;
    for (x = 0; x < count; ++x) {
        uint16_t pixel = src[x];
        dst[x] = (uint16_t)((pixel & 0xf800U) | table[pixel & 0x07ffU]);
    }
}

/* Manual OFF keeps a positive strength for the next N/Ctrl+direction. Zero is
 * fully OFF: Down stays there, Up starts at one step, and N uses the default. */
static inline bool night_mode_update_keys(NightModeState *state, bool toggle, bool up, bool down,
                                          uint32_t now_ms, bool allow_changes)
{
    if (!state) {
        return false;
    }
    bool toggle_edge = toggle && !state->previous_toggle;
    int previous_direction =
        state->previous_up != state->previous_down ? (state->previous_up ? 1 : -1) : 0;
    int direction = up != down ? (up ? 1 : -1) : 0;
    bool direction_edge = direction != 0 && direction != previous_direction;
    bool was_enabled = state->enabled;
    unsigned previous_percent = state->percent;
    state->previous_toggle = toggle;
    state->previous_up = up;
    state->previous_down = down;
    if (!allow_changes) {
        state->repeat_direction = 0;
        return false;
    }
    if (state->percent == 0) {
        state->enabled = false;
    }
    if (toggle_edge) {
        state->enabled = !state->enabled;
        if (state->enabled && state->percent == 0) {
            state->percent = NIGHT_MODE_DEFAULT_PERCENT;
        }
        state->repeat_direction = 0;
    } else if (!direction) {
        state->repeat_direction = 0;
    } else if (direction_edge || (direction == state->repeat_direction &&
                                  (int32_t)(now_ms - state->repeat_at) >= 0)) {
        if (!state->enabled && state->percent > 0) {
            state->enabled = true;
        } else {
            int percent = (int)state->percent + direction * NIGHT_MODE_STEP_PERCENT;
            state->percent = percent < 0 ? 0U : percent > 100 ? 100U : (unsigned)percent;
            state->enabled = state->percent != 0;
        }
        state->repeat_at = now_ms + (direction_edge ? 300U : 100U);
        state->repeat_direction = direction;
    }
    return state->enabled != was_enabled || state->percent != previous_percent;
}
#endif
