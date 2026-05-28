#include "player_internal.h"

void draw_prompt_button(
    SDL_Surface *screen,
    const Fonts *fonts,
    const SDL_Rect *button,
    const char *label,
    uint8_t selection_mix,
    uint8_t press_mix
)
{
    SDL_Rect draw_button = *button;
    int press_offset_x = pressed_control_offset_x(press_mix);
    int press_offset_y = pressed_control_offset_y(press_mix);
    Uint16 base = rgb565_lerp(UI_COLOR_GUNMETAL, ui_theme()->row_selected, selection_mix, 255);
    int text_x;

    text_x = draw_button.x + (draw_button.w - nSDL_GetStringWidth(fonts->white, label)) / 2 + press_offset_x;
    draw_soft_glass_panel_mix(screen, &draw_button, pressed_control_base(base, press_mix), selection_mix);
    draw_pressed_control_reflection(screen, &draw_button, press_mix);
    draw_soft_glass_panel_rim(screen, &draw_button, base, max_u8(selection_mix, press_mix));
    draw_ui_label(screen, fonts, text_x, draw_button.y + 6 + press_offset_y, label);
}

int prompt_resume_position(
    SDL_Surface *screen,
    const Fonts *fonts,
    Movie *movie,
    const char *path,
    uint32_t resume_frame,
    ScaleMode scale_mode,
    VideoAlign video_align_x,
    VideoAlign video_align_y,
    SDL_Surface **loading_snapshot
)
{
    bool prev_left = false;
    bool prev_right = false;
    bool prev_enter = false;
    bool prev_4 = false;
    bool prev_5 = false;
    bool prev_6 = false;
    bool prev_esc = false;
    bool prev_scratchpad = false;
    bool prev_on = false;
    bool prev_c = false;
    bool prev_s = false;
    PointerState pointer;
    PointerHoverGuard hover_guard;
    ScreenshotPreviewState screenshot_preview;
    SDL_Rect playback_src;
    SDL_Rect playback_dst;
    SDL_Rect full_src;
    SDL_Rect full_screen = {0, 0, SCREEN_W, SCREEN_H};
    SDL_Rect border = {33, 23, 254, 194};
    SDL_Rect panel = {34, 24, 252, 192};
    SDL_Rect header = {34, 24, 252, 26};
    SDL_Rect accent = {34, 50, 252, 2};
    SDL_Rect title_panel = {44, 54, 232, 18};
    SDL_Rect preview = {80, 0, 160, 90};
    SDL_Rect preview_border = {79, 0, 162, 92};
    SDL_Rect continue_button = {52, 0, 90, 22};
    SDL_Rect restart_button = {178, 0, 90, 22};
    char time_label[64];
    char time_value[24];
    char *title_main = NULL;
    char *title_detail = NULL;
    char fitted_title_main[96];
    char fitted_title_detail[96];
    const char *filename = path;
    size_t selected_button = 0;
    size_t previous_selected_button = 0;
    UiTransition button_press_anim;
    int pressed_button = -1;
    int enter_button_press_stage = 0;
    bool pressed_button_fallback = false;
    uint32_t button_anim_started_ms = 0;
    uint32_t prompt_open_started_ms = 0;
    uint32_t prompt_close_started_ms = 0;
    uint32_t preview_ms;
    const int title_y = accent.y + 8;
    int preview_target_y;
    int max_button_y;
    int max_preview_h;
    int title_block_height;
    int title_main_height;
    int title_detail_height;
    int time_label_height;
    int time_label_y;
    int button_y;
    bool prompt_closing = false;
    int prompt_closing_result = 0;
    SDL_Surface *start_over_source_frame = NULL;

    memset(&screenshot_preview, 0, sizeof(screenshot_preview));
    memset(&button_press_anim, 0, sizeof(button_press_anim));
    if (resume_frame >= movie->header.frame_count) {
        resume_frame = movie->header.frame_count ? (movie->header.frame_count - 1) : 0;
    }
    if (!decode_to_frame(movie, resume_frame)) {
        finish_loading_transition(screen, loading_snapshot, fonts, "Loading");
        return 0;
    }
    compute_video_rects(movie, scale_mode, video_align_x, video_align_y, &playback_src, &playback_dst);
    full_src.x = 0;
    full_src.y = 0;
    full_src.w = movie->header.video_width;
    full_src.h = movie->header.video_height;
    preview_ms = movie_frame_time_ms(movie, movie->current_frame);
    format_clock(preview_ms, time_value, sizeof(time_value));
    snprintf(time_label, sizeof(time_label), "Resume from %s", time_value);
    if (strrchr(path, '/')) {
        filename = strrchr(path, '/') + 1;
    } else if (strrchr(path, '\\')) {
        filename = strrchr(path, '\\') + 1;
    }
    if (movie_display_fields_for_filename(filename, &title_main, &title_detail)) {
        copy_fitted_text(fonts->white, title_main, fitted_title_main, sizeof(fitted_title_main), panel.w - 24);
        if (title_detail) {
            copy_fitted_text(fonts->white, title_detail, fitted_title_detail, sizeof(fitted_title_detail), panel.w - 24);
            title_main_height = nSDL_GetStringHeight(fonts->white, fitted_title_main);
            title_detail_height = nSDL_GetStringHeight(fonts->white, fitted_title_detail);
        } else {
            title_main_height = nSDL_GetStringHeight(fonts->white, fitted_title_main);
            title_detail_height = 0;
        }
    } else {
        snprintf(fitted_title_main, sizeof(fitted_title_main), "%s", filename);
        title_main_height = nSDL_GetStringHeight(fonts->white, fitted_title_main);
        title_detail_height = 0;
    }
    title_block_height = title_main_height + (title_detail_height > 0 ? 2 + title_detail_height : 0);
    title_panel.x = (Sint16) (panel.x + 8);
    title_panel.y = (Sint16) (title_y - 4);
    title_panel.w = (Uint16) (panel.w - 16);
    title_panel.h = (Uint16) (title_block_height + 8);
    preview_target_y = title_y + title_block_height + 8;
    time_label_height = nSDL_GetStringHeight(fonts->white, time_label);
    max_button_y = panel.y + panel.h - continue_button.h - 8;
    max_preview_h = max_button_y - time_label_height - 14 - preview_target_y;
    max_preview_h = clamp_int(max_preview_h, 72, 90);
    preview.h = (Uint16) max_preview_h;
    preview.w = (Uint16) ((preview.h * 16 + 4) / 9);
    preview.x = (Sint16) ((SCREEN_W - preview.w) / 2);
    preview.y = (Sint16) preview_target_y;
    preview_border.x = (Sint16) (preview.x - 1);
    preview_border.y = (Sint16) (preview.y - 1);
    preview_border.w = (Uint16) (preview.w + 2);
    preview_border.h = (Uint16) (preview.h + 2);
    time_label_y = preview.y + preview.h + 7;
    button_y = time_label_y + time_label_height + 7;
    if (button_y > max_button_y) {
        button_y = max_button_y;
    }
    continue_button.y = (Sint16) button_y;
    restart_button.y = (Sint16) button_y;
    pointer_init(&pointer);
    pointer_update(&pointer);
    prev_left = isKeyPressed(KEY_NSPIRE_LEFT);
    prev_right = isKeyPressed(KEY_NSPIRE_RIGHT);
    prev_enter = isKeyPressed(KEY_NSPIRE_ENTER);
    prev_4 = isKeyPressed(KEY_NSPIRE_4);
    prev_5 = isKeyPressed(KEY_NSPIRE_5);
    prev_6 = isKeyPressed(KEY_NSPIRE_6);
    prev_esc = isKeyPressed(KEY_NSPIRE_ESC);
    prev_scratchpad = isKeyPressed(KEY_NSPIRE_SCRATCHPAD);
    prev_on = on_key_pressed() ? true : false;
    prev_c = isKeyPressed(KEY_NSPIRE_C);
    prev_s = isKeyPressed(KEY_NSPIRE_S);
    pointer_hover_guard_reset(&hover_guard);
    prompt_open_started_ms = monotonic_clock_now_ms();

    while (1) {
        bool pointer_click = pointer_update(&pointer);
        bool pointer_hover_allowed = pointer_hover_guard_allows(&hover_guard, &pointer);
        uint32_t now_ms = monotonic_clock_now_ms();
        bool ctrl_down = isKeyPressed(KEY_NSPIRE_CTRL) ? true : false;
        bool keypad_4_edge = key_pressed_edge(KEY_NSPIRE_4, &prev_4);
        bool keypad_5_edge = key_pressed_edge(KEY_NSPIRE_5, &prev_5);
        bool keypad_6_edge = key_pressed_edge(KEY_NSPIRE_6, &prev_6);
        bool screenshot_edge = key_pressed_edge(KEY_NSPIRE_S, &prev_s);
        bool scratchpad_edge = key_pressed_edge(KEY_NSPIRE_SCRATCHPAD, &prev_scratchpad);
        bool esc_edge = key_pressed_edge(KEY_NSPIRE_ESC, &prev_esc);
        bool esc_down = prev_esc;
        bool theme_edge = key_pressed_edge(KEY_NSPIRE_C, &prev_c);
        bool on_edge = on_key_pressed_edge(&prev_on);
        bool enter_edge = key_pressed_edge(KEY_NSPIRE_ENTER, &prev_enter) || (!ctrl_down && keypad_5_edge);
        bool enter_down = prev_enter || (!ctrl_down && prev_5);
        bool left_edge = key_pressed_edge(KEY_NSPIRE_LEFT, &prev_left) || (!ctrl_down && keypad_4_edge);
        bool right_edge = key_pressed_edge(KEY_NSPIRE_RIGHT, &prev_right) || (!ctrl_down && keypad_6_edge);
        bool prompt_canceling = prompt_closing && prompt_closing_result < 0;
        uint32_t prompt_open_elapsed_ms = now_ms - prompt_open_started_ms;
        uint8_t prompt_open_mix = !prompt_closing
            ? ui_ease_out_cubic(prompt_open_elapsed_ms, RESUME_PROMPT_OPEN_MORPH_ANIM_MS)
            : 0;
        uint8_t prompt_header_mix = !prompt_closing
            ? staggered_content_mix(prompt_open_elapsed_ms, RESUME_PROMPT_CONTENT_START_MS, RESUME_PROMPT_CONTENT_STAGGER_MS, 0, RESUME_PROMPT_CONTENT_ITEM_ANIM_MS)
            : 0;
        uint8_t prompt_title_mix = !prompt_closing
            ? staggered_content_mix(prompt_open_elapsed_ms, RESUME_PROMPT_CONTENT_START_MS, RESUME_PROMPT_CONTENT_STAGGER_MS, 1, RESUME_PROMPT_CONTENT_ITEM_ANIM_MS)
            : 0;
        uint8_t prompt_preview_mix = !prompt_closing
            ? staggered_content_mix(prompt_open_elapsed_ms, RESUME_PROMPT_CONTENT_START_MS, RESUME_PROMPT_CONTENT_STAGGER_MS, 2, RESUME_PROMPT_CONTENT_ITEM_ANIM_MS)
            : 0;
        uint8_t prompt_time_mix = !prompt_closing
            ? staggered_content_mix(prompt_open_elapsed_ms, RESUME_PROMPT_CONTENT_START_MS, RESUME_PROMPT_CONTENT_STAGGER_MS, 3, RESUME_PROMPT_CONTENT_ITEM_ANIM_MS)
            : 0;
        uint8_t prompt_buttons_mix = !prompt_closing
            ? staggered_content_mix(prompt_open_elapsed_ms, RESUME_PROMPT_CONTENT_START_MS, RESUME_PROMPT_CONTENT_STAGGER_MS, 4, RESUME_PROMPT_CONTENT_ITEM_ANIM_MS)
            : 0;
        uint8_t prompt_content_mix = max_u8(max_u8(prompt_header_mix, prompt_title_mix), max_u8(prompt_preview_mix, max_u8(prompt_time_mix, prompt_buttons_mix)));
        uint8_t loading_text_mix = !prompt_closing
            ? (uint8_t) (255U - ui_ease_out_cubic(prompt_open_elapsed_ms, RESUME_PROMPT_LOADING_TEXT_EXIT_MS))
            : 0;
        uint8_t background_reveal_mix = (!prompt_closing && loading_snapshot && *loading_snapshot)
            ? prompt_open_mix
            : 255;
        uint8_t loading_mix = (uint8_t) (255U - prompt_open_mix);
        uint32_t prompt_close_elapsed_ms = prompt_closing ? (now_ms - prompt_close_started_ms) : 0;
        uint8_t close_mix = prompt_closing
            ? ui_ease_out_cubic(prompt_close_elapsed_ms, prompt_canceling ? RESUME_PROMPT_ANIM_MS : RESUME_COMMIT_PROMPT_ANIM_MS)
            : 0;
        uint8_t morph_mix = prompt_closing
            ? (prompt_canceling
                ? ui_ease_smoothstep(prompt_close_elapsed_ms, UI_RETURN_COLLAPSE_ANIM_MS)
                : ui_ease_out_cubic(prompt_close_elapsed_ms, RESUME_COMMIT_MORPH_ANIM_MS))
            : 0;
        uint8_t prompt_mix = prompt_closing
            ? (uint8_t) (255U - close_mix)
            : prompt_open_mix;
        uint8_t prompt_style_mix = 255;
        bool draw_prompt_contents = prompt_closing
            ? (prompt_canceling ? prompt_mix > 40 : true)
            : prompt_content_mix > 0;
        int prompt_y_offset = prompt_closing ? (((255 - prompt_mix) * 5 + 127) / 255) : 0;
        int dim_strength = prompt_closing
            ? ((RESUME_PROMPT_DIM_ALPHA * prompt_mix + 127) / 255)
            : ((UI_LOADING_DIM_ALPHA * (int) loading_mix + RESUME_PROMPT_DIM_ALPHA * (int) background_reveal_mix + 127) / 255);
        SDL_Rect background_src = full_src;
        SDL_Rect background_dst = full_screen;
        SDL_Rect draw_border = border;
        SDL_Rect draw_panel = panel;
        SDL_Rect draw_header = header;
        SDL_Rect draw_accent = accent;
        SDL_Rect draw_title_panel = title_panel;
        SDL_Rect draw_preview = preview;
        SDL_Rect draw_preview_border = preview_border;
        SDL_Rect draw_continue_button = continue_button;
        SDL_Rect draw_restart_button = restart_button;
        SDL_Rect loading_border = {88, 92, 136, 30};
        SDL_Rect loading_panel = {89, 93, 134, 28};
        SDL_Rect loading_accent = {90, 93, 132, 1};
        int draw_title_y = title_y + prompt_y_offset;
        int draw_time_label_y = time_label_y + prompt_y_offset;
        int hovered_button = -1;
        bool button_press_hot;
        uint8_t button_press_mix;
        bool woke_from_idle_off = false;
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
            left_edge ||
            right_edge;

        if ((g_display_power_state.idle_dim_active ||
                (g_display_power_state.off && g_display_power_state.off_from_idle)) &&
            input_activity) {
            woke_from_idle_off = g_display_power_state.off && g_display_power_state.off_from_idle;
            display_power_restore_animated(&g_display_power_state, now_ms);
        }

        if (scratchpad_edge) {
            display_power_off_for_exit(&g_display_power_state, screen, true);
            free(title_main);
            free(title_detail);
            if (start_over_source_frame) {
                SDL_FreeSurface(start_over_source_frame);
            }
            clear_screenshot_preview(&screenshot_preview);
            return RESUME_PROMPT_RESULT_SCRATCHPAD_EXIT;
        }
        if (g_display_power_state.off) {
            if (esc_down) {
                free(title_main);
                free(title_detail);
                if (start_over_source_frame) {
                    SDL_FreeSurface(start_over_source_frame);
                }
                clear_screenshot_preview(&screenshot_preview);
                return RESUME_PROMPT_RESULT_HOME_EXIT;
            }
            if (on_edge) {
                display_power_restore(&g_display_power_state, now_ms);
            }
            msleep(16);
            continue;
        }
        if (on_edge && !woke_from_idle_off) {
            display_power_off(&g_display_power_state, true);
            present_black_screen(screen);
            msleep(16);
            continue;
        }
        if (input_activity) {
            display_power_note_activity(&g_display_power_state, now_ms);
        }

        if (prompt_closing) {
            if (prompt_canceling) {
                background_dst = movie_vertical_morph_dst(&full_screen, (uint8_t) (255U - morph_mix));
            } else if (morph_mix >= 255) {
                background_src = playback_src;
                background_dst = playback_dst;
            } else {
                background_src = mix_sdl_rects(&full_src, &playback_src, morph_mix);
                background_dst = mix_sdl_rects(&full_screen, &playback_dst, morph_mix);
            }
        } else if (background_reveal_mix < 255) {
            background_dst = movie_vertical_morph_dst(&full_screen, background_reveal_mix);
        }
        draw_border.y = (Sint16) (draw_border.y + prompt_y_offset);
        draw_panel.y = (Sint16) (draw_panel.y + prompt_y_offset);
        draw_header.y = (Sint16) (draw_header.y + prompt_y_offset);
        draw_accent.y = (Sint16) (draw_accent.y + prompt_y_offset);
        draw_title_panel.y = (Sint16) (draw_title_panel.y + prompt_y_offset);
        draw_preview.y = (Sint16) (draw_preview.y + prompt_y_offset);
        draw_preview_border.y = (Sint16) (draw_preview_border.y + prompt_y_offset);
        draw_continue_button.y = (Sint16) (draw_continue_button.y + prompt_y_offset);
        draw_restart_button.y = (Sint16) (draw_restart_button.y + prompt_y_offset);
        if (!prompt_closing) {
            draw_border = mix_sdl_rects(&loading_border, &border, prompt_mix);
            draw_panel = mix_sdl_rects(&loading_panel, &panel, prompt_mix);
            draw_header = mix_sdl_rects(&loading_panel, &header, prompt_mix);
            draw_accent = mix_sdl_rects(&loading_accent, &accent, prompt_mix);
            draw_title_panel = title_panel;
            draw_preview = preview;
            draw_preview_border = preview_border;
            draw_continue_button = continue_button;
            draw_restart_button = restart_button;
            draw_title_y = title_y;
            draw_time_label_y = time_label_y;
        }

        if (!prompt_closing) {
            if (pointer_hover_allowed) {
                if (pointer.x >= continue_button.x && pointer.x < continue_button.x + continue_button.w &&
                    pointer.y >= continue_button.y && pointer.y < continue_button.y + continue_button.h) {
                    hovered_button = 0;
                    picker_set_selected(
                        &selected_button,
                        &previous_selected_button,
                        &button_anim_started_ms,
                        0,
                        now_ms
                    );
                } else if (pointer.x >= restart_button.x && pointer.x < restart_button.x + restart_button.w &&
                           pointer.y >= restart_button.y && pointer.y < restart_button.y + restart_button.h) {
                    hovered_button = 1;
                    picker_set_selected(
                        &selected_button,
                        &previous_selected_button,
                        &button_anim_started_ms,
                        1,
                        now_ms
                    );
                }
            }
            if (enter_button_press_stage == 0 && (left_edge || right_edge)) {
                picker_set_selected(
                    &selected_button,
                    &previous_selected_button,
                    &button_anim_started_ms,
                    1 - selected_button,
                    now_ms
                );
                pointer_hover_guard_lock(&hover_guard, &pointer);
            }
        }
        if (!prompt_closing && pointer.press_edge) {
            if (hovered_button >= 0) {
                pressed_button = hovered_button;
                pressed_button_fallback = false;
            } else {
                pressed_button = (int) selected_button;
                pressed_button_fallback = true;
            }
        }
        if (!prompt_closing && enter_edge && enter_button_press_stage == 0) {
            enter_button_press_stage = 1;
            pressed_button = (int) selected_button;
            pressed_button_fallback = true;
            ui_transition_init(&button_press_anim, false);
        }
        if (enter_button_press_stage == 1 && !enter_down) {
            enter_button_press_stage = 2;
            ui_transition_begin_press_release(&button_press_anim, now_ms);
        }
        if (!prompt_closing &&
            enter_button_press_stage == 0 &&
            pointer.release_edge &&
            pressed_button >= 0 &&
            (hovered_button == pressed_button || pressed_button_fallback)) {
            enter_button_press_stage = 2;
            ui_transition_begin_press_release(&button_press_anim, now_ms);
        }
        button_press_hot = !prompt_closing && pressed_button >= 0 && (
            enter_button_press_stage == 1 ||
            (pointer.down && (
                hovered_button == pressed_button ||
                (pressed_button_fallback && pressed_button == (int) selected_button)
            ))
        );
        button_press_mix = ui_transition_update_press(
            &button_press_anim,
            button_press_hot,
            now_ms
        );
        if (enter_button_press_stage == 0 && !pointer.down && !pointer.release_edge && button_press_mix == 0) {
            pressed_button = -1;
            pressed_button_fallback = false;
        }

        if (theme_edge) {
            ui_cycle_theme();
            ui_save_theme_for_movie(path);
        }
        if (!prompt_closing && loading_snapshot && *loading_snapshot) {
            SDL_BlitSurface(*loading_snapshot, NULL, screen, NULL);
        } else {
            SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
        }
        if (background_dst.w > 0 && background_dst.h > 0) {
            if (prompt_closing &&
                !prompt_canceling &&
                prompt_closing_result == 0 &&
                start_over_source_frame &&
                morph_mix < 255) {
                draw_surface_frame_scaled_clipped(screen, start_over_source_frame, &background_src, &background_dst);
                draw_surface_frame_scaled_clipped_mix(screen, movie->frame_surface, &background_src, &background_dst, morph_mix);
            } else {
                draw_movie_frame_scaled_clipped(screen, movie, &background_src, &background_dst);
            }
        }
        dim_rect_rgb565(screen, &full_screen, dim_strength);
        if (!prompt_closing && (prompt_mix > 0 || loading_text_mix > 0 || prompt_content_mix > 0)) {
            Uint16 panel_base = rgb565_lerp(UI_COLOR_GUNMETAL, ui_theme()->modal_panel, prompt_mix, 255);

            fill_soft_panel_backplate(
                screen,
                &draw_border,
                rgb565_lerp(UI_COLOR_GUNMETAL, ui_theme()->menu_border, prompt_mix, 255)
            );
            draw_soft_glass_panel_body_from_y(
                screen,
                &draw_panel,
                draw_accent.y + draw_accent.h,
                panel_base
            );
            draw_soft_glass_panel_top_mix(
                screen,
                &draw_header,
                UI_COLOR_GUNMETAL,
                0
            );
            draw_vertical_gradient(
                screen,
                &draw_accent,
                UI_COLOR_ACCENT,
                UI_COLOR_ACCENT_DEEP
            );
            if (loading_text_mix > 0) {
                SDL_Rect loading_label_border = loading_border;
                int loading_label_offset_y = -(((255 - loading_text_mix) * 8 + 127) / 255);

                loading_label_border.y = (Sint16) (loading_label_border.y + loading_label_offset_y);
                draw_loading_overlay_label_mix(
                    screen,
                    fonts,
                    &loading_label_border,
                    "Loading",
                    (int) ((prompt_open_elapsed_ms / 180U) % 3U),
                    loading_text_mix
                );
            }
            if (draw_prompt_contents) {
                if (prompt_header_mix > 0) {
                    SDL_Surface *item_surface;
                    SDL_Surface *item_target;
                    SDL_Rect old_clip;
                    int dx;
                    int dy;
                    int item_y_offset = ((255 - prompt_header_mix) * 4 + 127) / 255;

                    item_surface = begin_faded_region_draw(screen, &draw_border, prompt_header_mix, &item_target, &old_clip, &dx, &dy);
                    draw_ui_label(item_target, fonts, panel.x + 12 + dx, panel.y + 8 + dy + item_y_offset, "Continue Watching?");
                    end_faded_region_draw(screen, item_surface, &draw_border, &old_clip, prompt_header_mix);
                }
                if (prompt_title_mix > 0) {
                    SDL_Surface *item_surface;
                    SDL_Surface *item_target;
                    SDL_Rect old_clip;
                    SDL_Rect content_title_panel;
                    int dx;
                    int dy;
                    int item_y_offset = ((255 - prompt_title_mix) * 4 + 127) / 255;

                    item_surface = begin_faded_region_draw(screen, &draw_border, prompt_title_mix, &item_target, &old_clip, &dx, &dy);
                    content_title_panel = offset_sdl_rect(&draw_title_panel, dx, dy + item_y_offset);
                    draw_soft_glass_panel_mix(item_target, &content_title_panel, ui_theme()->modal_title_panel, 0);
                    draw_ui_label(item_target, fonts, panel.x + 12 + dx, draw_title_y + dy + item_y_offset, fitted_title_main);
                    if (title_detail_height > 0) {
                        draw_ui_label(item_target, fonts, panel.x + 12 + dx, draw_title_y + title_main_height + 2 + dy + item_y_offset, fitted_title_detail);
                    }
                    end_faded_region_draw(screen, item_surface, &draw_border, &old_clip, prompt_title_mix);
                }
                if (prompt_preview_mix > 0) {
                    SDL_Surface *item_surface;
                    SDL_Surface *item_target;
                    SDL_Rect old_clip;
                    SDL_Rect content_preview;
                    SDL_Rect content_preview_border;
                    int dx;
                    int dy;
                    int item_y_offset = ((255 - prompt_preview_mix) * 4 + 127) / 255;

                    item_surface = begin_faded_region_draw(screen, &draw_border, prompt_preview_mix, &item_target, &old_clip, &dx, &dy);
                    content_preview = offset_sdl_rect(&draw_preview, dx, dy + item_y_offset);
                    content_preview_border = offset_sdl_rect(&draw_preview_border, dx, dy + item_y_offset);
                    if (content_preview.x >= 0 &&
                        content_preview.y >= 0 &&
                        content_preview.x + content_preview.w <= item_target->w &&
                        content_preview.y + content_preview.h <= item_target->h) {
                        fill_rect_rgb565(item_target, &content_preview_border, UI_COLOR_WARM_WHITE);
                        SDL_SoftStretch(movie->frame_surface, NULL, item_target, &content_preview);
                    }
                    end_faded_region_draw(screen, item_surface, &draw_border, &old_clip, prompt_preview_mix);
                }
                if (prompt_time_mix > 0) {
                    SDL_Surface *item_surface;
                    SDL_Surface *item_target;
                    SDL_Rect old_clip;
                    int dx;
                    int dy;
                    int item_y_offset = ((255 - prompt_time_mix) * 4 + 127) / 255;

                    item_surface = begin_faded_region_draw(screen, &draw_border, prompt_time_mix, &item_target, &old_clip, &dx, &dy);
                    draw_ui_label(item_target, fonts, panel.x + 12 + dx, draw_time_label_y + dy + item_y_offset, time_label);
                    end_faded_region_draw(screen, item_surface, &draw_border, &old_clip, prompt_time_mix);
                }
                if (prompt_buttons_mix > 0) {
                    SDL_Surface *item_surface;
                    SDL_Surface *item_target;
                    SDL_Rect old_clip;
                    SDL_Rect content_continue_button;
                    SDL_Rect content_restart_button;
                    int dx;
                    int dy;
                    int item_y_offset = ((255 - prompt_buttons_mix) * 4 + 127) / 255;

                    item_surface = begin_faded_region_draw(screen, &draw_border, prompt_buttons_mix, &item_target, &old_clip, &dx, &dy);
                    content_continue_button = offset_sdl_rect(&draw_continue_button, dx, dy + item_y_offset);
                    content_restart_button = offset_sdl_rect(&draw_restart_button, dx, dy + item_y_offset);
                    draw_prompt_button(
                        item_target,
                        fonts,
                        &content_continue_button,
                        "CONTINUE",
                        prompt_button_selection_mix(0, selected_button, previous_selected_button, button_anim_started_ms, now_ms),
                        pressed_button == 0 ? button_press_mix : 0
                    );
                    draw_prompt_button(
                        item_target,
                        fonts,
                        &content_restart_button,
                        "START OVER",
                        prompt_button_selection_mix(1, selected_button, previous_selected_button, button_anim_started_ms, now_ms),
                        pressed_button == 1 ? button_press_mix : 0
                    );
                    end_faded_region_draw(screen, item_surface, &draw_border, &old_clip, prompt_buttons_mix);
                }
            }
        } else if (prompt_canceling && prompt_mix > 0) {
            Uint16 panel_base = rgb565_lerp(UI_COLOR_BLACK, ui_theme()->modal_panel, prompt_mix, 255);

            fill_soft_panel_backplate(screen, &draw_border, ui_theme()->menu_border);
            draw_soft_glass_panel_body_from_y(
                screen,
                &draw_panel,
                draw_accent.y + draw_accent.h,
                panel_base
            );
            draw_soft_glass_panel_top_mix(
                screen,
                &draw_header,
                rgb565_lerp(UI_COLOR_BLACK, UI_COLOR_GUNMETAL, prompt_mix, 255),
                0
            );
            draw_vertical_gradient(
                screen,
                &draw_accent,
                rgb565_lerp(ui_theme()->modal_panel, UI_COLOR_ACCENT, prompt_mix, 255),
                rgb565_lerp(ui_theme()->modal_panel, UI_COLOR_ACCENT_DEEP, prompt_mix, 255)
            );
            draw_soft_glass_panel_mix(
                screen,
                &draw_title_panel,
                rgb565_lerp(UI_COLOR_BLACK, ui_theme()->modal_title_panel, prompt_mix, 255),
                0
            );
            fill_rect_rgb565(screen, &draw_preview_border, UI_COLOR_WARM_WHITE);
            SDL_SoftStretch(movie->frame_surface, NULL, screen, &draw_preview);
            if (draw_prompt_contents) {
                draw_ui_label(screen, fonts, draw_panel.x + 12, draw_panel.y + 8, "Continue Watching?");
                draw_ui_label(screen, fonts, draw_panel.x + 12, draw_title_y, fitted_title_main);
                if (title_detail_height > 0) {
                    draw_ui_label(screen, fonts, draw_panel.x + 12, draw_title_y + title_main_height + 2, fitted_title_detail);
                }
                draw_ui_label(screen, fonts, draw_panel.x + 12, draw_time_label_y, time_label);
                draw_prompt_button(
                    screen,
                    fonts,
                    &draw_continue_button,
                    "CONTINUE",
                    prompt_button_selection_mix(0, selected_button, previous_selected_button, button_anim_started_ms, now_ms),
                    pressed_button == 0 ? button_press_mix : 0
                );
                draw_prompt_button(
                    screen,
                    fonts,
                    &draw_restart_button,
                    "START OVER",
                    prompt_button_selection_mix(1, selected_button, previous_selected_button, button_anim_started_ms, now_ms),
                    pressed_button == 1 ? button_press_mix : 0
                );
            }
        } else if (prompt_mix > 0) {
            SDL_Surface *prompt_surface = NULL;
            SDL_Surface *prompt_target = screen;
            SDL_Rect prompt_blit = {0, 0, SCREEN_W, SCREEN_H};
            Uint16 panel_base = rgb565_lerp(UI_COLOR_BLACK, ui_theme()->modal_panel, prompt_style_mix, 255);

            if (prompt_closing && !prompt_canceling && prompt_mix < 255) {
                prompt_surface = capture_screen_surface(screen);
                if (prompt_surface) {
                    prompt_target = prompt_surface;
                }
            }
            fill_soft_panel_backplate(
                prompt_target,
                &draw_border,
                rgb565_lerp(UI_COLOR_BLACK, ui_theme()->menu_border, prompt_style_mix, 255)
            );
            draw_soft_glass_panel_body_from_y(
                prompt_target,
                &draw_panel,
                draw_accent.y + draw_accent.h,
                panel_base
            );
            draw_soft_glass_panel_top_mix(
                prompt_target,
                &draw_header,
                rgb565_lerp(UI_COLOR_BLACK, UI_COLOR_GUNMETAL, prompt_style_mix, 255),
                0
            );
            draw_vertical_gradient(
                prompt_target,
                &draw_accent,
                rgb565_lerp(ui_theme()->modal_panel, UI_COLOR_ACCENT, prompt_style_mix, 255),
                rgb565_lerp(ui_theme()->modal_panel, UI_COLOR_ACCENT_DEEP, prompt_style_mix, 255)
            );
            draw_soft_glass_panel_mix(
                prompt_target,
                &draw_title_panel,
                rgb565_lerp(UI_COLOR_BLACK, ui_theme()->modal_title_panel, prompt_style_mix, 255),
                0
            );
            fill_rect_rgb565(
                prompt_target,
                &draw_preview_border,
                rgb565_lerp(UI_COLOR_BLACK, UI_COLOR_WARM_WHITE, prompt_style_mix, 255)
            );
            SDL_SoftStretch(movie->frame_surface, NULL, prompt_target, &draw_preview);
            if (draw_prompt_contents) {
                draw_ui_label(prompt_target, fonts, draw_panel.x + 12, draw_panel.y + 8, "Continue Watching?");
                draw_ui_label(prompt_target, fonts, draw_panel.x + 12, draw_title_y, fitted_title_main);
                if (title_detail_height > 0) {
                    draw_ui_label(prompt_target, fonts, draw_panel.x + 12, draw_title_y + title_main_height + 2, fitted_title_detail);
                }
                draw_ui_label(prompt_target, fonts, draw_panel.x + 12, draw_time_label_y, time_label);
                draw_prompt_button(
                    prompt_target,
                    fonts,
                    &draw_continue_button,
                    "CONTINUE",
                    prompt_button_selection_mix(0, selected_button, previous_selected_button, button_anim_started_ms, now_ms),
                    pressed_button == 0 ? button_press_mix : 0
                );
                draw_prompt_button(
                    prompt_target,
                    fonts,
                    &draw_restart_button,
                    "START OVER",
                    prompt_button_selection_mix(1, selected_button, previous_selected_button, button_anim_started_ms, now_ms),
                    pressed_button == 1 ? button_press_mix : 0
                );
            }
            if (prompt_surface) {
                blit_surface_rgb565_mix(screen, prompt_surface, &prompt_blit, prompt_mix);
                SDL_FreeSurface(prompt_surface);
            }
        }
        draw_screenshot_preview_osd(screen, fonts, &screenshot_preview, now_ms);
        if (pointer.visible) {
            draw_cursor(screen, pointer.x, pointer.y);
        }
        present_screen(screen);
        if (prompt_closing && (prompt_canceling ? morph_mix >= 255 : (close_mix >= 255 && morph_mix >= 255))) {
            if (prompt_canceling) {
                present_black_screen(screen);
            }
            free(title_main);
            free(title_detail);
            if (start_over_source_frame) {
                SDL_FreeSurface(start_over_source_frame);
            }
            clear_screenshot_preview(&screenshot_preview);
            return prompt_closing_result;
        }
        if (screenshot_edge) {
            char saved_path[MAX_PATH_LEN];
            if (save_screenshot_bitmap(screen, path, saved_path, sizeof(saved_path))) {
                prepare_screenshot_preview(&screenshot_preview, screen, saved_path);
            }
        }
        if (loading_snapshot && *loading_snapshot && background_reveal_mix >= 255) {
            SDL_FreeSurface(*loading_snapshot);
            *loading_snapshot = NULL;
        }
        if (!prompt_closing && display_power_tick_idle(&g_display_power_state, screen, monotonic_clock_now_ms(), true, true)) {
            msleep(16);
            continue;
        }

        if (prompt_closing) {
            msleep(16);
            continue;
        }
        if (enter_button_press_stage != 0) {
            if (enter_button_press_stage == 2 && button_press_mix == 0 && pressed_button >= 0) {
                uint32_t close_started_ms;

                if (pressed_button == 1) {
                    SDL_Rect frame_rect = {0, 0, 0, 0};

                    if (start_over_source_frame) {
                        SDL_FreeSurface(start_over_source_frame);
                        start_over_source_frame = NULL;
                    }
                    if (movie->frame_surface) {
                        frame_rect.w = movie->frame_surface->w;
                        frame_rect.h = movie->frame_surface->h;
                        start_over_source_frame = capture_rect_surface(movie->frame_surface, &frame_rect);
                    }
                    if (!decode_to_frame(movie, 0) && start_over_source_frame) {
                        SDL_FreeSurface(start_over_source_frame);
                        start_over_source_frame = NULL;
                    }
                }
                close_started_ms = monotonic_clock_now_ms();
                prompt_closing = true;
                prompt_closing_result = pressed_button == 0 ? 1 : 0;
                prompt_close_started_ms = close_started_ms ? close_started_ms : 1U;
                enter_button_press_stage = 0;
                continue;
            }
        }
        if (esc_edge) {
            prompt_closing = true;
            prompt_closing_result = -1;
            prompt_close_started_ms = now_ms ? now_ms : 1U;
            continue;
        }
        msleep(16);
    }
}

