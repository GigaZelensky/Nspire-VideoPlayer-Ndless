#include "player_internal.h"

void pointer_init(PointerState *pointer)
{
    memset(pointer, 0, sizeof(*pointer));
    pointer->info = touchpad_getinfo();
    pointer->x = SCREEN_W / 2;
    pointer->y = SCREEN_H / 2;
    pointer->fx = pointer->x << POINTER_FIXED_SHIFT;
    pointer->fy = pointer->y << POINTER_FIXED_SHIFT;
    if (pointer->info) {
        pointer->max_touch_dx = pointer->info->width / POINTER_SPIKE_DELTA_DIVISOR;
        pointer->max_touch_dy = pointer->info->height / POINTER_SPIKE_DELTA_DIVISOR;
        if (pointer->max_touch_dx <= POINTER_JITTER_THRESHOLD) {
            pointer->max_touch_dx = POINTER_JITTER_THRESHOLD + 1;
        }
        if (pointer->max_touch_dy <= POINTER_JITTER_THRESHOLD) {
            pointer->max_touch_dy = POINTER_JITTER_THRESHOLD + 1;
        }
    }
    pointer->visible = pointer->info != NULL;
}

bool pointer_update(PointerState *pointer)
{
    touchpad_report_t report;
    bool click_edge = false;
    bool current_down = false;
    bool previous_down;
    bool has_touch_position;
    if (!pointer->info || touchpad_scan(&report) != 0) {
        pointer->press_edge = false;
        pointer->release_edge = false;
        return false;
    }
    pointer->moved = false;
    pointer->press_edge = false;
    pointer->release_edge = false;
    current_down = (report.pressed && report.arrow == TPAD_ARROW_CLICK) ? true : false;
    has_touch_position =
        (report.contact || report.proximity) &&
        report.x < pointer->info->width &&
        report.y < pointer->info->height;
    if (has_touch_position) {
        if (!pointer->tracking) {
            pointer->last_touch_x = report.x;
            pointer->last_touch_y = report.y;
            pointer->tracking = true;
        } else {
            int dx = (int) report.x - pointer->last_touch_x;
            int dy = (int) report.y - pointer->last_touch_y;
            int abs_dx;
            int abs_dy;
            abs_dx = dx < 0 ? -dx : dx;
            abs_dy = dy < 0 ? -dy : dy;
            if (abs_dx > pointer->max_touch_dx || abs_dy > pointer->max_touch_dy) {
                pointer->tracking = false;
            } else if (abs_dx <= POINTER_JITTER_THRESHOLD && abs_dy <= POINTER_JITTER_THRESHOLD) {
                dx = 0;
                dy = 0;
            } else {
                if (abs_dx * POINTER_AXIS_LOCK_RATIO_DEN >= abs_dy * POINTER_AXIS_LOCK_RATIO_NUM) {
                    dy = 0;
                } else if (abs_dy * POINTER_AXIS_LOCK_RATIO_DEN >= abs_dx * POINTER_AXIS_LOCK_RATIO_NUM) {
                    dx = 0;
                } else if (abs_dx + abs_dy < POINTER_DECISIVE_SUM_THRESHOLD) {
                    dx = 0;
                    dy = 0;
                }
            }
            if (pointer->tracking && (dx != 0 || dy != 0)) {
                pointer->fx += (dx * SCREEN_W * POINTER_GAIN_NUM << POINTER_FIXED_SHIFT) / ((int) pointer->info->width * POINTER_GAIN_DEN);
                pointer->fy -= (dy * SCREEN_H * POINTER_GAIN_NUM << POINTER_FIXED_SHIFT) / ((int) pointer->info->height * POINTER_GAIN_DEN);
                pointer->x = clamp_int(pointer->fx >> POINTER_FIXED_SHIFT, 0, SCREEN_W - 1);
                pointer->y = clamp_int(pointer->fy >> POINTER_FIXED_SHIFT, 0, SCREEN_H - 1);
                pointer->fx = pointer->x << POINTER_FIXED_SHIFT;
                pointer->fy = pointer->y << POINTER_FIXED_SHIFT;
                pointer->moved = true;
            }
            if (pointer->tracking) {
                pointer->last_touch_x = report.x;
                pointer->last_touch_y = report.y;
            }
        }
        pointer->visible = true;
    } else {
        pointer->tracking = false;
        if (current_down) {
            pointer->visible = true;
        }
    }
    /* Treat only the touchpad center click as a pointer activation.
     * Generic "pressed" also fires for directional pad presses, which should
     * not activate UI elements such as progress-bar seeking. */
    previous_down = pointer->down;
    click_edge = current_down && !previous_down;
    pointer->press_edge = click_edge;
    pointer->release_edge = !current_down && previous_down;
    pointer->down = current_down;
    return click_edge;
}

