#include "player_internal.h"

typedef struct {
    bool active;
    bool off_when_complete;
    bool off_was_paused;
    uint32_t started_ms;
    uint32_t from_raw;
    uint32_t to_raw;
    uint32_t off_saved_brightness;
    unsigned from_percent;
    unsigned to_percent;
    unsigned current_percent;
} BrightnessAnimation;

static void format_brightness_status(char *buffer, size_t buffer_size, unsigned percent)
{
    snprintf(buffer, buffer_size, "BRIGHT %3u%%", percent);
}

static uint32_t mix_u32(uint32_t from, uint32_t to, uint8_t mix)
{
    if (to >= from) {
        return from + (uint32_t) ((((uint64_t) (to - from) * mix) + 127U) / 255U);
    }
    return from - (uint32_t) ((((uint64_t) (from - to) * mix) + 127U) / 255U);
}

static unsigned mix_percent(unsigned from, unsigned to, uint8_t mix)
{
    if (to >= from) {
        return from + (unsigned) ((((uint32_t) (to - from) * mix) + 127U) / 255U);
    }
    return from - (unsigned) ((((uint32_t) (from - to) * mix) + 127U) / 255U);
}

static void brightness_animation_begin(
    BrightnessAnimation *animation,
    uint32_t now_ms,
    uint32_t from_raw,
    uint32_t to_raw,
    bool off_when_complete,
    uint32_t off_saved_brightness,
    bool off_was_paused
)
{
    if (!animation) {
        return;
    }

    animation->from_percent = animation->active
        ? animation->current_percent
        : lcd_brightness_percent(from_raw);
    animation->active = true;
    animation->off_when_complete = off_when_complete;
    animation->off_was_paused = off_was_paused;
    animation->started_ms = now_ms ? now_ms : 1U;
    animation->from_raw = from_raw;
    animation->to_raw = to_raw;
    animation->off_saved_brightness = off_saved_brightness;
    animation->to_percent = lcd_brightness_percent(to_raw);
    animation->current_percent = animation->from_percent;
}

static void brightness_animation_cancel(BrightnessAnimation *animation)
{
    if (!animation) {
        return;
    }
    memset(animation, 0, sizeof(*animation));
}

static uint32_t brightness_animation_logical_raw(const BrightnessAnimation *animation)
{
    return animation && animation->active ? animation->to_raw : current_lcd_brightness();
}

static void brightness_animation_tick(
    BrightnessAnimation *animation,
    SDL_Surface *screen,
    uint32_t now_ms,
    char *status_overlay_text,
    size_t status_overlay_text_size
)
{
    uint32_t elapsed_ms;
    uint8_t mix;
    uint32_t raw;
    unsigned percent;

    if (!animation || !animation->active) {
        return;
    }

    elapsed_ms = now_ms - animation->started_ms;
    if (elapsed_ms >= LCD_BRIGHTNESS_FADE_MS) {
        set_lcd_brightness((int) animation->to_raw);
        animation->current_percent = animation->to_percent;
        format_brightness_status(status_overlay_text, status_overlay_text_size, animation->to_percent);
        if (animation->off_when_complete) {
            display_power_off_with_saved_brightness(
                &g_display_power_state,
                screen,
                animation->off_saved_brightness,
                animation->off_was_paused
            );
        }
        brightness_animation_cancel(animation);
        return;
    }

    mix = ui_ease_smoothstep(elapsed_ms, LCD_BRIGHTNESS_FADE_MS);
    raw = mix_u32(animation->from_raw, animation->to_raw, mix);
    percent = mix_percent(animation->from_percent, animation->to_percent, mix);
    set_lcd_brightness((int) raw);
    animation->current_percent = percent;
    format_brightness_status(status_overlay_text, status_overlay_text_size, percent);
}

