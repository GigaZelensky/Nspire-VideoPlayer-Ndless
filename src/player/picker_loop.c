#include "player_internal.h"

int pick_movie(
    SDL_Surface *screen,
    const Fonts *fonts,
    const char *directory,
    char *selected_path,
    size_t selected_size,
    bool *resume_without_prompt
)
{
    bool prev_up = false;
    bool prev_down = false;
    bool prev_left = false;
    bool prev_right = false;
    bool prev_enter = false;
    bool prev_esc = false;
    bool prev_scratchpad = false;
    bool prev_on = false;
    bool prev_c = false;
    bool prev_s = false;
    bool prev_2 = false;
    bool prev_4 = false;
    bool prev_5 = false;
    bool prev_6 = false;
    bool prev_8 = false;
    ScreenshotPreviewState screenshot_preview;
    PointerState pointer;
    PointerHoverGuard hover_guard;
    PickerTooltipHoverState tooltip_hover;
    MovieFile *files;
    size_t count = 0;
    size_t selected = 0;
    size_t scroll_start = 0;
    size_t previous_selected = 0;
    uint32_t selection_anim_started_ms = 0;
    uint8_t selected_start_mix = 0;
    uint8_t previous_start_mix = 0;
    uint32_t intro_started_ms = 0;
    UiTransition resume_badge_anim;
    UiTransition resume_tooltip_anim;
    UiTransition movie_tooltip_anim;
    UiTransition picker_press_anim;
    PickerScrollAnim scroll_anim;
    int resume_badge_anim_index = -1;
    int resume_tooltip_anim_index = -1;
    int movie_tooltip_anim_index = -1;
    int hover_scroll_direction = 0;
    uint32_t hover_scroll_last_ms = 0;
    int key_scroll_direction = 0;
    uint32_t key_scroll_repeat_next_ms = 0;
    int pressed_row_index = -1;
    int pressed_resume_badge_index = -1;
    bool pressed_selected_fallback = false;
    bool keyboard_resume_focused = false;
    bool deferred_movie_cleanup_done = false;
    int enter_press_stage = 0;

    memset(&screenshot_preview, 0, sizeof(screenshot_preview));
    memset(&resume_badge_anim, 0, sizeof(resume_badge_anim));
    memset(&resume_tooltip_anim, 0, sizeof(resume_tooltip_anim));
    memset(&movie_tooltip_anim, 0, sizeof(movie_tooltip_anim));
    memset(&picker_press_anim, 0, sizeof(picker_press_anim));
    memset(&scroll_anim, 0, sizeof(scroll_anim));
    ensure_movie_picker_cache(&g_picker_cache, directory);
    files = g_picker_cache.files;
    count = g_picker_cache.count;
    if (count > 0 && g_picker_cache.has_position) {
        selected = g_picker_cache.selected_index < count ? g_picker_cache.selected_index : count - 1;
        scroll_start = picker_scroll_start_for_selection(count, selected, g_picker_cache.scroll_start);
    } else {
        scroll_start = picker_scroll_start_centered(count, selected);
    }
    previous_selected = selected;
    if (resume_without_prompt) {
        *resume_without_prompt = false;
    }
    pointer_init(&pointer);
    pointer_update(&pointer);
    prev_up = isKeyPressed(KEY_NSPIRE_UP);
    prev_down = isKeyPressed(KEY_NSPIRE_DOWN);
    prev_left = isKeyPressed(KEY_NSPIRE_LEFT);
    prev_right = isKeyPressed(KEY_NSPIRE_RIGHT);
    prev_enter = isKeyPressed(KEY_NSPIRE_ENTER);
    prev_esc = isKeyPressed(KEY_NSPIRE_ESC);
    prev_scratchpad = isKeyPressed(KEY_NSPIRE_SCRATCHPAD);
    prev_on = on_key_pressed() ? true : false;
    prev_c = isKeyPressed(KEY_NSPIRE_C);
    prev_s = isKeyPressed(KEY_NSPIRE_S);
    prev_2 = isKeyPressed(KEY_NSPIRE_2);
    prev_4 = isKeyPressed(KEY_NSPIRE_4);
    prev_5 = isKeyPressed(KEY_NSPIRE_5);
    prev_6 = isKeyPressed(KEY_NSPIRE_6);
    prev_8 = isKeyPressed(KEY_NSPIRE_8);
    pointer_hover_guard_reset(&hover_guard);
    pointer_hover_guard_lock(&hover_guard, &pointer);
    picker_tooltip_hover_reset(&tooltip_hover);
    intro_started_ms = monotonic_clock_now_ms();
    while (1) {
        bool pointer_click = pointer_update(&pointer);
        uint32_t now_ms = monotonic_clock_now_ms();
        bool ctrl_down = isKeyPressed(KEY_NSPIRE_CTRL) ? true : false;
        bool keypad_2_edge = key_pressed_edge(KEY_NSPIRE_2, &prev_2);
        bool keypad_4_edge = key_pressed_edge(KEY_NSPIRE_4, &prev_4);
        bool keypad_5_edge = key_pressed_edge(KEY_NSPIRE_5, &prev_5);
        bool keypad_6_edge = key_pressed_edge(KEY_NSPIRE_6, &prev_6);
        bool keypad_8_edge = key_pressed_edge(KEY_NSPIRE_8, &prev_8);
        bool screenshot_edge = key_pressed_edge(KEY_NSPIRE_S, &prev_s);
        bool scratchpad_edge = key_pressed_edge(KEY_NSPIRE_SCRATCHPAD, &prev_scratchpad);
        bool esc_edge = key_pressed_edge(KEY_NSPIRE_ESC, &prev_esc);
        bool esc_down = prev_esc;
        bool theme_edge = key_pressed_edge(KEY_NSPIRE_C, &prev_c);
        bool on_edge = on_key_pressed_edge(&prev_on);
        bool enter_edge = key_pressed_edge(KEY_NSPIRE_ENTER, &prev_enter) || (!ctrl_down && keypad_5_edge);
        bool enter_down = prev_enter || (!ctrl_down && prev_5);
        bool up_edge = key_pressed_edge(KEY_NSPIRE_UP, &prev_up) || (!ctrl_down && keypad_8_edge);
        bool down_edge = key_pressed_edge(KEY_NSPIRE_DOWN, &prev_down) || (!ctrl_down && keypad_2_edge);
        bool up_down = prev_up || (!ctrl_down && prev_8);
        bool down_down = prev_down || (!ctrl_down && prev_2);
        bool left_edge = key_pressed_edge(KEY_NSPIRE_LEFT, &prev_left) || (!ctrl_down && keypad_4_edge);
        bool right_edge = key_pressed_edge(KEY_NSPIRE_RIGHT, &prev_right) || (!ctrl_down && keypad_6_edge);
        bool pointer_hover_allowed = pointer_hover_guard_allows(&hover_guard, &pointer, pointer_click);
        int next_hover_scroll_direction = pointer_hover_allowed && !pointer.down
            ? picker_hover_scroll_direction(count, scroll_start, &pointer)
            : 0;
        size_t scroll_draw_start;
        int scroll_draw_offset_y;
        int hovered_index;
        int resume_hovered_index;
        int active_resume_index;
        int tooltip_index;
        uint8_t resume_badge_mix;
        uint8_t resume_tooltip_mix;
        uint8_t movie_tooltip_mix;
        bool picker_press_hot;
        uint8_t picker_press_mix;
        int activated_index = -1;
        bool activated_resume = false;
        bool input_activity =
            pointer_click ||
            pointer.release_edge ||
            pointer.moved ||
            pointer.down ||
            scratchpad_edge ||
            esc_edge ||
            esc_down ||
            theme_edge ||
            screenshot_edge ||
            on_edge ||
            enter_edge ||
            enter_down ||
            up_edge ||
            down_edge ||
            up_down ||
            down_down ||
            left_edge ||
            right_edge;

        if ((g_display_power_state.idle_dim_active ||
                (g_display_power_state.off && g_display_power_state.off_from_idle)) &&
            input_activity) {
            display_power_restore(&g_display_power_state, now_ms);
            msleep(16);
            continue;
        }

        if (scratchpad_edge) {
            display_power_off_for_exit(&g_display_power_state, screen, true);
            clear_screenshot_preview(&screenshot_preview);
            return PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT;
        }
        if (g_display_power_state.off) {
            if (esc_down) {
                clear_screenshot_preview(&screenshot_preview);
                return PLAY_MOVIE_RESULT_HOME_EXIT;
            }
            if (on_edge) {
                display_power_restore(&g_display_power_state, now_ms);
            }
            msleep(16);
            continue;
        }
        if (on_edge) {
            display_power_off(&g_display_power_state, true);
            present_black_screen(screen);
            msleep(16);
            continue;
        }
        if (input_activity) {
            display_power_note_activity(&g_display_power_state, now_ms);
        }

        if (next_hover_scroll_direction != hover_scroll_direction) {
            hover_scroll_direction = next_hover_scroll_direction;
            hover_scroll_last_ms = 0;
        }
        if (hover_scroll_direction != 0 &&
            (hover_scroll_last_ms == 0 ||
                (uint32_t) (now_ms - hover_scroll_last_ms) >= PICKER_HOVER_SCROLL_REPEAT_MS)) {
            size_t next_scroll_start = scroll_start;

            if (hover_scroll_direction < 0 && next_scroll_start > 0) {
                --next_scroll_start;
            } else if (hover_scroll_direction > 0 && next_scroll_start + PICKER_VISIBLE_ROWS < count) {
                ++next_scroll_start;
            }
            if (next_scroll_start != scroll_start) {
                picker_scroll_to(count, &scroll_start, &scroll_anim, next_scroll_start, now_ms);
                hover_scroll_last_ms = now_ms;
            } else {
                hover_scroll_direction = 0;
                hover_scroll_last_ms = 0;
            }
        }
        picker_scroll_anim_view(&scroll_anim, scroll_start, now_ms, &scroll_draw_start, &scroll_draw_offset_y);
        hovered_index = pointer_hover_allowed
            ? picker_row_index_at(count, scroll_draw_start, scroll_draw_offset_y, pointer.x, pointer.y)
            : -1;
        resume_hovered_index = pointer_hover_allowed
            ? picker_resume_badge_index_at(fonts, files, count, scroll_draw_start, scroll_draw_offset_y, pointer.x, pointer.y)
            : -1;
        tooltip_index = resume_hovered_index >= 0
            ? -1
            : picker_tooltip_hover_update(&tooltip_hover, hovered_index, &pointer, pointer_click, now_ms);

        if (keyboard_resume_focused && pointer_hover_allowed && (hovered_index >= 0 || resume_hovered_index >= 0)) {
            keyboard_resume_focused = false;
        }
        if (hovered_index >= 0) {
            picker_set_selected_row(
                &selected,
                &previous_selected,
                &selection_anim_started_ms,
                &selected_start_mix,
                &previous_start_mix,
                (size_t) hovered_index,
                now_ms
            );
        }
        if (enter_press_stage == 0 && right_edge && count > 0 && files[selected].has_resume) {
            keyboard_resume_focused = true;
            pointer_hover_guard_lock(&hover_guard, &pointer);
        }
        if (enter_press_stage == 0 && left_edge && keyboard_resume_focused) {
            keyboard_resume_focused = false;
            pointer_hover_guard_lock(&hover_guard, &pointer);
        }
        active_resume_index = (keyboard_resume_focused && count > 0 && files[selected].has_resume)
            ? (int) selected
            : resume_hovered_index;
        if (active_resume_index >= 0 && resume_badge_anim_index != active_resume_index) {
            ui_transition_init(&resume_badge_anim, false);
            resume_badge_anim_index = active_resume_index;
        }
        resume_badge_mix = ui_transition_update(
            &resume_badge_anim,
            active_resume_index >= 0,
            now_ms,
            UI_HOVER_ANIM_MS
        );
        if (resume_badge_mix == 0 && active_resume_index < 0) {
            resume_badge_anim_index = -1;
        }
        if (resume_hovered_index >= 0 && resume_tooltip_anim_index != resume_hovered_index) {
            ui_transition_init(&resume_tooltip_anim, false);
            resume_tooltip_anim_index = resume_hovered_index;
        }
        resume_tooltip_mix = ui_transition_update(
            &resume_tooltip_anim,
            resume_hovered_index >= 0,
            now_ms,
            UI_TOOLTIP_ANIM_MS
        );
        if (resume_tooltip_mix == 0 && resume_hovered_index < 0) {
            resume_tooltip_anim_index = -1;
        }
        if (tooltip_index >= 0 && movie_tooltip_anim_index != tooltip_index) {
            ui_transition_init(&movie_tooltip_anim, false);
            movie_tooltip_anim_index = tooltip_index;
        }
        movie_tooltip_mix = ui_transition_update(
            &movie_tooltip_anim,
            tooltip_index >= 0,
            now_ms,
            UI_TOOLTIP_ANIM_MS
        );
        if (movie_tooltip_mix == 0 && tooltip_index < 0) {
            movie_tooltip_anim_index = -1;
        }
        if (pointer.press_edge) {
            if (count > 0 && resume_hovered_index >= 0) {
                pressed_resume_badge_index = resume_hovered_index;
                pressed_row_index = -1;
                pressed_selected_fallback = false;
            } else if (count > 0 && hovered_index >= 0) {
                pressed_row_index = hovered_index;
                pressed_resume_badge_index = -1;
                pressed_selected_fallback = false;
            } else if (count > 0 && keyboard_resume_focused && files[selected].has_resume) {
                pressed_row_index = -1;
                pressed_resume_badge_index = (int) selected;
                pressed_selected_fallback = true;
            } else if (count > 0) {
                pressed_row_index = (int) selected;
                pressed_resume_badge_index = -1;
                pressed_selected_fallback = true;
            } else {
                pressed_row_index = -1;
                pressed_resume_badge_index = -1;
                pressed_selected_fallback = false;
            }
        }
        if (enter_edge && count > 0 && enter_press_stage == 0) {
            enter_press_stage = 1;
            if (resume_hovered_index >= 0) {
                pressed_row_index = -1;
                pressed_resume_badge_index = resume_hovered_index;
                pressed_selected_fallback = false;
            } else if (keyboard_resume_focused && files[selected].has_resume) {
                pressed_row_index = -1;
                pressed_resume_badge_index = (int) selected;
                pressed_selected_fallback = false;
            } else {
                pressed_row_index = (int) selected;
                pressed_resume_badge_index = -1;
                pressed_selected_fallback = true;
            }
            ui_transition_init(&picker_press_anim, false);
        }
        if (enter_press_stage == 1 && !enter_down) {
            ui_transition_begin_press_release(&picker_press_anim, now_ms);
            if (pressed_row_index >= 0 || pressed_resume_badge_index >= 0) {
                activated_resume = pressed_resume_badge_index >= 0;
                activated_index = activated_resume ? pressed_resume_badge_index : pressed_row_index;
                selected = (size_t) activated_index;
                keyboard_resume_focused = false;
            }
            enter_press_stage = 0;
        }
        if (enter_press_stage == 0 && pointer.release_edge && count > 0) {
            if (pressed_resume_badge_index >= 0 && resume_hovered_index == pressed_resume_badge_index) {
                activated_index = pressed_resume_badge_index;
                activated_resume = true;
            } else if (pressed_selected_fallback && pressed_resume_badge_index >= 0) {
                activated_index = pressed_resume_badge_index;
                activated_resume = true;
            } else if (pressed_row_index >= 0 && hovered_index == pressed_row_index) {
                activated_index = pressed_row_index;
            } else if (pressed_selected_fallback && pressed_row_index >= 0) {
                activated_index = pressed_row_index;
            }
            if (activated_index >= 0) {
                selected = (size_t) activated_index;
                ui_transition_begin_press_release(&picker_press_anim, now_ms);
            }
        }
        picker_press_hot = (enter_press_stage == 1 && (pressed_row_index >= 0 || pressed_resume_badge_index >= 0)) || (pointer.down && (
            (pressed_resume_badge_index >= 0 && (
                (pressed_selected_fallback && pressed_resume_badge_index == (int) selected) ||
                resume_hovered_index == pressed_resume_badge_index
            )) ||
            (pressed_row_index >= 0 && (
                (pressed_selected_fallback && pressed_row_index == (int) selected) ||
                hovered_index == pressed_row_index
            ))
        ));
        picker_press_mix = ui_transition_update_press_ex(
            &picker_press_anim,
            picker_press_hot,
            now_ms,
            UI_PRESS_ANIM_MS,
            PICKER_PRESS_RELEASE_ANIM_MS
        );
        if (activated_index >= 0) {
            uint32_t exit_started_ms = monotonic_clock_now_ms();
            PointerState hidden_pointer = pointer;

            hidden_pointer.visible = false;
            clear_screenshot_preview(&screenshot_preview);
            while (1) {
                uint32_t exit_now_ms = monotonic_clock_now_ms();
                uint32_t exit_elapsed_ms = exit_now_ms - exit_started_ms;
                uint32_t loading_elapsed_ms = exit_elapsed_ms >= PICKER_EXIT_LOADING_DELAY_MS
                    ? exit_elapsed_ms - PICKER_EXIT_LOADING_DELAY_MS
                    : 0U;
                uint8_t loading_mix = ui_ease_out_cubic(loading_elapsed_ms, UI_LOADING_ANIM_MS);
                uint8_t exit_press_mix = ui_transition_update_press_ex(
                    &picker_press_anim,
                    false,
                    exit_now_ms,
                    UI_PRESS_ANIM_MS,
                    PICKER_PRESS_RELEASE_ANIM_MS
                );

                picker_scroll_anim_view(&scroll_anim, scroll_start, exit_now_ms, &scroll_draw_start, &scroll_draw_offset_y);
                render_picker(
                    screen,
                    fonts,
                    files,
                    count,
                    scroll_draw_start,
                    scroll_draw_offset_y,
                    selected,
                    previous_selected,
                    selection_anim_started_ms,
                    selected_start_mix,
                    previous_start_mix,
                    -1,
                    0,
                    -1,
                    0,
                    -1,
                    0,
                    pressed_row_index,
                    pressed_resume_badge_index,
                    exit_press_mix,
                    &hidden_pointer,
                    &screenshot_preview,
                    exit_now_ms,
                    intro_started_ms,
                    exit_elapsed_ms,
                    loading_mix,
                    "Loading",
                    (int) ((exit_now_ms / 180U) % 3U)
                );
                if (exit_elapsed_ms >= PICKER_EXIT_TO_LOADING_ANIM_MS && loading_mix >= 255) {
                    break;
                }
                msleep(16);
            }
            strncpy(selected_path, files[activated_index].path, selected_size - 1);
            selected_path[selected_size - 1] = '\0';
            movie_picker_cache_remember_position(&g_picker_cache, count, selected, scroll_start);
            if (resume_without_prompt) {
                *resume_without_prompt = activated_resume;
            }
            return 0;
        }
        if (enter_press_stage == 0 && !pointer.down && !pointer.release_edge && picker_press_mix == 0) {
            pressed_row_index = -1;
            pressed_resume_badge_index = -1;
            pressed_selected_fallback = false;
        }
        if (theme_edge) {
            ui_cycle_theme();
            ui_save_theme_for_directory(directory);
        }
        render_picker(
            screen,
            fonts,
            files,
            count,
            scroll_draw_start,
            scroll_draw_offset_y,
            selected,
            previous_selected,
            selection_anim_started_ms,
            selected_start_mix,
            previous_start_mix,
            movie_tooltip_anim_index,
            movie_tooltip_mix,
            resume_badge_anim_index,
            resume_badge_mix,
            resume_tooltip_anim_index,
            resume_tooltip_mix,
            pressed_row_index,
            pressed_resume_badge_index,
            picker_press_mix,
            &pointer,
            &screenshot_preview,
            now_ms,
            intro_started_ms,
            PICKER_EXIT_INACTIVE,
            0,
            NULL,
            0
        );
        if (screenshot_edge) {
            char saved_path[MAX_PATH_LEN];
            if (save_screenshot_bitmap_in_directory(screen, directory, saved_path, sizeof(saved_path))) {
                prepare_screenshot_preview(&screenshot_preview, screen, saved_path);
            }
        }
        if (display_power_tick_idle(&g_display_power_state, screen, monotonic_clock_now_ms(), true, true)) {
            msleep(16);
            continue;
        }
        if (!deferred_movie_cleanup_done &&
            g_deferred_playback_movie &&
            (int32_t) (now_ms - (intro_started_ms + PICKER_INTRO_ANIM_MS + 80U)) >= 0 &&
            enter_press_stage == 0 &&
            !pointer.down) {
            cleanup_deferred_playback_movie();
            deferred_movie_cleanup_done = true;
        }
        {
            int move_direction = 0;

            if (enter_press_stage == 0 && count > 0) {
                if (up_edge && !down_down) {
                    move_direction = -1;
                    key_scroll_direction = -1;
                    key_scroll_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
                } else if (down_edge && !up_down) {
                    move_direction = 1;
                    key_scroll_direction = 1;
                    key_scroll_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
                } else if (up_down != down_down) {
                    int held_direction = up_down ? -1 : 1;

                    if (key_scroll_direction != held_direction || key_scroll_repeat_next_ms == 0U) {
                        key_scroll_direction = held_direction;
                        key_scroll_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
                    } else if ((int32_t) (now_ms - key_scroll_repeat_next_ms) >= 0) {
                        move_direction = held_direction;
                        key_scroll_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_FALLBACK_INTERVAL_MS;
                    }
                } else {
                    key_scroll_direction = 0;
                    key_scroll_repeat_next_ms = 0;
                }
            } else {
                key_scroll_direction = 0;
                key_scroll_repeat_next_ms = 0;
            }
            if (move_direction != 0) {
                size_t next_selected = picker_adjacent_selection(count, selected, move_direction);

                keyboard_resume_focused = false;
                picker_set_selected_row(
                    &selected,
                    &previous_selected,
                    &selection_anim_started_ms,
                    &selected_start_mix,
                    &previous_start_mix,
                    next_selected,
                    now_ms
                );
                picker_scroll_to(
                    count,
                    &scroll_start,
                    &scroll_anim,
                    picker_scroll_start_for_selection(count, selected, scroll_start),
                    now_ms
                );
                pointer_hover_guard_lock(&hover_guard, &pointer);
            }
        }
        if (esc_edge) {
            clear_screenshot_preview(&screenshot_preview);
            return -1;
        }
        msleep(16);
    }
}

bool seek_delta_target_frame(const Movie *movie, int32_t delta_ms, uint32_t *out_target_frame)
{
    int64_t current_ms;
    int64_t target_ms;
    uint32_t duration_ms;
    uint32_t target_frame;
    if (!movie || !out_target_frame || movie->header.frame_count == 0) {
        return false;
    }
    current_ms = (int64_t) movie_frame_time_ms(movie, movie->current_frame);
    target_ms = current_ms + delta_ms;
    duration_ms = movie_duration_ms(movie);
    if (target_ms < 0) {
        target_ms = 0;
    }
    if ((uint64_t) target_ms >= duration_ms) {
        target_ms = duration_ms > 1 ? duration_ms - 1 : 0;
    }
    target_frame = movie_frames_from_ms(movie, (uint32_t) target_ms);
    if (target_frame >= movie->header.frame_count) {
        target_frame = movie->header.frame_count - 1;
    }
    *out_target_frame = target_frame;
    return true;
}