void pointer_hover_guard_reset(PointerHoverGuard *guard)
{
    if (!guard) {
        return;
    }
    guard->locked = false;
    guard->anchor_x = 0;
    guard->anchor_y = 0;
}

void pointer_hover_guard_lock(PointerHoverGuard *guard, const PointerState *pointer)
{
    if (!guard || !pointer || !pointer->visible) {
        return;
    }
    guard->locked = true;
    guard->anchor_x = pointer->x;
    guard->anchor_y = pointer->y;
}

bool pointer_hover_guard_allows(PointerHoverGuard *guard, const PointerState *pointer, bool pointer_click)
{
    int dx;
    int dy;
    int distance_squared;
    int threshold_squared;

    if (!guard || !pointer || !pointer->visible) {
        return false;
    }
    if (!guard->locked) {
        return true;
    }
    if (pointer_click) {
        pointer_hover_guard_reset(guard);
        return true;
    }
    if (!pointer->moved) {
        return false;
    }
    dx = pointer->x - guard->anchor_x;
    dy = pointer->y - guard->anchor_y;
    distance_squared = dx * dx + dy * dy;
    threshold_squared = POINTER_HOVER_REARM_PIXELS * POINTER_HOVER_REARM_PIXELS;
    if (distance_squared < threshold_squared) {
        return false;
    }
    pointer_hover_guard_reset(guard);
    return true;
}

void picker_tooltip_hover_reset(PickerTooltipHoverState *state)
{
    if (!state) {
        return;
    }
    state->row_index = -1;
    state->started_ms = 0;
    state->armed = false;
}

int picker_tooltip_hover_update(PickerTooltipHoverState *state, int hovered_index, const PointerState *pointer, bool pointer_click, uint32_t now_ms)
{
    if (!state) {
        return -1;
    }
    if (!pointer || !pointer->visible || hovered_index < 0 || pointer_click) {
        picker_tooltip_hover_reset(state);
        return -1;
    }
    if (pointer->moved) {
        state->armed = true;
        state->row_index = hovered_index;
        state->started_ms = now_ms;
        return -1;
    }
    if (!state->armed) {
        return -1;
    }
    if (state->row_index != hovered_index) {
        state->row_index = hovered_index;
        state->started_ms = now_ms;
        return -1;
    }
    if ((uint32_t) (now_ms - state->started_ms) >= PICKER_TOOLTIP_DWELL_MS) {
        return hovered_index;
    }
    return -1;
}

char *append_uint_decimal_raw(char *out, uint32_t value)
{
    char digits[10];
    size_t count = 0;

    do {
        digits[count++] = (char) ('0' + (value % 10U));
        value /= 10U;
    } while (value > 0U && count < sizeof(digits));
    while (count > 0) {
        *out++ = digits[--count];
    }
    return out;
}

char *append_two_digits_raw(char *out, uint32_t value)
{
    value %= 100U;
    *out++ = (char) ('0' + (value / 10U));
    *out++ = (char) ('0' + (value % 10U));
    return out;
}

void copy_truncated(char *buffer, size_t buffer_size, const char *text)
{
    size_t index;

    if (!buffer || buffer_size == 0) {
        return;
    }
    if (!text) {
        buffer[0] = '\0';
        return;
    }
    for (index = 0; index + 1 < buffer_size && text[index] != '\0'; ++index) {
        buffer[index] = text[index];
    }
    buffer[index] = '\0';
}

char *append_text_bounded(char *out, char *end, const char *text)
{
    if (!out || !end || out > end) {
        return out;
    }
    if (!text) {
        *out = '\0';
        return out;
    }
    while (out < end && *text != '\0') {
        *out++ = *text++;
    }
    *out = '\0';
    return out;
}