int play_movie(
    SDL_Surface *screen,
    const Fonts *fonts,
    const char *path,
    char *next_path,
    size_t next_path_size,
    bool resume_without_prompt,
    bool loading_already_open
)
{
    static Movie movie;
    bool prev_enter = false;
    bool prev_space = false;
    bool prev_left = false;
    bool prev_right = false;
    bool prev_up = false;
    bool prev_down = false;
    bool prev_1 = false;
    bool prev_2 = false;
    bool prev_3 = false;
    bool prev_4 = false;
    bool prev_5 = false;
    bool prev_6 = false;
    bool prev_7 = false;
    bool prev_8 = false;
    bool prev_9 = false;
    bool prev_tab = false;
    bool prev_esc = false;
    bool prev_scratchpad = false;
    bool prev_cat = false;
    bool prev_divide = false;
    bool prev_exp = false;
    bool prev_tenx = false;
    bool prev_lp = false;
    bool prev_rp = false;
    bool prev_lthan = false;
    bool prev_gthan = false;
    bool prev_f = false;
    bool prev_t = false;
    bool prev_m = false;
    bool prev_d = false;
    bool prev_s = false;
    bool prev_p = false;
    bool prev_r = false;
    bool prev_c = false;
    bool prev_plus = false;
    bool prev_minus = false;
    bool prev_on = false;
    bool paused = false;
    bool esc_exit_suppressed_until_release = false;
    uint64_t frame_interval_ticks;
    uint64_t next_frame_due_ticks;
    uint64_t playback_anchor_ticks;
    uint32_t playback_anchor_frame;
    uint32_t tab_hold_repeat_interval_ms;
    uint32_t tab_repeat_next_ms = 0;
    uint32_t seek_repeat_next_ms = 0;
    uint32_t brightness_repeat_next_ms = 0;
    uint32_t speed_repeat_next_ms = 0;
    uint32_t ui_visible_until;
    uint32_t resume_input_guard_until_ms = 0;
    uint32_t subtitle_font_overlay_until = 0;
    int result = 0;
    int seek_repeat_direction = 0;
    int brightness_repeat_direction = 0;
    int speed_repeat_direction = 0;
    ScaleMode scale_mode = SCALE_FIT;
    size_t playback_rate_index = PLAYBACK_RATE_DEFAULT_INDEX;
    size_t subtitle_font_index = SUBTITLE_FONT_DEFAULT_INDEX;
    PlaybackMode playback_mode = PLAYBACK_MODE_AUTO_NEXT;
    MemoryOverlayMode memory_overlay_mode = MEMORY_OVERLAY_OFF;
    bool realtime_frame_skip = false;
    int subtitle_size = 0;
    SubtitlePlacement subtitle_placement = SUBTITLE_POS_BAR_BOTTOM;
    VideoAlign video_align_x = VIDEO_ALIGN_CENTER;
    VideoAlign video_align_y = VIDEO_ALIGN_CENTER;
    char playback_title[128] = {0};
    char playback_detail[128] = {0};
    char status_overlay_text[64] = {0};
    uint32_t status_overlay_started_ms = 0;
    uint32_t status_overlay_until = 0;
    SubtitleSurfaceCache subtitle_cache;
    ScreenshotPreviewState screenshot_preview;
    SeekBarPreviewState seek_preview;
    PlaybackUiTransitions ui_transitions;
    PlaybackUiMixes ui_mixes;
    ScaleMorphState scale_morph;
    BrightnessAnimation brightness_animation;
    PlaybackPressTarget playback_press_target = PLAYBACK_PRESS_NONE;
    PointerState pointer;
    bool help_menu_open = false;
    bool help_resume_playback = false;
    uint32_t resume_frame = 0;
    HistoryStore startup_history;
    int startup_history_index = -1;
    bool startup_has_resume = false;
    bool resume_prompt_returned = false;
    int32_t pending_seek_ms = 0;
    uint32_t pending_seek_commit_at_ms = 0;
    int32_t seek_badge_ms = 0;
    uint32_t seek_badge_started_ms = 0;
    uint32_t seek_badge_hide_elapsed_ms = 0;
    uint32_t seek_badge_last_render_ms = 0;
    uint32_t paused_ui_quiet_until_ms = 0;
    uint32_t playback_input_prefetch_quiet_until_ms = 0;
    uint32_t playback_badge_press_until_ms = 0;
    uint32_t scale_badge_press_until_ms = 0;
    uint32_t speed_badge_press_until_ms = 0;
    bool playback_pause_key_press_active = false;
    bool playback_pointer_press_active = false;
    bool hover_preview_needs_rebuffer = false;
    SDL_Surface *loading_snapshot = NULL;
    LoadingProgress loading_progress;
    const char *movie_filename = path;
    char *playback_title_alloc = NULL;
    char *playback_detail_alloc = NULL;

    memset(&subtitle_cache, 0, sizeof(subtitle_cache));
    memset(&screenshot_preview, 0, sizeof(screenshot_preview));
    memset(&seek_preview, 0, sizeof(seek_preview));
    memset(&ui_transitions, 0, sizeof(ui_transitions));
    memset(&ui_mixes, 0, sizeof(ui_mixes));
    memset(&scale_morph, 0, sizeof(scale_morph));
    memset(&brightness_animation, 0, sizeof(brightness_animation));
    seek_preview.decoded_chunk_index = -1;
    seek_preview.decoded_frame_index = UINT32_MAX;
    seek_preview.last_pointer_x = -1;
    seek_preview.decode_job.chunk_index = -1;
    if (strrchr(path, '/')) {
        movie_filename = strrchr(path, '/') + 1;
    } else if (strrchr(path, '\\')) {
        movie_filename = strrchr(path, '\\') + 1;
    }
    if (movie_display_fields_for_filename(movie_filename ? movie_filename : "", &playback_title_alloc, &playback_detail_alloc)) {
        snprintf(playback_title, sizeof(playback_title), "%s", playback_title_alloc);
        if (playback_detail_alloc) {
            snprintf(playback_detail, sizeof(playback_detail), "%s", playback_detail_alloc);
        }
        free(playback_title_alloc);
        free(playback_detail_alloc);
    } else {
        snprintf(playback_title, sizeof(playback_title), "%s", movie_filename ? movie_filename : "");
    }

    if (debug_is_runtime_logging_enabled()) {
        g_debug_ring_count = 0;
        g_debug_ring_next = 0;
    } else {
        release_debug_ring_storage();
    }
    debug_clear_last_error();
    if (loading_already_open) {
        loading_snapshot = create_black_screen_snapshot(screen);
        if (!loading_snapshot) {
            loading_snapshot = capture_screen_surface(screen);
        }
    } else {
        loading_snapshot = capture_screen_surface(screen);
        animate_loading_transition(screen, loading_snapshot, fonts, "Loading", true);
    }
    loading_progress_init(&loading_progress, screen, loading_snapshot, fonts, "Loading", true);
    loading_progress_tick(&loading_progress, true);
    cleanup_deferred_playback_movie();
    loading_progress_tick(&loading_progress, false);
    flush_queued_history_save(&g_pending_history_save, "loading");
    loading_progress_tick(&loading_progress, false);
    flush_queued_theme_save("loading");
    loading_progress_tick(&loading_progress, false);
    if (!load_movie(path, &movie, &loading_progress)) {
        finish_loading_transition(screen, &loading_snapshot, fonts, "Loading");
        report_movie_open_failure(path);
        return -1;
    }
    if (load_history_store(path, &startup_history)) {
        ui_set_theme(startup_history.theme_id);
        if (startup_history.has_default_settings) {
            apply_history_entry_settings(
                &startup_history.default_settings,
                &movie,
                &scale_mode,
                &playback_rate_index,
                &playback_mode,
                &realtime_frame_skip,
                &subtitle_font_index,
                &subtitle_size,
                &subtitle_placement,
                &video_align_x,
                &video_align_y
            );
        }
        startup_history_index = history_find_entry_index(&startup_history, path);
        if (startup_history_index >= 0) {
            if (startup_history.has_default_settings) {
                apply_history_entry_subtitle_track(&startup_history.entries[startup_history_index], &movie);
            } else {
                apply_history_entry_settings(
                    &startup_history.entries[startup_history_index],
                    &movie,
                    &scale_mode,
                    &playback_rate_index,
                    &playback_mode,
                    &realtime_frame_skip,
                    &subtitle_font_index,
                    &subtitle_size,
                    &subtitle_placement,
                    &video_align_x,
                    &video_align_y
                );
                playback_mode = PLAYBACK_MODE_AUTO_NEXT;
            }
            if (startup_history.entries[startup_history_index].has_resume) {
                startup_has_resume = true;
                resume_frame = startup_history.entries[startup_history_index].frame;
            }
        }
        free_history_store(&startup_history);
    }
    if (startup_history_index < 0 && selected_subtitle_track_supports_auto_positioning(&movie)) {
        subtitle_placement = SUBTITLE_POS_AUTO;
    }
    subtitle_placement = subtitle_normalize_placement(
        subtitle_placement,
        selected_subtitle_track_supports_auto_positioning(&movie)
    );
    if (startup_has_resume && resume_frame < movie.header.frame_count) {
        int resume_choice = 1;

        if (!resume_without_prompt) {
            resume_choice = prompt_resume_position(
                screen,
                fonts,
                &movie,
                path,
                resume_frame,
                scale_mode,
                video_align_x,
                video_align_y,
                &loading_snapshot
            );
            if (resume_choice == RESUME_PROMPT_RESULT_HOME_EXIT ||
                resume_choice == RESUME_PROMPT_RESULT_SCRATCHPAD_EXIT) {
                if (loading_snapshot) {
                    SDL_FreeSurface(loading_snapshot);
                    loading_snapshot = NULL;
                }
                defer_playback_movie_cleanup(&movie);
                return resume_choice == RESUME_PROMPT_RESULT_SCRATCHPAD_EXIT
                    ? PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT
                    : PLAY_MOVIE_RESULT_HOME_EXIT;
            }
            if (resume_choice < 0) {
                if (debug_is_runtime_logging_enabled()) {
                    char log_path[MAX_PATH_LEN];
                    debug_log_path_for_movie(path, log_path, sizeof(log_path));
                    debug_dump_session(log_path, &movie, "resume-cancel");
                }
                defer_playback_movie_cleanup(&movie);
                return 0;
            }
            resume_prompt_returned = true;
        }
        if (resume_choice == 0) {
            if (movie.current_frame != 0) {
                decode_to_frame(&movie, 0);
            }
        } else {
            if (resume_without_prompt && !decode_to_frame(&movie, resume_frame)) {
                finish_loading_transition(screen, &loading_snapshot, fonts, "Loading");
                report_movie_decode_failure(&movie, path, "direct resume");
                destroy_movie(&movie);
                return PLAY_MOVIE_RESULT_ERROR;
            }
            snprintf(status_overlay_text, sizeof(status_overlay_text), "RESUMED");
            status_overlay_show(monotonic_clock_now_ms(), true, &status_overlay_started_ms, &status_overlay_until);
        }
    }
    if (loading_snapshot) {
        draw_movie_frame_background(loading_snapshot, &movie, scale_mode, video_align_x, video_align_y, NULL, NULL);
    }
    finish_loading_transition(screen, &loading_snapshot, fonts, "Loading");
    pointer_init(&pointer);
    pointer_update(&pointer);
    display_power_restore(&g_display_power_state, monotonic_clock_now_ms());
    prev_enter = isKeyPressed(KEY_NSPIRE_ENTER);
    prev_space = isKeyPressed(KEY_NSPIRE_SPACE);
    prev_tab = isKeyPressed(KEY_NSPIRE_TAB);
    prev_up = isKeyPressed(KEY_NSPIRE_UP);
    prev_down = isKeyPressed(KEY_NSPIRE_DOWN);
    prev_1 = isKeyPressed(KEY_NSPIRE_1);
    prev_2 = isKeyPressed(KEY_NSPIRE_2);
    prev_3 = isKeyPressed(KEY_NSPIRE_3);
    prev_4 = isKeyPressed(KEY_NSPIRE_4);
    prev_5 = isKeyPressed(KEY_NSPIRE_5);
    prev_6 = isKeyPressed(KEY_NSPIRE_6);
    prev_7 = isKeyPressed(KEY_NSPIRE_7);
    prev_8 = isKeyPressed(KEY_NSPIRE_8);
    prev_9 = isKeyPressed(KEY_NSPIRE_9);
    prev_t = isKeyPressed(KEY_NSPIRE_T);
    prev_m = isKeyPressed(KEY_NSPIRE_M);
    prev_d = isKeyPressed(KEY_NSPIRE_D);
    prev_s = isKeyPressed(KEY_NSPIRE_S);
    prev_p = isKeyPressed(KEY_NSPIRE_P);
    prev_r = isKeyPressed(KEY_NSPIRE_R);
    prev_c = isKeyPressed(KEY_NSPIRE_C);
    prev_esc = isKeyPressed(KEY_NSPIRE_ESC);
    prev_scratchpad = isKeyPressed(KEY_NSPIRE_SCRATCHPAD);
    prev_on = on_key_pressed() ? true : false;
    debug_set_metrics_collection(debug_is_runtime_logging_enabled());
    frame_interval_ticks = movie_frame_interval_ticks(&movie);
    tab_hold_repeat_interval_ms = tab_hold_frame_repeat_interval_ms(&movie);
    if (!resume_prompt_returned) {
        prefetch_tick(&movie, true, 1000, NULL);
    }
    debug_tracef(
        "play start path=%s frames=%lu chunks=%lu frame=%lu chunk=%d",
        path,
        (unsigned long) movie.header.frame_count,
        (unsigned long) movie.header.chunk_count,
        (unsigned long) movie.current_frame,
        movie.loaded_chunk
    );
    debug_log_sram_status();
    playback_anchor_ticks = monotonic_clock_now_ticks();
    playback_anchor_frame = movie.current_frame;
    next_frame_due_ticks = playback_anchor_ticks;
    reset_playback_timeline(
        &movie,
        playback_rate_for_index(playback_rate_index),
        &playback_anchor_ticks,
        &playback_anchor_frame,
        &next_frame_due_ticks
    );
    ui_visible_until = monotonic_clock_now_ms() + POINTER_UI_TIMEOUT_MS;
    if (resume_prompt_returned || resume_without_prompt) {
        resume_input_guard_until_ms = monotonic_clock_now_ms() + RESUME_INPUT_GUARD_MS;
    }

    while (1) {
        bool woke_from_idle_off = false;
        bool esc_down = isKeyPressed(KEY_NSPIRE_ESC) ? true : false;
        bool scratchpad_down = isKeyPressed(KEY_NSPIRE_SCRATCHPAD) ? true : false;
        bool esc_edge = esc_down && !prev_esc;
        bool scratchpad_edge = scratchpad_down && !prev_scratchpad;
        prev_esc = esc_down;
        prev_scratchpad = scratchpad_down;
        if (!esc_down) {
            esc_exit_suppressed_until_release = false;
        }
        if ((scratchpad_edge || esc_down) &&
            (g_display_power_state.idle_dim_active ||
                (g_display_power_state.off && g_display_power_state.off_from_idle))) {
            woke_from_idle_off = g_display_power_state.off && g_display_power_state.off_from_idle;
            display_power_restore(&g_display_power_state, monotonic_clock_now_ms());
        }
        if (scratchpad_edge) {
            display_power_off_for_exit(&g_display_power_state, screen, true);
            result = PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT;
            break;
        }
        if (g_display_power_state.off && !g_display_power_state.off_from_idle && esc_down) {
            result = PLAY_MOVIE_RESULT_HOME_EXIT;
            break;
        }
        if (!g_display_power_state.off && !help_menu_open && esc_down && !esc_exit_suppressed_until_release) {
            SDL_Surface *collapse_overlay = capture_screen_surface(screen);

            animate_movie_collapse_to_black(
                screen,
                &movie,
                scale_mode,
                video_align_x,
                video_align_y,
                &collapse_overlay
            );
            break;
        }

        bool touchpad_click = pointer_update(&pointer);
        bool pointer_click = touchpad_click;
        bool pending_seek_consumed_click = false;
        uint64_t now_ticks = monotonic_clock_now_ticks();
        uint32_t now_ms = monotonic_clock_ticks_to_ms(now_ticks);
        const PlaybackRate *playback_rate = playback_rate_for_index(playback_rate_index);
        bool show_ui = help_menu_open || paused || (now_ms <= ui_visible_until);
        bool ctrl_down = isKeyPressed(KEY_NSPIRE_CTRL) ? true : false;
        bool enter_key_was_down = prev_enter;
        bool keypad_5_was_down = prev_5;
        bool keypad_1_edge = key_pressed_edge(KEY_NSPIRE_1, &prev_1);
        bool keypad_2_edge = key_pressed_edge(KEY_NSPIRE_2, &prev_2);
        bool keypad_3_edge = key_pressed_edge(KEY_NSPIRE_3, &prev_3);
        bool keypad_4_edge = key_pressed_edge(KEY_NSPIRE_4, &prev_4);
        bool keypad_5_edge = key_pressed_edge(KEY_NSPIRE_5, &prev_5);
        bool keypad_6_edge = key_pressed_edge(KEY_NSPIRE_6, &prev_6);
        bool keypad_7_edge = key_pressed_edge(KEY_NSPIRE_7, &prev_7);
        bool keypad_8_edge = key_pressed_edge(KEY_NSPIRE_8, &prev_8);
        bool keypad_9_edge = key_pressed_edge(KEY_NSPIRE_9, &prev_9);
        bool previous_video_edge = !ctrl_down && keypad_7_edge;
        bool next_video_edge = !ctrl_down && keypad_9_edge;
        bool enter_edge = key_pressed_edge(KEY_NSPIRE_ENTER, &prev_enter) || (!ctrl_down && keypad_5_edge);
        bool enter_down = prev_enter || (!ctrl_down && prev_5);
        bool enter_was_down = enter_key_was_down || (!ctrl_down && keypad_5_was_down);
        bool enter_release_edge = !enter_down && enter_was_down;
        bool space_edge = key_pressed_edge(KEY_NSPIRE_SPACE, &prev_space);
        bool space_down = prev_space;
        bool tab_edge = key_pressed_edge(KEY_NSPIRE_TAB, &prev_tab);
        bool tab_down = prev_tab;
        bool cat_edge = key_pressed_edge(KEY_NSPIRE_CAT, &prev_cat);
        bool esc_exit_request;
        bool divide_edge = key_pressed_edge(KEY_NSPIRE_DIVIDE, &prev_divide);
        bool divide_down = prev_divide;
        bool exp_edge = key_pressed_edge(KEY_NSPIRE_EXP, &prev_exp);
        bool tenx_edge = key_pressed_edge(KEY_NSPIRE_TENX, &prev_tenx);
        bool lp_edge = key_pressed_edge(KEY_NSPIRE_LP, &prev_lp);
        bool rp_edge = key_pressed_edge(KEY_NSPIRE_RP, &prev_rp);
        bool lthan_edge = key_pressed_edge(KEY_NSPIRE_LTHAN, &prev_lthan);
        bool gthan_edge = key_pressed_edge(KEY_NSPIRE_GTHAN, &prev_gthan);
        bool speed_down_edge = lp_edge || lthan_edge;
        bool speed_up_edge = rp_edge || gthan_edge;
        bool speed_down_down = prev_lp || prev_lthan;
        bool speed_up_down = prev_rp || prev_gthan;
        bool speed_key_down = speed_down_down || speed_up_down;
        bool seek_left_edge = key_pressed_edge(KEY_NSPIRE_LEFT, &prev_left) || (!ctrl_down && keypad_4_edge);
        bool seek_right_edge = key_pressed_edge(KEY_NSPIRE_RIGHT, &prev_right) || (!ctrl_down && keypad_6_edge);
        bool seek_left_down = prev_left || (!ctrl_down && prev_4);
        bool seek_right_down = prev_right || (!ctrl_down && prev_6);
        bool brightness_up_edge = key_pressed_edge(KEY_NSPIRE_UP, &prev_up) || (!ctrl_down && keypad_8_edge);
        bool brightness_down_edge = key_pressed_edge(KEY_NSPIRE_DOWN, &prev_down) || (!ctrl_down && keypad_2_edge);
        bool brightness_up_down = prev_up || (!ctrl_down && prev_8);
        bool brightness_down_down = prev_down || (!ctrl_down && prev_2);
        bool video_down_left_edge = ctrl_down && keypad_1_edge;
        bool video_down_edge = ctrl_down && keypad_2_edge;
        bool video_down_right_edge = ctrl_down && keypad_3_edge;
        bool video_left_edge = ctrl_down && keypad_4_edge;
        bool video_center_edge = ctrl_down && keypad_5_edge;
        bool video_right_edge = ctrl_down && keypad_6_edge;
        bool video_up_left_edge = ctrl_down && keypad_7_edge;
        bool video_up_edge = ctrl_down && keypad_8_edge;
        bool video_up_right_edge = ctrl_down && keypad_9_edge;
        bool subtitle_font_edge = key_pressed_edge(KEY_NSPIRE_F, &prev_f);
        bool subtitle_track_edge = key_pressed_edge(KEY_NSPIRE_T, &prev_t);
        bool memory_overlay_edge = key_pressed_edge(KEY_NSPIRE_M, &prev_m);
        bool debug_logging_edge = key_pressed_edge(KEY_NSPIRE_D, &prev_d);
        bool screenshot_edge = key_pressed_edge(KEY_NSPIRE_S, &prev_s);
        bool playback_mode_edge = key_pressed_edge(KEY_NSPIRE_P, &prev_p);
        bool frame_skip_mode_edge = key_pressed_edge(KEY_NSPIRE_R, &prev_r);
        bool theme_edge = key_pressed_edge(KEY_NSPIRE_C, &prev_c);
        bool subtitle_size_up_edge = key_pressed_edge(KEY_NSPIRE_PLUS, &prev_plus);
        bool subtitle_size_down_edge = key_pressed_edge(KEY_NSPIRE_MINUS, &prev_minus);
        bool on_edge = on_key_pressed_edge(&prev_on);
        bool take_screenshot = false;
        bool tab_repeat_step = false;
        bool restart_after_pause = false;
        bool playback_badge_press_triggered = false;
        int seek_delta_ms = 0;
        int brightness_delta = 0;
        int speed_delta = 0;
        bool pointer_release_edge;
        bool show_ui_before_pointer_activation;
        bool input_activity;

        esc_exit_request = esc_edge || (!help_menu_open && esc_down && !esc_exit_suppressed_until_release);

        if (resume_input_guard_until_ms != 0U) {
            if (ui_time_before(now_ms, resume_input_guard_until_ms)) {
                pointer_click = false;
                enter_edge = false;
                enter_release_edge = false;
                space_edge = false;
                tab_edge = false;
            } else {
                resume_input_guard_until_ms = 0;
            }
        }
        if (enter_edge && pointer.visible) {
            pointer_click = true;
        }
        pointer_release_edge = pointer.release_edge || (enter_release_edge && pointer.visible);
        input_activity =
            pointer_click ||
            pointer_release_edge ||
            pointer.moved ||
            pointer.down ||
            enter_edge ||
            enter_release_edge ||
            enter_down ||
            space_edge ||
            space_down ||
            tab_edge ||
            tab_down ||
            cat_edge ||
            esc_edge ||
            esc_down ||
            divide_edge ||
            divide_down ||
            speed_key_down ||
            seek_left_edge ||
            seek_right_edge ||
            seek_left_down ||
            seek_right_down ||
            previous_video_edge ||
            next_video_edge ||
            brightness_up_edge ||
            brightness_down_edge ||
            brightness_up_down ||
            brightness_down_down ||
            video_down_left_edge ||
            video_down_edge ||
            video_down_right_edge ||
            video_left_edge ||
            video_center_edge ||
            video_right_edge ||
            video_up_left_edge ||
            video_up_edge ||
            video_up_right_edge ||
            subtitle_font_edge ||
            subtitle_track_edge ||
            subtitle_size_up_edge ||
            subtitle_size_down_edge ||
            memory_overlay_edge ||
            debug_logging_edge ||
            screenshot_edge ||
            playback_mode_edge ||
            frame_skip_mode_edge ||
            theme_edge ||
            on_edge;
        if ((g_display_power_state.idle_dim_active ||
                (g_display_power_state.off && g_display_power_state.off_from_idle)) &&
            input_activity) {
            woke_from_idle_off = g_display_power_state.off && g_display_power_state.off_from_idle;
            display_power_restore(&g_display_power_state, now_ms);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (input_activity) {
            display_power_note_activity(&g_display_power_state, now_ms);
        }
        if (pointer_click ||
            pointer_release_edge ||
            enter_edge ||
            enter_release_edge ||
            space_edge ||
            tab_edge ||
            cat_edge ||
            divide_edge ||
            speed_down_edge ||
            speed_up_edge ||
            seek_left_edge ||
            seek_right_edge ||
            previous_video_edge ||
            next_video_edge ||
            brightness_up_edge ||
            brightness_down_edge ||
            screenshot_edge ||
            playback_mode_edge ||
            frame_skip_mode_edge ||
            theme_edge) {
            playback_input_prefetch_quiet_until_ms = now_ms + PLAYBACK_INPUT_PREFETCH_GRACE_MS;
        }
        if (!space_down) {
            playback_pause_key_press_active = false;
        }
        if (!pointer.down && !enter_down) {
            playback_pointer_press_active = false;
        }
        if (divide_down || speed_key_down) {
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }

        if (!g_display_power_state.off && theme_edge) {
            ui_cycle_theme();
            ui_save_theme_for_movie(path);
            snprintf(status_overlay_text, sizeof(status_overlay_text), "THEME %s", ui_theme_name(g_ui_theme_id));
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            show_ui = true;
        }

        if (g_display_power_state.off) {
            bool off_input_activity =
                pointer_click ||
                pointer_release_edge ||
                pointer.moved ||
                enter_edge ||
                enter_release_edge ||
                space_edge ||
                tab_edge ||
                cat_edge ||
                divide_edge ||
                speed_down_edge ||
                speed_up_edge ||
                seek_left_edge ||
                seek_right_edge ||
                previous_video_edge ||
                next_video_edge ||
                brightness_up_edge ||
                brightness_down_edge ||
                screenshot_edge ||
                playback_mode_edge ||
                frame_skip_mode_edge ||
                theme_edge ||
                esc_edge ||
                on_edge;

            if (g_display_power_state.off_from_idle && off_input_activity) {
                woke_from_idle_off = true;
                display_power_restore(&g_display_power_state, now_ms);
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            } else if (on_edge || brightness_up_edge) {
                bool resume_playback = g_display_power_state.resume_playback_on_wake;
                bool keep_brightness_badge =
                    brightness_up_edge &&
                    strncmp(status_overlay_text, "BRIGHT ", 7) == 0 &&
                    (int32_t) (now_ms - (status_overlay_until + STATUS_BADGE_EXIT_ANIM_MS)) < 0;

                brightness_animation_cancel(&brightness_animation);
                display_power_on(&g_display_power_state);
                paused = !resume_playback;
                if (!paused) {
                    paused_ui_quiet_until_ms = 0;
                }
                if (resume_playback) {
                    hover_preview_needs_rebuffer = false;
                }
                if (brightness_up_edge) {
                    set_lcd_brightness(LCD_BRIGHTNESS_MAX);
                    brightness_animation_begin(
                        &brightness_animation,
                        now_ms,
                        LCD_BRIGHTNESS_MAX,
                        LCD_BRIGHTNESS_LOWEST_NORMAL,
                        false,
                        0,
                        false
                    );
                    brightness_animation_tick(
                        &brightness_animation,
                        screen,
                        now_ms,
                        status_overlay_text,
                        sizeof(status_overlay_text)
                    );
                } else if (resume_playback) {
                    snprintf(status_overlay_text, sizeof(status_overlay_text), "PLAY");
                } else {
                    snprintf(status_overlay_text, sizeof(status_overlay_text), "PAUSED");
                }
                now_ms = monotonic_clock_now_ms();
                status_overlay_show(
                    now_ms,
                    !brightness_up_edge || !keep_brightness_badge,
                    &status_overlay_started_ms,
                    &status_overlay_until
                );
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
                if (brightness_up_edge) {
                    brightness_repeat_direction = -1;
                    brightness_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
                }
                reset_playback_timeline(
                    &movie,
                    playback_rate,
                    &playback_anchor_ticks,
                    &playback_anchor_frame,
                    &next_frame_due_ticks
                );
                msleep(16);
                continue;
            }
            if (g_display_power_state.off) {
                msleep(16);
                continue;
            }
        }

        if (on_edge && !woke_from_idle_off) {
            bool was_paused = paused;

            brightness_animation_cancel(&brightness_animation);
            paused = true;
            seek_repeat_direction = 0;
            seek_repeat_next_ms = 0;
            brightness_repeat_direction = 0;
            brightness_repeat_next_ms = 0;
            reset_playback_timeline(
                &movie,
                playback_rate,
                &playback_anchor_ticks,
                &playback_anchor_frame,
                &next_frame_due_ticks
            );
            display_power_off(&g_display_power_state, was_paused);
            present_black_screen(screen);
            continue;
        }

        brightness_animation_tick(
            &brightness_animation,
            screen,
            now_ms,
            status_overlay_text,
            sizeof(status_overlay_text)
        );
        if (g_display_power_state.off) {
            msleep(16);
            continue;
        }

        if (tab_edge) {
            tab_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
        } else if (!tab_down) {
            tab_repeat_next_ms = 0;
        } else if (tab_repeat_next_ms != 0U &&
                   paused &&
                   !help_menu_open &&
                   pending_seek_ms == 0 &&
                   (int32_t) (now_ms - tab_repeat_next_ms) >= 0) {
            tab_repeat_step = true;
            tab_repeat_next_ms = now_ms + tab_hold_repeat_interval_ms;
        }

        show_ui_before_pointer_activation = show_ui;
        if (pointer.moved ||
            pointer_click ||
            (pointer.down && playback_press_target != PLAYBACK_PRESS_NONE) ||
            (enter_down && playback_press_target != PLAYBACK_PRESS_NONE)) {
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            show_ui = true;
        }
        if (!pointer_click) {
            bool allow_seek_preview = paused && show_ui && !help_menu_open;
            if (allow_seek_preview) {
                update_seek_bar_preview(&movie, &seek_preview, &pointer, allow_seek_preview, now_ms);
            } else {
                seek_preview.over_bar = false;
                seek_preview.tracking = false;
                seek_preview.last_pointer_x = -1;
                clear_seek_bar_preview_decode_job(&seek_preview);
                if (!paused) {
                    clear_seek_bar_preview(&seek_preview);
                }
            }
        } else {
            seek_preview.over_bar = false;
            if (!(show_ui && pointer.y >= SCREEN_H - UI_BAR_H && pointer.y < SCREEN_H)) {
                clear_seek_bar_preview_decode_job(&seek_preview);
            }
        }
        if (seek_left_edge) {
            seek_delta_ms = -SEEK_STEP_MS;
            seek_repeat_direction = -1;
            seek_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
        } else if (seek_right_edge) {
            seek_delta_ms = SEEK_STEP_MS;
            seek_repeat_direction = 1;
            seek_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
        } else if ((seek_repeat_direction < 0 && !seek_left_down) ||
                   (seek_repeat_direction > 0 && !seek_right_down) ||
                   (seek_left_down && seek_right_down)) {
            seek_repeat_direction = 0;
            seek_repeat_next_ms = 0;
        } else if (seek_repeat_direction != 0 &&
                   seek_repeat_next_ms != 0U &&
                   !help_menu_open &&
                   (int32_t) (now_ms - seek_repeat_next_ms) >= 0) {
            seek_delta_ms = seek_repeat_direction * SEEK_STEP_MS;
            seek_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_FALLBACK_INTERVAL_MS;
        }
        if (seek_delta_ms != 0) {
            int32_t previous_pending_seek_ms = pending_seek_ms;
            int64_t next_seek_ms = (int64_t) pending_seek_ms + seek_delta_ms;
            int64_t seek_limit_ms = (int64_t) movie_duration_ms(&movie);

            if (next_seek_ms < -seek_limit_ms) {
                next_seek_ms = -seek_limit_ms;
            }
            if (next_seek_ms > seek_limit_ms) {
                next_seek_ms = seek_limit_ms;
            }
            pending_seek_ms = (int32_t) next_seek_ms;
            pending_seek_commit_at_ms = now_ms + SEEK_STACK_DELAY_MS;
            if (pending_seek_ms != 0) {
                if (previous_pending_seek_ms == 0 ||
                    seek_badge_ms == 0 ||
                    (previous_pending_seek_ms < 0) != (pending_seek_ms < 0)) {
                    seek_badge_started_ms = now_ms ? now_ms : 1U;
                }
                seek_badge_ms = pending_seek_ms;
                seek_badge_hide_elapsed_ms = 0;
                seek_badge_last_render_ms = 0;
            } else if (seek_badge_ms != 0 && seek_badge_hide_elapsed_ms == 0) {
                seek_badge_hide_elapsed_ms = SEEK_BADGE_HIDE_PENDING;
            }
            if (paused) {
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            }
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (pending_seek_ms != 0 &&
            seek_delta_ms == 0 &&
            !seek_left_down &&
            !seek_right_down &&
            (now_ms >= pending_seek_commit_at_ms || tab_edge || pointer_click)) {
            uint32_t target_frame;
            uint32_t seek_render_now_ms;
            CommittedSeekRenderContext seek_context;

            pending_seek_consumed_click = pointer_click;
            show_ui = true;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            seek_badge_ms = pending_seek_ms;
            seek_badge_hide_elapsed_ms = 0;
            seek_render_now_ms = monotonic_clock_now_ms();
            clear_seek_bar_preview(&seek_preview);
            if (!seek_delta_target_frame(&movie, pending_seek_ms, &target_frame)) {
                report_movie_decode_failure(&movie, path, "seek");
                result = -1;
                break;
            }
            memset(&seek_context, 0, sizeof(seek_context));
            seek_context.screen = screen;
            seek_context.fonts = fonts;
            seek_context.paused = paused;
            seek_context.show_ui = true;
            seek_context.scale_mode = scale_mode;
            seek_context.scale_morph = &scale_morph;
            seek_context.video_align_x = video_align_x;
            seek_context.video_align_y = video_align_y;
            seek_context.playback_rate = playback_rate;
            seek_context.memory_overlay_mode = memory_overlay_mode;
            seek_context.subtitle_cache = &subtitle_cache;
            seek_context.subtitle_font_index = subtitle_font_index;
            seek_context.subtitle_font_overlay_visible = seek_render_now_ms <= subtitle_font_overlay_until;
            seek_context.subtitle_size = subtitle_size;
            seek_context.subtitle_placement = subtitle_placement;
            seek_context.movie_title_text = playback_title;
            seek_context.movie_detail_text = playback_detail;
            seek_context.status_overlay_text = status_overlay_text;
            seek_context.status_overlay_started_ms = status_overlay_started_ms;
            seek_context.status_overlay_until_ms = status_overlay_until;
            seek_context.screenshot_preview = &screenshot_preview;
            seek_context.seek_preview = &seek_preview;
            seek_context.pointer = &pointer;
            seek_context.pending_seek_ms = pending_seek_ms;
            seek_context.seek_badge_ms = seek_badge_ms;
            seek_context.seek_badge_started_ms = seek_badge_started_ms;
            seek_context.seek_badge_hide_elapsed_ms = seek_badge_hide_elapsed_ms;
            seek_context.ui_transitions = &ui_transitions;
            seek_context.ui_mixes = &ui_mixes;
            seek_context.playback_press_target = playback_press_target;
            seek_context.playback_press_active =
                playback_pause_key_press_active ||
                playback_pointer_press_active ||
                (playback_press_target == PLAYBACK_PRESS_PLAY && enter_down) ||
                ui_time_before(seek_render_now_ms, playback_badge_press_until_ms);
            seek_context.scale_press_active =
                divide_down ||
                (playback_press_target == PLAYBACK_PRESS_SCALE && enter_down) ||
                ui_time_before(seek_render_now_ms, scale_badge_press_until_ms);
            seek_context.speed_press_active =
                speed_key_down ||
                (playback_press_target == PLAYBACK_PRESS_SPEED && enter_down) ||
                ui_time_before(seek_render_now_ms, speed_badge_press_until_ms);
            seek_context.title_strip_active =
                !help_menu_open &&
                pointer.visible &&
                pointer.y < PLAYBACK_TITLE_TOP_EDGE_PX;
            seek_context.abort_on_input = true;
            playback_key_snapshot_init(&seek_context.abort_key_snapshot);
            seek_context.target_frame = target_frame;
            if (!decode_to_frame_with_progress(
                    &movie,
                    target_frame,
                    should_publish_committed_seek_frame,
                    render_committed_seek_frame,
                    &seek_context,
                    &seek_context.abort_requested)) {
                if (seek_context.abort_requested) {
                    uint32_t abort_now_ms = monotonic_clock_now_ms();

                    pending_seek_ms = 0;
                    pending_seek_commit_at_ms = 0;
                    seek_badge_hide_elapsed_ms = seek_badge_ms != 0 ? SEEK_BADGE_HIDE_PENDING : 0;
                    reset_playback_timeline(
                        &movie,
                        playback_rate,
                        &playback_anchor_ticks,
                        &playback_anchor_frame,
                        &next_frame_due_ticks
                    );
                    show_ui = true;
                    ui_visible_until = abort_now_ms + POINTER_UI_TIMEOUT_MS;
                    continue;
                }
                report_movie_decode_failure(&movie, path, "seek");
                result = -1;
                break;
            }
            hover_preview_needs_rebuffer = false;
            seek_badge_ms = pending_seek_ms;
            seek_badge_hide_elapsed_ms = SEEK_BADGE_HIDE_PENDING;
            pending_seek_ms = 0;
            pending_seek_commit_at_ms = 0;
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            show_ui = true;
        }
        if (pending_seek_consumed_click) {
            pointer_click = false;
        }
        if (screenshot_edge) {
            take_screenshot = true;
        }
        if (cat_edge) {
            bool was_paused = paused;

            if (help_menu_open) {
                help_menu_open = false;
                if (help_resume_playback) {
                    paused = false;
                    hover_preview_needs_rebuffer = false;
                }
                help_resume_playback = false;
            } else {
                help_menu_open = true;
                help_resume_playback = !paused;
                paused = true;
            }
            note_pause_transition(was_paused, paused, now_ms, &paused_ui_quiet_until_ms);
            if (!was_paused && paused) {
                restart_after_pause = true;
            }
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            show_ui = true;
        }
        if (esc_exit_request) {
            if (help_menu_open) {
                bool was_paused = paused;

                help_menu_open = false;
                if (help_resume_playback) {
                    paused = false;
                    hover_preview_needs_rebuffer = false;
                }
                help_resume_playback = false;
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                note_pause_transition(was_paused, paused, now_ms, &paused_ui_quiet_until_ms);
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
                show_ui = true;
                esc_exit_suppressed_until_release = true;
            } else {
                SDL_Surface *collapse_overlay = capture_screen_surface(screen);

                animate_movie_collapse_to_black(
                    screen,
                    &movie,
                    scale_mode,
                    video_align_x,
                    video_align_y,
                    &collapse_overlay
                );
                break;
            }
        }
        if (help_menu_open) {
            update_playback_ui_mixes(
                &ui_transitions,
                &ui_mixes,
                fonts,
                &movie,
                scale_mode,
                &scale_morph,
                video_align_x,
                video_align_y,
                playback_rate,
                true,
                help_menu_open,
                PLAYBACK_PRESS_NONE,
                false,
                false,
                false,
                false,
                &pointer,
                now_ms
            );
            render_movie(
                screen,
                fonts,
                &movie,
                paused,
                true,
                true,
                scale_mode,
                &scale_morph,
                video_align_x,
                video_align_y,
                playback_rate,
                memory_overlay_mode,
                &subtitle_cache,
                subtitle_font_index,
                false,
                subtitle_size,
                subtitle_placement,
                playback_title,
                playback_detail,
                status_overlay_text,
                status_overlay_started_ms,
                status_overlay_until,
                &screenshot_preview,
                &seek_preview,
                now_ms,
                &pointer,
                0,
                0,
                0,
                0,
                &ui_mixes
            );
            if (take_screenshot) {
                char saved_path[MAX_PATH_LEN];
                if (save_screenshot_bitmap(screen, path, saved_path, sizeof(saved_path))) {
                    prepare_screenshot_preview(&screenshot_preview, screen, saved_path);
                }
            }
            if (display_power_tick_idle(&g_display_power_state, screen, monotonic_clock_now_ms(), true, true)) {
                msleep(16);
                continue;
            }
            if (!playback_ui_mixes_animating(&ui_mixes) &&
                !ui_time_before(monotonic_clock_now_ms(), paused_ui_quiet_until_ms) &&
                !ui_time_before(monotonic_clock_now_ms(), playback_input_prefetch_quiet_until_ms)) {
                prefetch_tick(&movie, true, 1000, &pointer);
            }
            msleep(16);
            continue;
        }
        if (previous_video_edge || next_video_edge) {
            bool found_adjacent = previous_video_edge
                ? find_previous_movie_path(path, next_path, next_path_size)
                : find_next_movie_path(path, next_path, next_path_size);

            if (found_adjacent) {
                result = PLAY_MOVIE_RESULT_SWITCH_MOVIE;
                break;
            }
            snprintf(status_overlay_text, sizeof(status_overlay_text), "%s OF LIST", previous_video_edge ? "START" : "END");
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (playback_mode_edge) {
            playback_mode = (PlaybackMode) ((playback_mode + 1) % PLAYBACK_MODE_COUNT);
            snprintf(status_overlay_text, sizeof(status_overlay_text), "MODE %s", playback_mode_text(playback_mode));
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (frame_skip_mode_edge) {
            realtime_frame_skip = !realtime_frame_skip;
            snprintf(
                status_overlay_text,
                sizeof(status_overlay_text),
                "SYNC %s",
                realtime_frame_skip ? "REALTIME" : "SMOOTH"
            );
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            if (!paused) {
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            }
        }
        if (video_up_left_edge) {
            apply_video_align_preset(&video_align_x, &video_align_y, VIDEO_ALIGN_NEGATIVE, VIDEO_ALIGN_NEGATIVE);
            format_video_align_status(video_align_x, video_align_y, status_overlay_text, sizeof(status_overlay_text));
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (video_up_edge) {
            apply_video_align_preset(&video_align_x, &video_align_y, VIDEO_ALIGN_CENTER, VIDEO_ALIGN_NEGATIVE);
            format_video_align_status(video_align_x, video_align_y, status_overlay_text, sizeof(status_overlay_text));
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (video_up_right_edge) {
            apply_video_align_preset(&video_align_x, &video_align_y, VIDEO_ALIGN_POSITIVE, VIDEO_ALIGN_NEGATIVE);
            format_video_align_status(video_align_x, video_align_y, status_overlay_text, sizeof(status_overlay_text));
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (video_left_edge) {
            apply_video_align_preset(&video_align_x, &video_align_y, VIDEO_ALIGN_NEGATIVE, VIDEO_ALIGN_CENTER);
            format_video_align_status(video_align_x, video_align_y, status_overlay_text, sizeof(status_overlay_text));
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (video_center_edge) {
            video_align_x = VIDEO_ALIGN_CENTER;
            video_align_y = VIDEO_ALIGN_CENTER;
            format_video_align_status(video_align_x, video_align_y, status_overlay_text, sizeof(status_overlay_text));
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (video_right_edge) {
            apply_video_align_preset(&video_align_x, &video_align_y, VIDEO_ALIGN_POSITIVE, VIDEO_ALIGN_CENTER);
            format_video_align_status(video_align_x, video_align_y, status_overlay_text, sizeof(status_overlay_text));
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (video_down_left_edge) {
            apply_video_align_preset(&video_align_x, &video_align_y, VIDEO_ALIGN_NEGATIVE, VIDEO_ALIGN_POSITIVE);
            format_video_align_status(video_align_x, video_align_y, status_overlay_text, sizeof(status_overlay_text));
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (video_down_edge) {
            apply_video_align_preset(&video_align_x, &video_align_y, VIDEO_ALIGN_CENTER, VIDEO_ALIGN_POSITIVE);
            format_video_align_status(video_align_x, video_align_y, status_overlay_text, sizeof(status_overlay_text));
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (video_down_right_edge) {
            apply_video_align_preset(&video_align_x, &video_align_y, VIDEO_ALIGN_POSITIVE, VIDEO_ALIGN_POSITIVE);
            format_video_align_status(video_align_x, video_align_y, status_overlay_text, sizeof(status_overlay_text));
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (brightness_up_edge) {
            brightness_delta = -LCD_BRIGHTNESS_STEP;
            brightness_repeat_direction = -1;
            brightness_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
        } else if (brightness_down_edge) {
            brightness_delta = LCD_BRIGHTNESS_STEP;
            brightness_repeat_direction = 1;
            brightness_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
        } else if ((brightness_repeat_direction < 0 && !brightness_up_down) ||
                   (brightness_repeat_direction > 0 && !brightness_down_down) ||
                   (brightness_up_down && brightness_down_down)) {
            brightness_repeat_direction = 0;
            brightness_repeat_next_ms = 0;
        } else if (brightness_repeat_direction != 0 &&
                   brightness_repeat_next_ms != 0U &&
                   (int32_t) (now_ms - brightness_repeat_next_ms) >= 0) {
            brightness_delta = brightness_repeat_direction * LCD_BRIGHTNESS_STEP;
            brightness_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_FALLBACK_INTERVAL_MS;
        }
        if (brightness_delta != 0) {
            uint32_t brightness_from_raw = current_lcd_brightness();
            uint32_t brightness_logical_raw = brightness_animation_logical_raw(&brightness_animation);
            uint32_t brightness_target_raw;
            bool keep_brightness_badge =
                strncmp(status_overlay_text, "BRIGHT ", 7) == 0 &&
                (int32_t) (now_ms - (status_overlay_until + STATUS_BADGE_EXIT_ANIM_MS)) < 0;

            if (brightness_delta > 0 && brightness_logical_raw >= LCD_BRIGHTNESS_LOWEST_NORMAL) {
                bool was_paused = paused;

                paused = true;
                brightness_repeat_direction = 0;
                brightness_repeat_next_ms = 0;
                seek_repeat_direction = 0;
                seek_repeat_next_ms = 0;
                speed_repeat_direction = 0;
                speed_repeat_next_ms = 0;
                brightness_animation_begin(
                    &brightness_animation,
                    now_ms,
                    brightness_from_raw,
                    LCD_BRIGHTNESS_MAX,
                    true,
                    brightness_logical_raw,
                    was_paused
                );
                brightness_animation_tick(
                    &brightness_animation,
                    screen,
                    now_ms,
                    status_overlay_text,
                    sizeof(status_overlay_text)
                );
                status_overlay_show(now_ms, !keep_brightness_badge, &status_overlay_started_ms, &status_overlay_until);
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
                reset_playback_timeline(
                    &movie,
                    playback_rate,
                    &playback_anchor_ticks,
                    &playback_anchor_frame,
                    &next_frame_due_ticks
                );
                continue;
            }

            brightness_target_raw = lcd_brightness_step_target_from(brightness_logical_raw, brightness_delta);
            if (brightness_target_raw != brightness_from_raw ||
                lcd_brightness_percent(brightness_target_raw) != lcd_brightness_percent(brightness_from_raw)) {
                brightness_animation_begin(
                    &brightness_animation,
                    now_ms,
                    brightness_from_raw,
                    brightness_target_raw,
                    false,
                    0,
                    false
                );
                brightness_animation_tick(
                    &brightness_animation,
                    screen,
                    now_ms,
                    status_overlay_text,
                    sizeof(status_overlay_text)
                );
            } else {
                brightness_animation_cancel(&brightness_animation);
                set_lcd_brightness((int) brightness_target_raw);
                format_brightness_status(
                    status_overlay_text,
                    sizeof(status_overlay_text),
                    lcd_brightness_percent(brightness_target_raw)
                );
            }

            status_overlay_show(now_ms, !keep_brightness_badge, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (space_edge) {
            bool was_paused = paused;

            playback_pause_key_press_active = true;
            ui_transition_prime_press(&ui_transitions.playback_press, now_ms);
            playback_badge_press_triggered = true;
            if (movie.current_frame + 1 >= movie.header.frame_count) {
                if (!decode_to_frame(&movie, 0)) {
                    report_movie_decode_failure(&movie, path, "restart");
                    result = -1;
                    break;
                }
                hover_preview_needs_rebuffer = false;
                paused = false;
            } else {
                paused = !paused;
            }
            if (!paused) {
                hover_preview_needs_rebuffer = false;
            }
            note_pause_transition(was_paused, paused, now_ms, &paused_ui_quiet_until_ms);
            if (!was_paused && paused) {
                restart_after_pause = true;
            }
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (divide_edge) {
            cycle_scale_mode_with_morph(&movie, &scale_morph, &scale_mode, video_align_x, video_align_y, now_ms);
            trigger_badge_press(&ui_transitions.scale_press, &scale_badge_press_until_ms, now_ms);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (speed_down_edge) {
            speed_delta = -1;
            speed_repeat_direction = -1;
            speed_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
            trigger_badge_press(&ui_transitions.speed_press, &speed_badge_press_until_ms, now_ms);
        } else if (speed_up_edge) {
            speed_delta = 1;
            speed_repeat_direction = 1;
            speed_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
            trigger_badge_press(&ui_transitions.speed_press, &speed_badge_press_until_ms, now_ms);
        } else if ((speed_repeat_direction < 0 && !speed_down_down) ||
                   (speed_repeat_direction > 0 && !speed_up_down) ||
                   (speed_down_down && speed_up_down)) {
            speed_repeat_direction = 0;
            speed_repeat_next_ms = 0;
        } else if (speed_repeat_direction != 0 &&
                   speed_repeat_next_ms != 0U &&
                   (int32_t) (now_ms - speed_repeat_next_ms) >= 0) {
            speed_delta = speed_repeat_direction;
            speed_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_FALLBACK_INTERVAL_MS;
        }
        if (speed_delta != 0) {
            bool speed_changed = false;

            if (speed_delta < 0 && playback_rate_index > 0) {
                playback_rate_index--;
                speed_changed = true;
            } else if (speed_delta > 0 && playback_rate_index + 1 < PLAYBACK_RATE_COUNT) {
                playback_rate_index++;
                speed_changed = true;
            } else {
                speed_repeat_direction = 0;
                speed_repeat_next_ms = 0;
            }
            if (speed_changed) {
                playback_rate = playback_rate_for_index(playback_rate_index);
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            }
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (exp_edge || tenx_edge) {
            subtitle_placement = subtitle_cycle_placement(
                subtitle_placement,
                selected_subtitle_track_supports_auto_positioning(&movie)
            );
            snprintf(
                status_overlay_text,
                sizeof(status_overlay_text),
                "SUB POS %s",
                subtitle_placement_label(subtitle_placement)
            );
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (subtitle_size_up_edge && subtitle_size < 3) {
            subtitle_size++;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (subtitle_size_down_edge && subtitle_size > -1) {
            subtitle_size--;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (subtitle_font_edge) {
            subtitle_font_index = (subtitle_font_index + 1) % SUBTITLE_FONT_CHOICE_COUNT;
            subtitle_font_overlay_until = now_ms + SUBTITLE_FONT_OVERLAY_MS;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (subtitle_track_edge) {
            if (movie.subtitle_track_count > 1) {
                movie.selected_subtitle_track = (uint16_t) ((movie.selected_subtitle_track + 1) % movie.subtitle_track_count);
            } else {
                movie.selected_subtitle_track = 0;
            }
            subtitle_placement = subtitle_normalize_placement(
                subtitle_placement,
                selected_subtitle_track_supports_auto_positioning(&movie)
            );
            snprintf(
                status_overlay_text,
                sizeof(status_overlay_text),
                "SUB %.22s",
                active_subtitle_track_name(&movie)
            );
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (memory_overlay_edge) {
            memory_overlay_mode = (memory_overlay_mode == MEMORY_OVERLAY_OFF)
                ? MEMORY_OVERLAY_ALWAYS
                : MEMORY_OVERLAY_OFF;
            debug_set_metrics_collection(
                memory_overlay_mode != MEMORY_OVERLAY_OFF || debug_is_runtime_logging_enabled()
            );
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (debug_logging_edge) {
            if (debug_is_runtime_logging_enabled()) {
                debug_set_runtime_logging(false);
                snprintf(status_overlay_text, sizeof(status_overlay_text), "DEBUG LOG OFF");
            } else {
                debug_set_runtime_logging(true);
                snprintf(
                    status_overlay_text,
                    sizeof(status_overlay_text),
                    "%s",
                    debug_is_runtime_logging_enabled() ? "DEBUG LOG ON" : "DEBUG LOG NO RAM"
                );
                if (debug_is_runtime_logging_enabled()) {
                    debug_tracef(
                        "debug logging enabled frame=%lu chunk=%d",
                        (unsigned long) movie.current_frame,
                        movie.loaded_chunk
                    );
                    debug_log_sram_status();
                }
            }
            debug_set_metrics_collection(
                memory_overlay_mode != MEMORY_OVERLAY_OFF || debug_is_runtime_logging_enabled()
            );
            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (tab_edge) {
            bool was_paused = paused;

            if (!paused) {
                paused = true;
            } else if (!step_movie_forward_one_frame(&movie, &hover_preview_needs_rebuffer)) {
                report_movie_decode_failure(&movie, path, "tab step");
                result = -1;
                break;
            }
            note_pause_transition(was_paused, paused, now_ms, &paused_ui_quiet_until_ms);
            if (!was_paused && paused) {
                restart_after_pause = true;
            }
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (tab_repeat_step && paused) {
            if (!step_movie_forward_one_frame(&movie, &hover_preview_needs_rebuffer)) {
                report_movie_decode_failure(&movie, path, "tab hold step");
                result = -1;
                break;
            }
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (pointer_click || pointer_release_edge) {
            SDL_Rect bar = progress_bar_rect();
            SDL_Rect click_src;
            SDL_Rect click_dst;
            SDL_Rect playback_badge;
            SDL_Rect scale_badge;
            SDL_Rect speed_badge;
            bool controls_live;

            scale_morph_current_rects(&movie, &scale_morph, scale_mode, video_align_x, video_align_y, now_ms, &click_src, &click_dst);
            playback_badge = playback_badge_rect(&click_dst);
            status_badge_rects(fonts, &click_dst, scale_mode, playback_rate, &scale_badge, &speed_badge);
            controls_live = (show_ui_before_pointer_activation || playback_press_target != PLAYBACK_PRESS_NONE) && !help_menu_open;
            if (pointer_click) {
                playback_press_target = PLAYBACK_PRESS_NONE;
                if (controls_live && pointer_over_rect(&pointer, &playback_badge)) {
                    playback_press_target = PLAYBACK_PRESS_PLAY;
                    pointer_click = false;
                } else if (controls_live && pointer_over_rect(&pointer, &scale_badge)) {
                    playback_press_target = PLAYBACK_PRESS_SCALE;
                    pointer_click = false;
                } else if (controls_live && pointer_over_rect(&pointer, &speed_badge)) {
                    playback_press_target = PLAYBACK_PRESS_SPEED;
                    pointer_click = false;
                }
            }
            if (pointer_release_edge && playback_press_target != PLAYBACK_PRESS_NONE) {
                PlaybackPressTarget released_target = playback_press_target;
                bool released_on_target = false;

                if (controls_live && released_target == PLAYBACK_PRESS_PLAY && pointer_over_rect(&pointer, &playback_badge)) {
                    released_on_target = true;
                } else if (controls_live && released_target == PLAYBACK_PRESS_SCALE && pointer_over_rect(&pointer, &scale_badge)) {
                    released_on_target = true;
                } else if (controls_live && released_target == PLAYBACK_PRESS_SPEED && pointer_over_rect(&pointer, &speed_badge)) {
                    released_on_target = true;
                }
                playback_press_target = PLAYBACK_PRESS_NONE;
                if (released_on_target) {
                    if (released_target == PLAYBACK_PRESS_PLAY) {
                        bool was_paused = paused;

                        if (movie.current_frame + 1 >= movie.header.frame_count) {
                            if (!decode_to_frame(&movie, 0)) {
                                report_movie_decode_failure(&movie, path, "pointer restart");
                                result = -1;
                                break;
                            }
                            hover_preview_needs_rebuffer = false;
                            paused = false;
                        } else {
                            paused = !paused;
                        }
                        if (!paused) {
                            hover_preview_needs_rebuffer = false;
                        }
                        note_pause_transition(was_paused, paused, now_ms, &paused_ui_quiet_until_ms);
                        if (!was_paused && paused) {
                            restart_after_pause = true;
                        }
                        reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                    } else if (released_target == PLAYBACK_PRESS_SCALE) {
                        cycle_scale_mode_with_morph(&movie, &scale_morph, &scale_mode, video_align_x, video_align_y, now_ms);
                    } else if (released_target == PLAYBACK_PRESS_SPEED) {
                        playback_rate_index = (playback_rate_index + 1) % PLAYBACK_RATE_COUNT;
                        playback_rate = playback_rate_for_index(playback_rate_index);
                        reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                    }
                    ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
                }
            }

            if (pointer_click && show_ui_before_pointer_activation && pointer.y >= SCREEN_H - UI_BAR_H && pointer.y < SCREEN_H) {
                int seek_marker_x = progress_bar_marker_x_from_pointer(&bar, pointer.x);
                uint32_t target_frame;
                uint32_t seek_render_now_ms;
                CommittedSeekRenderContext seek_context;
                bool used_preview_frame = false;

                seek_render_now_ms = monotonic_clock_now_ms();
                if (!progress_bar_target_frame_for_marker(&movie, &bar, seek_marker_x, &target_frame, NULL)) {
                    report_movie_decode_failure(&movie, path, "pointer seek");
                    result = -1;
                    break;
                }
                if (target_frame == movie.current_frame) {
                    clear_seek_bar_preview(&seek_preview);
                    suppress_seek_bar_preview_rebuild(&seek_preview, seek_marker_x, target_frame);
                } else {
                    memset(&seek_context, 0, sizeof(seek_context));
                    seek_context.screen = screen;
                    seek_context.fonts = fonts;
                    seek_context.paused = paused;
                    seek_context.show_ui = true;
                    seek_context.scale_mode = scale_mode;
                    seek_context.scale_morph = &scale_morph;
                    seek_context.video_align_x = video_align_x;
                    seek_context.video_align_y = video_align_y;
                    seek_context.playback_rate = playback_rate;
                    seek_context.memory_overlay_mode = memory_overlay_mode;
                    seek_context.subtitle_cache = &subtitle_cache;
                    seek_context.subtitle_font_index = subtitle_font_index;
                    seek_context.subtitle_font_overlay_visible = seek_render_now_ms <= subtitle_font_overlay_until;
                    seek_context.subtitle_size = subtitle_size;
                    seek_context.subtitle_placement = subtitle_placement;
                    seek_context.movie_title_text = playback_title;
                    seek_context.movie_detail_text = playback_detail;
                    seek_context.status_overlay_text = status_overlay_text;
                    seek_context.status_overlay_started_ms = status_overlay_started_ms;
                    seek_context.status_overlay_until_ms = status_overlay_until;
                    seek_context.screenshot_preview = &screenshot_preview;
                    seek_context.seek_preview = &seek_preview;
                    seek_context.pointer = &pointer;
                    seek_context.pending_seek_ms = 0;
                    seek_context.seek_badge_ms = seek_badge_ms;
                    seek_context.seek_badge_started_ms = seek_badge_started_ms;
                    seek_context.seek_badge_hide_elapsed_ms = seek_badge_hide_elapsed_ms;
                    seek_context.ui_transitions = &ui_transitions;
                    seek_context.ui_mixes = &ui_mixes;
                    seek_context.playback_press_target = playback_press_target;
                    seek_context.playback_press_active =
                        playback_pause_key_press_active ||
                        playback_pointer_press_active ||
                        (playback_press_target == PLAYBACK_PRESS_PLAY && enter_down) ||
                        ui_time_before(seek_render_now_ms, playback_badge_press_until_ms);
                    seek_context.scale_press_active =
                        divide_down ||
                        (playback_press_target == PLAYBACK_PRESS_SCALE && enter_down) ||
                        ui_time_before(seek_render_now_ms, scale_badge_press_until_ms);
                    seek_context.speed_press_active =
                        speed_key_down ||
                        (playback_press_target == PLAYBACK_PRESS_SPEED && enter_down) ||
                        ui_time_before(seek_render_now_ms, speed_badge_press_until_ms);
                    seek_context.title_strip_active =
                        !help_menu_open &&
                        pointer.visible &&
                        pointer.y < PLAYBACK_TITLE_TOP_EDGE_PX;
                    seek_context.abort_on_input = true;
                    playback_key_snapshot_init(&seek_context.abort_key_snapshot);
                    seek_context.target_frame = target_frame;
                    used_preview_frame = commit_seek_bar_preview_to_movie(&movie, &seek_preview, target_frame);
                    clear_seek_bar_preview(&seek_preview);
                    suppress_seek_bar_preview_rebuild(&seek_preview, seek_marker_x, target_frame);
                    if (used_preview_frame) {
                        render_committed_seek_frame(&movie, movie.current_frame, &seek_context);
                    }
                    if ((!used_preview_frame || movie.current_frame != target_frame) &&
                        !decode_to_frame_with_progress(
                            &movie,
                            target_frame,
                            should_publish_committed_seek_frame,
                            render_committed_seek_frame,
                            &seek_context,
                            &seek_context.abort_requested)) {
                        if (seek_context.abort_requested) {
                            uint32_t abort_now_ms = monotonic_clock_now_ms();

                            hover_preview_needs_rebuffer = false;
                            reset_playback_timeline(
                                &movie,
                                playback_rate,
                                &playback_anchor_ticks,
                                &playback_anchor_frame,
                                &next_frame_due_ticks
                            );
                            show_ui = true;
                            ui_visible_until = abort_now_ms + POINTER_UI_TIMEOUT_MS;
                            continue;
                        }
                        report_movie_decode_failure(&movie, path, "pointer seek");
                        result = -1;
                        break;
                    }
                    hover_preview_needs_rebuffer = false;
                    reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                }
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            } else if (pointer_click) {
                bool was_paused = paused;

                playback_pointer_press_active = true;
                trigger_playback_badge_press(&ui_transitions, &playback_badge_press_until_ms, now_ms);
                playback_badge_press_triggered = true;
                if (movie.current_frame + 1 >= movie.header.frame_count) {
                    if (!decode_to_frame(&movie, 0)) {
                        report_movie_decode_failure(&movie, path, "pointer restart");
                        result = -1;
                        break;
                    }
                    hover_preview_needs_rebuffer = false;
                    paused = false;
                } else {
                    paused = !paused;
                }
                if (!paused) {
                    hover_preview_needs_rebuffer = false;
                }
                note_pause_transition(was_paused, paused, now_ms, &paused_ui_quiet_until_ms);
                if (!was_paused && paused) {
                    restart_after_pause = true;
                }
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            }
        }
        if (restart_after_pause) {
            if (!playback_badge_press_triggered) {
                trigger_playback_badge_press(&ui_transitions, &playback_badge_press_until_ms, now_ms);
            }
        }
        if (!paused && frame_interval_ticks > 0) {
            if (now_ticks >= next_frame_due_ticks) {
                uint64_t elapsed_ticks = now_ticks - playback_anchor_ticks;
                uint32_t frames_to_advance = movie_frames_from_scaled_ticks(&movie, elapsed_ticks, playback_rate);
                uint32_t target_frame = playback_anchor_frame + frames_to_advance;
                bool lagged = false;
                uint32_t lag_frames = 0;
                uint32_t late_ms = 0;

                if (target_frame > movie.current_frame + 1U) {
                    lag_frames = target_frame - (movie.current_frame + 1U);
                    if (!realtime_frame_skip) {
                        target_frame = movie.current_frame + 1U;
                    }
                    lagged = true;
                }
                if (lagged) {
                    late_ms = monotonic_clock_ticks_to_ms(now_ticks - next_frame_due_ticks);
                    movie.diag_lag_event_count++;
                    movie.diag_lag_frame_total += lag_frames;
                    if (lag_frames > movie.diag_max_lag_frames) {
                        movie.diag_max_lag_frames = lag_frames;
                    }
                    if (late_ms > movie.diag_max_late_ms) {
                        movie.diag_max_late_ms = late_ms;
                    }
                    debug_tracef(
                        "lag frame=%lu target=%lu late_ms=%lu lag_frames=%lu spare_prev=%lu",
                        (unsigned long) movie.current_frame,
                        (unsigned long) target_frame,
                        (unsigned long) late_ms,
                        (unsigned long) lag_frames,
                        (unsigned long) movie.diag_last_spare_ms
                    );
                }

                if (target_frame >= movie.header.frame_count) {
                    if (playback_mode == PLAYBACK_MODE_REPEAT) {
                        if (!decode_to_frame(&movie, 0)) {
                            report_movie_decode_failure(&movie, path, "auto replay");
                            result = PLAY_MOVIE_RESULT_ERROR;
                            break;
                        }
                        hover_preview_needs_rebuffer = false;
                        reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                        ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
                    } else if (playback_mode == PLAYBACK_MODE_AUTO_NEXT &&
                               find_next_movie_path(path, next_path, next_path_size)) {
                        result = PLAY_MOVIE_RESULT_AUTO_NEXT;
                        break;
                    } else {
                        if (movie.current_frame + 1 < movie.header.frame_count) {
                            if (!decode_to_frame(&movie, movie.header.frame_count - 1)) {
                                report_movie_decode_failure(&movie, path, "final frame");
                                result = PLAY_MOVIE_RESULT_ERROR;
                                break;
                            }
                        }
                        {
                            bool was_paused = paused;

                            paused = true;
                            note_pause_transition(was_paused, paused, now_ms, &paused_ui_quiet_until_ms);
                        }
                        if (playback_mode == PLAYBACK_MODE_AUTO_NEXT) {
                            snprintf(status_overlay_text, sizeof(status_overlay_text), "END OF LIST");
                            status_overlay_show(now_ms, true, &status_overlay_started_ms, &status_overlay_until);
                        }
                        ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
                    }
                } else {
                    if (target_frame > movie.current_frame) {
                        uint32_t decode_start_ms = monotonic_clock_now_ms();
                        uint32_t decode_end_ms;
                        uint32_t decode_elapsed_ms;

                        if (!decode_to_frame(&movie, target_frame)) {
                            report_movie_decode_failure(&movie, path, "playback advance");
                            result = -1;
                            break;
                        }
                        decode_end_ms = monotonic_clock_now_ms();
                        decode_elapsed_ms = decode_end_ms - decode_start_ms;
                        record_debug_displayed_frame(&movie, decode_end_ms);
                        if (debug_should_collect_metrics()) {
                            movie.diag_foreground_decode_count++;
                        }
                        record_h264_foreground_decode_time(&movie, decode_elapsed_ms);
                        if (debug_is_runtime_logging_enabled() &&
                            (decode_elapsed_ms >= DEBUG_TRACE_FOREGROUND_MS || lagged)) {
                            debug_tracef(
                                "fg frame=%lu ms=%lu lagged=%u chunk=%d direct=%lu",
                                (unsigned long) target_frame,
                                (unsigned long) decode_elapsed_ms,
                                lagged ? 1U : 0U,
                                movie.loaded_chunk,
                                (unsigned long) movie.diag_foreground_direct_decode_count
                            );
                        }
                    }
                    if (lagged && !realtime_frame_skip) {
                        reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                    } else {
                        next_frame_due_ticks = playback_anchor_ticks + movie_frame_time_scaled_ticks(&movie, (target_frame - playback_anchor_frame) + 1, playback_rate);
                    }
                }
            }
        }
        {
            uint32_t render_now_ms = monotonic_clock_now_ms();
            bool subtitle_font_overlay_visible = render_now_ms <= subtitle_font_overlay_until;

            if (seek_badge_hide_elapsed_ms == SEEK_BADGE_HIDE_PENDING) {
                seek_badge_hide_elapsed_ms = 1U;
                seek_badge_last_render_ms = render_now_ms;
            } else if (seek_badge_hide_elapsed_ms != 0U) {
                uint32_t seek_badge_frame_ms = seek_badge_last_render_ms == 0U
                    ? 16U
                    : render_now_ms - seek_badge_last_render_ms;
                if (seek_badge_frame_ms == 0U) {
                    seek_badge_frame_ms = 1U;
                } else if (seek_badge_frame_ms > 18U) {
                    seek_badge_frame_ms = 18U;
                }
                seek_badge_last_render_ms = render_now_ms;
                if (seek_badge_hide_elapsed_ms + seek_badge_frame_ms >= SEEK_BADGE_EXIT_ANIM_MS) {
                    seek_badge_hide_elapsed_ms = SEEK_BADGE_EXIT_ANIM_MS;
                } else {
                    seek_badge_hide_elapsed_ms += seek_badge_frame_ms;
                }
            }
            show_ui = paused || (render_now_ms <= ui_visible_until);
            update_playback_ui_mixes(
                &ui_transitions,
                &ui_mixes,
                fonts,
                &movie,
                scale_mode,
                &scale_morph,
                video_align_x,
                video_align_y,
                playback_rate,
                show_ui || ui_transitions.help_menu.current_mix > 0,
                help_menu_open,
                playback_press_target,
                playback_pause_key_press_active ||
                    playback_pointer_press_active ||
                    (playback_press_target == PLAYBACK_PRESS_PLAY && enter_down) ||
                    ui_time_before(render_now_ms, playback_badge_press_until_ms),
                divide_down ||
                    (playback_press_target == PLAYBACK_PRESS_SCALE && enter_down) ||
                    ui_time_before(render_now_ms, scale_badge_press_until_ms),
                speed_key_down ||
                    (playback_press_target == PLAYBACK_PRESS_SPEED && enter_down) ||
                    ui_time_before(render_now_ms, speed_badge_press_until_ms),
                show_ui && !help_menu_open && pointer.visible && pointer.y < PLAYBACK_TITLE_TOP_EDGE_PX,
                &pointer,
                render_now_ms
            );
            render_movie(
                screen,
                fonts,
                &movie,
                paused,
                show_ui,
                false,
                scale_mode,
                &scale_morph,
                video_align_x,
                video_align_y,
                playback_rate,
                memory_overlay_mode,
                &subtitle_cache,
                subtitle_font_index,
                subtitle_font_overlay_visible,
                subtitle_size,
                subtitle_placement,
                playback_title,
                playback_detail,
                status_overlay_text,
                status_overlay_started_ms,
                status_overlay_until,
                &screenshot_preview,
                &seek_preview,
                render_now_ms,
                &pointer,
                pending_seek_ms,
                seek_badge_ms,
                seek_badge_started_ms,
                seek_badge_hide_elapsed_ms,
                &ui_mixes
            );
            if (take_screenshot) {
                char saved_path[MAX_PATH_LEN];
                if (save_screenshot_bitmap(screen, path, saved_path, sizeof(saved_path))) {
                    prepare_screenshot_preview(&screenshot_preview, screen, saved_path);
                }
            }
            if (display_power_tick_idle(
                    &g_display_power_state,
                    screen,
                    render_now_ms,
                    paused || help_menu_open || frame_interval_ticks == 0,
                    true)) {
                paused = true;
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                msleep(16);
                continue;
            }
            if (seek_badge_hide_elapsed_ms >= SEEK_BADGE_EXIT_ANIM_MS) {
                seek_badge_ms = 0;
                seek_badge_started_ms = 0;
                seek_badge_hide_elapsed_ms = 0;
                seek_badge_last_render_ms = 0;
            }
        }
        if (paused || frame_interval_ticks == 0) {
            uint32_t idle_now_ms = monotonic_clock_now_ms();
            bool paused_input_grace = paused && ui_time_before(idle_now_ms, paused_ui_quiet_until_ms);
            bool playback_input_grace = ui_time_before(idle_now_ms, playback_input_prefetch_quiet_until_ms);

            if (paused && !paused_input_grace && !playback_input_grace && seek_bar_preview_decode_active(&seek_preview)) {
                step_seek_bar_preview_decode(
                    &movie,
                    &seek_preview,
                    idle_now_ms + SEEK_BAR_PREVIEW_SLICE_MS
                );
                idle_now_ms = monotonic_clock_now_ms();
            }
            bool paused_ui_busy = paused && (
                playback_ui_mixes_animating(&ui_mixes) ||
                scale_morph_animating(&scale_morph, idle_now_ms) ||
                seek_bar_preview_decode_active(&seek_preview) ||
                seek_preview_surface_animating(&seek_preview, idle_now_ms) ||
                paused_input_grace ||
                playback_input_grace
            );

            if (!paused_ui_busy) {
                prefetch_tick(&movie, true, 1000, &pointer);
            }
            if (!paused_ui_busy &&
                debug_is_runtime_logging_enabled() &&
                now_ms - movie.diag_last_snapshot_ms >= DEBUG_SNAPSHOT_INTERVAL_MS) {
                movie.diag_last_snapshot_ms = now_ms;
                debug_trace_runtime_snapshot(&movie, true, 1000U, playback_rate, "paused");
            }
            msleep((paused_input_grace || playback_input_grace) ? 2 : 16);
        } else {
            uint64_t after_render_ticks = monotonic_clock_now_ticks();
            uint64_t spare_ticks = next_frame_due_ticks > after_render_ticks ? (next_frame_due_ticks - after_render_ticks) : 0;
            uint32_t spare_ms = monotonic_clock_ticks_to_ms(spare_ticks);
            uint64_t wait_target_ticks = next_frame_due_ticks;
            bool playback_input_grace = ui_time_before(monotonic_clock_ticks_to_ms(after_render_ticks), playback_input_prefetch_quiet_until_ms);
            if (debug_should_collect_metrics()) {
                if (spare_ms > movie.diag_max_spare_ms) {
                    movie.diag_max_spare_ms = spare_ms;
                }
                movie.diag_last_spare_ms = spare_ms;
            }
            if (!playback_input_grace && !playback_wait_input_pending(&pointer)) {
                prefetch_tick(&movie, false, spare_ms, &pointer);
            }
            if (debug_is_runtime_logging_enabled() &&
                now_ms - movie.diag_last_snapshot_ms >= DEBUG_SNAPSHOT_INTERVAL_MS) {
                movie.diag_last_snapshot_ms = now_ms;
                debug_trace_runtime_snapshot(&movie, false, spare_ms, playback_rate, "play");
            }
            after_render_ticks = monotonic_clock_now_ticks();
            spare_ticks = next_frame_due_ticks > after_render_ticks ? (next_frame_due_ticks - after_render_ticks) : 0;
            if (spare_ticks > 0) {
                uint64_t max_wait_ticks = (((uint64_t) monotonic_clock_ticks_per_second()) * 8U) / 1000U;
                if (spare_ticks > max_wait_ticks) {
                    wait_target_ticks = after_render_ticks + max_wait_ticks;
                }
                wait_until_ticks_playback(wait_target_ticks, &pointer);
            }
        }
    }

    if (result != PLAY_MOVIE_RESULT_HOME_EXIT &&
        result != PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT) {
        display_power_restore(&g_display_power_state, monotonic_clock_now_ms());
    }
    if (result == PLAY_MOVIE_RESULT_EXIT ||
        result == PLAY_MOVIE_RESULT_AUTO_NEXT ||
        result == PLAY_MOVIE_RESULT_SWITCH_MOVIE ||
        result == PLAY_MOVIE_RESULT_APP_EXIT ||
        result == PLAY_MOVIE_RESULT_HOME_EXIT ||
        result == PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT) {
        queue_history_save_from_movie(
            &g_pending_history_save,
            &g_picker_cache,
            path,
            &movie,
            scale_mode,
            playback_rate_index,
            playback_mode,
            realtime_frame_skip,
            subtitle_font_index,
            subtitle_size,
            subtitle_placement,
            video_align_x,
            video_align_y
        );
    }
    if (debug_is_runtime_logging_enabled() ||
        (result != PLAY_MOVIE_RESULT_EXIT &&
            result != PLAY_MOVIE_RESULT_AUTO_NEXT &&
            result != PLAY_MOVIE_RESULT_SWITCH_MOVIE &&
            result != PLAY_MOVIE_RESULT_APP_EXIT &&
            result != PLAY_MOVIE_RESULT_HOME_EXIT &&
            result != PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT)) {
        char log_path[MAX_PATH_LEN];
        const char *exit_reason = "normal-exit";

        if (result == PLAY_MOVIE_RESULT_AUTO_NEXT) {
            exit_reason = "auto-next";
        } else if (result == PLAY_MOVIE_RESULT_SWITCH_MOVIE) {
            exit_reason = "switch-movie";
        } else if (result == PLAY_MOVIE_RESULT_APP_EXIT) {
            exit_reason = "app-exit";
        } else if (result == PLAY_MOVIE_RESULT_HOME_EXIT) {
            exit_reason = "home-exit";
        } else if (result == PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT) {
            exit_reason = "scratchpad-exit";
        } else if (result != PLAY_MOVIE_RESULT_EXIT) {
            exit_reason = "aborted";
        }
        debug_log_path_for_movie(path, log_path, sizeof(log_path));
        debug_dump_session(log_path, &movie, exit_reason);
    }
    free_subtitle_surface_cache(&subtitle_cache);
    clear_screenshot_preview(&screenshot_preview);
    clear_seek_bar_preview(&seek_preview);
    if (result == PLAY_MOVIE_RESULT_EXIT) {
        defer_playback_movie_cleanup(&movie);
    } else {
        destroy_movie(&movie);
    }
    return result;
}