void format_clock(uint32_t total_ms, char *buffer, size_t buffer_size)
{
    uint32_t total_seconds = total_ms / 1000;
    uint32_t hours = total_seconds / 3600;
    uint32_t minutes = (total_seconds / 60) % 60;
    uint32_t seconds = total_seconds % 60;
    char formatted[16];
    char *out = formatted;

    if (!buffer || buffer_size == 0) {
        return;
    }
    if (hours > 0) {
        out = append_uint_decimal_raw(out, hours);
        *out++ = ':';
        out = append_two_digits_raw(out, minutes);
        *out++ = ':';
        out = append_two_digits_raw(out, seconds);
    } else {
        out = append_two_digits_raw(out, minutes);
        *out++ = ':';
        out = append_two_digits_raw(out, seconds);
    }
    *out = '\0';
    copy_truncated(buffer, buffer_size, formatted);
}

MemoryStats query_memory_stats(const Movie *movie)
{
    MemoryStats stats;
    size_t framebuffer_words;
    size_t chunk_prefetch_bytes = 0;
    int index;

    memset(&stats, 0, sizeof(stats));
    if (!movie) {
        return stats;
    }

    if (movie->chunk_index) {
        stats.used_bytes += (size_t) movie->header.chunk_count * sizeof(ChunkIndexEntry);
    }
    if (movie->subtitles) {
        stats.used_bytes += (size_t) movie->header.subtitle_count * sizeof(SubtitleCue);
    }

    framebuffer_words = (size_t) movie->header.video_width * movie->header.video_height;
    if (movie->framebuffer) {
        stats.used_bytes += framebuffer_words * sizeof(uint16_t);
    }
    if (movie->frame_offsets && movie->loaded_chunk >= 0 && (uint32_t) movie->loaded_chunk < movie->header.chunk_count) {
        stats.used_bytes += (size_t) movie->chunk_index[movie->loaded_chunk].frame_count * sizeof(uint32_t);
    }
    if (movie->chunk_storage && !movie->chunk_storage_in_sram) {
        stats.used_bytes += movie->chunk_storage_size;
    }

    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        if (movie->prefetched[index].chunk_index >= 0 && movie->prefetched[index].chunk_storage) {
            chunk_prefetch_bytes += movie->prefetched[index].chunk_storage_size;
        }
    }

    stats.prefetched_bytes = chunk_prefetch_bytes;
    stats.used_bytes += chunk_prefetch_bytes;
    stats.total_bytes = APP_RAM_TARGET_BYTES;
    stats.free_bytes = stats.total_bytes > stats.used_bytes
        ? (stats.total_bytes - stats.used_bytes)
        : 0;
    if (stats.total_bytes > 0) {
        stats.percent_used = (unsigned) (((stats.used_bytes * 100U) + (stats.total_bytes / 2U)) / stats.total_bytes);
        if (stats.percent_used > 100U) {
            stats.percent_used = 100U;
        }
    }
    stats.valid = true;
    return stats;
}

void format_memory_compact(size_t bytes, char *buffer, size_t buffer_size)
{
    const size_t mib = 1024U * 1024U;
    size_t whole = bytes / mib;
    size_t tenth = ((bytes % mib) * 10U + (mib / 2U)) / mib;

    if (tenth >= 10U) {
        whole++;
        tenth = 0;
    }
    snprintf(buffer, buffer_size, "%lu.%luM", (unsigned long) whole, (unsigned long) tenth);
}

uint64_t movie_frame_interval_ticks(const Movie *movie)
{
    if (!movie->header.fps_num) {
        return 0;
    }
    return (((uint64_t) monotonic_clock_ticks_per_second()) * movie->header.fps_den) / movie->header.fps_num;
}

uint32_t tab_hold_frame_repeat_interval_ms(const Movie *movie)
{
    uint64_t interval_ms;

    if (!movie || !movie->header.fps_num || !movie->header.fps_den) {
        return TAB_HOLD_FRAME_REPEAT_FALLBACK_INTERVAL_MS;
    }

    interval_ms = (2000ULL * movie->header.fps_den + (movie->header.fps_num - 1U)) / movie->header.fps_num;
    if (interval_ms == 0ULL) {
        interval_ms = 1ULL;
    }
    if (interval_ms > UINT32_MAX) {
        interval_ms = UINT32_MAX;
    }
    return (uint32_t) interval_ms;
}

const PlaybackRate *playback_rate_for_index(size_t rate_index)
{
    if (rate_index >= PLAYBACK_RATE_COUNT) {
        return &g_playback_rates[PLAYBACK_RATE_DEFAULT_INDEX];
    }
    return &g_playback_rates[rate_index];
}

uint32_t movie_header_frame_time_ms(const MovieHeader *header, uint32_t frame_index)
{
    if (!header || !header->fps_num) {
        return 0;
    }
    return (uint32_t) (((uint64_t) frame_index * 1000ULL * header->fps_den) / header->fps_num);
}

uint32_t movie_frame_time_ms(const Movie *movie, uint32_t frame_index)
{
    return movie_header_frame_time_ms(movie ? &movie->header : NULL, frame_index);
}

uint64_t movie_frame_time_scaled_ticks(const Movie *movie, uint32_t frame_index, const PlaybackRate *rate)
{
    if (!movie->header.fps_num || !rate || !rate->numerator) {
        return 0;
    }
    return (((uint64_t) frame_index) * monotonic_clock_ticks_per_second() * movie->header.fps_den * rate->denominator)
        / (((uint64_t) movie->header.fps_num) * rate->numerator);
}

uint32_t movie_frames_from_ms(const Movie *movie, uint32_t total_ms)
{
    if (!movie->header.fps_num || !movie->header.fps_den) {
        return 0;
    }
    return (uint32_t) (((uint64_t) total_ms * movie->header.fps_num) / (1000ULL * movie->header.fps_den));
}

uint32_t movie_frames_from_scaled_ticks(const Movie *movie, uint64_t total_ticks, const PlaybackRate *rate)
{
    if (!movie->header.fps_num || !movie->header.fps_den || !rate || !rate->denominator) {
        return 0;
    }
    return (uint32_t) ((total_ticks * movie->header.fps_num * rate->numerator)
        / (((uint64_t) monotonic_clock_ticks_per_second()) * movie->header.fps_den * rate->denominator));
}

uint32_t movie_duration_ms(const Movie *movie)
{
    return movie_frame_time_ms(movie, movie->header.frame_count);
}

void reset_playback_timeline(const Movie *movie, const PlaybackRate *playback_rate, uint64_t *anchor_ticks, uint32_t *anchor_frame, uint64_t *next_frame_due_ticks)
{
    uint64_t now_ticks = monotonic_clock_now_ticks();
    *anchor_ticks = now_ticks;
    *anchor_frame = movie->current_frame;
    *next_frame_due_ticks = now_ticks + movie_frame_time_scaled_ticks(movie, 1, playback_rate);
}

bool step_movie_forward_one_frame(Movie *movie, bool *hover_preview_needs_rebuffer)
{
    if (!movie || movie->current_frame + 1 >= movie->header.frame_count) {
        return true;
    }
    if (!decode_to_frame(movie, movie->current_frame + 1)) {
        return false;
    }
    if (hover_preview_needs_rebuffer) {
        *hover_preview_needs_rebuffer = false;
    }
    return true;
}

uint16_t rolling_u16_average(uint16_t current, uint32_t sample_ms)
{
    if (sample_ms == 0U) {
        sample_ms = 1U;
    }
    if (current == 0U) {
        return (uint16_t) sample_ms;
    }
    return (uint16_t) (((current * 7U) + sample_ms + 4U) / 8U);
}

void record_h264_foreground_decode_time(Movie *movie, uint32_t elapsed_ms)
{
    uint32_t average_ms;
    uint32_t peak_ms;

    if (!movie) {
        return;
    }

    average_ms = rolling_u16_average(movie->h264.foreground_decode_avg_ms, elapsed_ms);
    if (average_ms > 1000U) {
        average_ms = 1000U;
    }

    peak_ms = movie->h264.foreground_decode_peak_ms;
    if (elapsed_ms > peak_ms) {
        peak_ms = elapsed_ms;
    } else if (peak_ms > elapsed_ms) {
        peak_ms -= (peak_ms - elapsed_ms + 3U) / 4U;
    }
    if (peak_ms > 1000U) {
        peak_ms = 1000U;
    }

    movie->h264.foreground_decode_avg_ms = (uint16_t) average_ms;
    movie->h264.foreground_decode_peak_ms = (uint16_t) peak_ms;
}

void record_debug_displayed_frame(Movie *movie, uint32_t now_ms)
{
    uint32_t elapsed_ms;
    uint32_t fps_x10;

    if (!movie) {
        return;
    }
    if (now_ms == 0U) {
        now_ms = 1U;
    }

    if (movie->diag_display_fps_window_start_ms == 0U) {
        movie->diag_display_fps_window_start_ms = now_ms;
        movie->diag_display_fps_window_frames = 0U;
    }

    elapsed_ms = now_ms - movie->diag_display_fps_window_start_ms;
    if (elapsed_ms > DEBUG_FPS_IDLE_RESET_MS) {
        movie->diag_display_fps_window_start_ms = now_ms;
        movie->diag_display_fps_window_frames = 0U;
        elapsed_ms = 0U;
    }

    if (movie->diag_display_fps_window_frames < UINT16_MAX) {
        movie->diag_display_fps_window_frames++;
    }

    elapsed_ms = now_ms - movie->diag_display_fps_window_start_ms;
    if (elapsed_ms >= DEBUG_FPS_MIN_SAMPLE_MS) {
        fps_x10 = ((uint32_t) movie->diag_display_fps_window_frames * 10000U + (elapsed_ms / 2U)) / elapsed_ms;
        if (fps_x10 > UINT16_MAX) {
            fps_x10 = UINT16_MAX;
        }
        movie->diag_display_fps_x10 = (uint16_t) fps_x10;
    }
    if (elapsed_ms >= DEBUG_FPS_WINDOW_MS) {
        movie->diag_display_fps_window_start_ms = now_ms;
        movie->diag_display_fps_window_frames = 0U;
    }
}

bool playback_wait_key_pending(void)
{
    return
        isKeyPressed(KEY_NSPIRE_ESC) ||
        isKeyPressed(KEY_NSPIRE_ENTER) ||
        isKeyPressed(KEY_NSPIRE_SPACE) ||
        isKeyPressed(KEY_NSPIRE_TAB) ||
        isKeyPressed(KEY_NSPIRE_CAT) ||
        isKeyPressed(KEY_NSPIRE_SCRATCHPAD) ||
        isKeyPressed(KEY_NSPIRE_1) ||
        isKeyPressed(KEY_NSPIRE_2) ||
        isKeyPressed(KEY_NSPIRE_3) ||
        isKeyPressed(KEY_NSPIRE_4) ||
        isKeyPressed(KEY_NSPIRE_5) ||
        isKeyPressed(KEY_NSPIRE_6) ||
        isKeyPressed(KEY_NSPIRE_7) ||
        isKeyPressed(KEY_NSPIRE_8) ||
        isKeyPressed(KEY_NSPIRE_9) ||
        isKeyPressed(KEY_NSPIRE_LEFT) ||
        isKeyPressed(KEY_NSPIRE_RIGHT) ||
        isKeyPressed(KEY_NSPIRE_UP) ||
        isKeyPressed(KEY_NSPIRE_DOWN) ||
        isKeyPressed(KEY_NSPIRE_DIVIDE) ||
        isKeyPressed(KEY_NSPIRE_EXP) ||
        isKeyPressed(KEY_NSPIRE_TENX) ||
        isKeyPressed(KEY_NSPIRE_LP) ||
        isKeyPressed(KEY_NSPIRE_RP) ||
        isKeyPressed(KEY_NSPIRE_LTHAN) ||
        isKeyPressed(KEY_NSPIRE_GTHAN) ||
        isKeyPressed(KEY_NSPIRE_PLUS) ||
        isKeyPressed(KEY_NSPIRE_MINUS) ||
        isKeyPressed(KEY_NSPIRE_F) ||
        isKeyPressed(KEY_NSPIRE_T) ||
        isKeyPressed(KEY_NSPIRE_M) ||
        isKeyPressed(KEY_NSPIRE_D) ||
        isKeyPressed(KEY_NSPIRE_S) ||
        isKeyPressed(KEY_NSPIRE_C) ||
        isKeyPressed(KEY_NSPIRE_P) ||
        isKeyPressed(KEY_NSPIRE_R) ||
        on_key_pressed();
}

static bool key_snapshot_new_press(t_key key, bool *previous)
{
    bool down = isKeyPressed(key) ? true : false;
    bool pressed = down && !*previous;

    *previous = down;
    return pressed;
}

static bool on_key_snapshot_new_press(bool *previous)
{
    bool down = on_key_pressed() ? true : false;
    bool pressed = down && !*previous;

    *previous = down;
    return pressed;
}

void playback_key_snapshot_init(PlaybackKeySnapshot *snapshot)
{
    if (!snapshot) {
        return;
    }

    snapshot->esc = isKeyPressed(KEY_NSPIRE_ESC);
    snapshot->enter = isKeyPressed(KEY_NSPIRE_ENTER);
    snapshot->space = isKeyPressed(KEY_NSPIRE_SPACE);
    snapshot->tab = isKeyPressed(KEY_NSPIRE_TAB);
    snapshot->cat = isKeyPressed(KEY_NSPIRE_CAT);
    snapshot->scratchpad = isKeyPressed(KEY_NSPIRE_SCRATCHPAD);
    snapshot->keypad_1 = isKeyPressed(KEY_NSPIRE_1);
    snapshot->keypad_2 = isKeyPressed(KEY_NSPIRE_2);
    snapshot->keypad_3 = isKeyPressed(KEY_NSPIRE_3);
    snapshot->keypad_4 = isKeyPressed(KEY_NSPIRE_4);
    snapshot->keypad_5 = isKeyPressed(KEY_NSPIRE_5);
    snapshot->keypad_6 = isKeyPressed(KEY_NSPIRE_6);
    snapshot->keypad_7 = isKeyPressed(KEY_NSPIRE_7);
    snapshot->keypad_8 = isKeyPressed(KEY_NSPIRE_8);
    snapshot->keypad_9 = isKeyPressed(KEY_NSPIRE_9);
    snapshot->left = isKeyPressed(KEY_NSPIRE_LEFT);
    snapshot->right = isKeyPressed(KEY_NSPIRE_RIGHT);
    snapshot->up = isKeyPressed(KEY_NSPIRE_UP);
    snapshot->down = isKeyPressed(KEY_NSPIRE_DOWN);
    snapshot->divide = isKeyPressed(KEY_NSPIRE_DIVIDE);
    snapshot->exp = isKeyPressed(KEY_NSPIRE_EXP);
    snapshot->tenx = isKeyPressed(KEY_NSPIRE_TENX);
    snapshot->lp = isKeyPressed(KEY_NSPIRE_LP);
    snapshot->rp = isKeyPressed(KEY_NSPIRE_RP);
    snapshot->lthan = isKeyPressed(KEY_NSPIRE_LTHAN);
    snapshot->gthan = isKeyPressed(KEY_NSPIRE_GTHAN);
    snapshot->plus = isKeyPressed(KEY_NSPIRE_PLUS);
    snapshot->minus = isKeyPressed(KEY_NSPIRE_MINUS);
    snapshot->f = isKeyPressed(KEY_NSPIRE_F);
    snapshot->t = isKeyPressed(KEY_NSPIRE_T);
    snapshot->m = isKeyPressed(KEY_NSPIRE_M);
    snapshot->d = isKeyPressed(KEY_NSPIRE_D);
    snapshot->s = isKeyPressed(KEY_NSPIRE_S);
    snapshot->c = isKeyPressed(KEY_NSPIRE_C);
    snapshot->p = isKeyPressed(KEY_NSPIRE_P);
    snapshot->r = isKeyPressed(KEY_NSPIRE_R);
    snapshot->on = on_key_pressed() ? true : false;
}

bool playback_key_snapshot_new_press(PlaybackKeySnapshot *snapshot)
{
    bool pending = false;

    if (!snapshot) {
        return playback_wait_key_pending();
    }

    pending = key_snapshot_new_press(KEY_NSPIRE_ESC, &snapshot->esc) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_ENTER, &snapshot->enter) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_SPACE, &snapshot->space) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_TAB, &snapshot->tab) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_CAT, &snapshot->cat) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_SCRATCHPAD, &snapshot->scratchpad) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_1, &snapshot->keypad_1) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_2, &snapshot->keypad_2) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_3, &snapshot->keypad_3) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_4, &snapshot->keypad_4) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_5, &snapshot->keypad_5) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_6, &snapshot->keypad_6) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_7, &snapshot->keypad_7) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_8, &snapshot->keypad_8) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_9, &snapshot->keypad_9) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_LEFT, &snapshot->left) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_RIGHT, &snapshot->right) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_UP, &snapshot->up) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_DOWN, &snapshot->down) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_DIVIDE, &snapshot->divide) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_EXP, &snapshot->exp) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_TENX, &snapshot->tenx) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_LP, &snapshot->lp) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_RP, &snapshot->rp) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_LTHAN, &snapshot->lthan) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_GTHAN, &snapshot->gthan) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_PLUS, &snapshot->plus) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_MINUS, &snapshot->minus) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_F, &snapshot->f) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_T, &snapshot->t) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_M, &snapshot->m) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_D, &snapshot->d) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_S, &snapshot->s) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_C, &snapshot->c) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_P, &snapshot->p) || pending;
    pending = key_snapshot_new_press(KEY_NSPIRE_R, &snapshot->r) || pending;
    pending = on_key_snapshot_new_press(&snapshot->on) || pending;
    return pending;
}

bool playback_wait_touchpad_pending(const PointerState *pointer)
{
    touchpad_report_t report;
    bool current_down;
    bool has_touch_position;
    int dx;
    int dy;

    if (!pointer || !pointer->info || touchpad_scan(&report) != 0) {
        return false;
    }
    current_down = (report.pressed && report.arrow == TPAD_ARROW_CLICK) ? true : false;
    if (current_down != pointer->down) {
        return true;
    }
    has_touch_position =
        (report.contact || report.proximity) &&
        report.x < pointer->info->width &&
        report.y < pointer->info->height;
    if (!has_touch_position) {
        return pointer->tracking;
    }
    if (!pointer->tracking) {
        return true;
    }
    dx = (int) report.x - pointer->last_touch_x;
    dy = (int) report.y - pointer->last_touch_y;
    if (dx < 0) {
        dx = -dx;
    }
    if (dy < 0) {
        dy = -dy;
    }
    return dx > POINTER_JITTER_THRESHOLD || dy > POINTER_JITTER_THRESHOLD;
}

bool playback_wait_touchpad_click_pending(const PointerState *pointer)
{
    touchpad_report_t report;
    bool current_down;

    if (!pointer || !pointer->info || touchpad_scan(&report) != 0) {
        return false;
    }
    current_down = (report.pressed && report.arrow == TPAD_ARROW_CLICK) ? true : false;
    return current_down && !pointer->down;
}

bool playback_wait_input_pending(const PointerState *pointer)
{
    return playback_wait_key_pending() || playback_wait_touchpad_pending(pointer);
}

bool prefetch_abort_requested(const PointerState *pointer)
{
    return pointer && playback_wait_input_pending(pointer);
}

void wait_until_ticks_playback(uint64_t target_ticks, const PointerState *pointer)
{
    uint64_t poll_interval_ticks = ((uint64_t) monotonic_clock_ticks_per_second()) / 1000U;
    uint64_t next_poll_ticks = monotonic_clock_now_ticks();
    uint64_t now_ticks = next_poll_ticks;

    if (poll_interval_ticks == 0) {
        poll_interval_ticks = 1;
    }
    while ((int64_t) (target_ticks - now_ticks) > 0) {
        if ((int64_t) (now_ticks - next_poll_ticks) >= 0) {
            if (playback_wait_input_pending(pointer)) {
                break;
            }
            next_poll_ticks = now_ticks + poll_interval_ticks;
        }
        now_ticks = monotonic_clock_now_ticks();
    }
}

void free_movie_files(MovieFile *files, size_t count)
{
    size_t index;
    if (!files) {
        return;
    }
    for (index = 0; index < count; ++index) {
        free(files[index].name);
        free(files[index].detail);
        free(files[index].path);
    }
    free(files);
}

void clear_movie_picker_cache(MoviePickerCache *cache)
{
    if (!cache) {
        return;
    }
    free_movie_files(cache->files, cache->count);
    memset(cache, 0, sizeof(*cache));
}

void free_history_store(HistoryStore *history)
{
    size_t index;
    if (!history) {
        return;
    }
    free(history->default_settings.path);
    history->default_settings.path = NULL;
    history->has_default_settings = false;
    for (index = 0; index < history->count && index < HISTORY_MAX_ENTRIES; ++index) {
        free(history->entries[index].path);
        history->entries[index].path = NULL;
    }
    history->count = 0;
}

