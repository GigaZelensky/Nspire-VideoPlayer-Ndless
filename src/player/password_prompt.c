#include "player_internal.h"
#include "movie_crypto_session.h"

/* These are text-entry keys, not playback shortcuts. In particular S, N,
 * Space and the numeric keypad must never activate player controls here. */
static const t_key *const text_keys[] = {
    &KEY_NSPIRE_A,&KEY_NSPIRE_B,&KEY_NSPIRE_C,&KEY_NSPIRE_D,&KEY_NSPIRE_E,&KEY_NSPIRE_F,
    &KEY_NSPIRE_G,&KEY_NSPIRE_H,&KEY_NSPIRE_I,&KEY_NSPIRE_J,&KEY_NSPIRE_K,&KEY_NSPIRE_L,
    &KEY_NSPIRE_M,&KEY_NSPIRE_N,&KEY_NSPIRE_O,&KEY_NSPIRE_P,&KEY_NSPIRE_Q,&KEY_NSPIRE_R,
    &KEY_NSPIRE_S,&KEY_NSPIRE_T,&KEY_NSPIRE_U,&KEY_NSPIRE_V,&KEY_NSPIRE_W,&KEY_NSPIRE_X,
    &KEY_NSPIRE_Y,&KEY_NSPIRE_Z,&KEY_NSPIRE_0,&KEY_NSPIRE_1,&KEY_NSPIRE_2,&KEY_NSPIRE_3,
    &KEY_NSPIRE_4,&KEY_NSPIRE_5,&KEY_NSPIRE_6,&KEY_NSPIRE_7,&KEY_NSPIRE_8,&KEY_NSPIRE_9,
    &KEY_NSPIRE_SPACE,&KEY_NSPIRE_PERIOD,&KEY_NSPIRE_COMMA,&KEY_NSPIRE_MINUS,&KEY_NSPIRE_PLUS,
    &KEY_NSPIRE_DIVIDE,&KEY_NSPIRE_MULTIPLY,&KEY_NSPIRE_LP,&KEY_NSPIRE_RP
};
static const char text_plain[] = "abcdefghijklmnopqrstuvwxyz0123456789 .,-+/*()";
static const char text_shift[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ)!@#$%^&*( ><_+?*[]";
static const char symbols[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";

void draw_lock_icon(SDL_Surface *screen, int x, int y, uint8_t mix)
{
    Uint16 ink = UI_COLOR_WARM_WHITE;
    SDL_Rect body = {(Sint16)x, (Sint16)(y + 4), 8, 6};
    SDL_Rect top = {(Sint16)(x + 2), (Sint16)y, 4, 1};
    SDL_Rect left = {(Sint16)(x + 1), (Sint16)(y + 1), 1, 4};
    SDL_Rect right = {(Sint16)(x + 6), (Sint16)(y + 1), 1, 4};
    SDL_Rect hole = {(Sint16)(x + 3), (Sint16)(y + 6), 2, 2};
    fill_rect_rgb565_mix(screen, &body, ink, mix);
    fill_rect_rgb565_mix(screen, &top, ink, mix);
    fill_rect_rgb565_mix(screen, &left, ink, mix);
    fill_rect_rgb565_mix(screen, &right, ink, mix);
    fill_rect_rgb565_mix(screen, &hole, UI_COLOR_GUNMETAL, mix);
}

static void draw_password_field(SDL_Surface *screen, const SDL_Rect *rect)
{
    const Uint16 shadow = UI_RGB565(12, 12, 12);
    const Uint16 highlight = UI_RGB565(94, 86, 76);
    const Uint16 top = UI_RGB565(40, 38, 34);
    const Uint16 bottom = UI_RGB565(62, 58, 52);
    /* Reverse the buttons' lighting: a dark upper/left bevel, light lower/
     * right lip, and a deeper inner shadow. Every edge uses the same mask. */
    for (int row = 0; row < rect->h; ++row) {
        int inset = soft_panel_inset_for_row(row, rect->h);
        SDL_Rect line = {rect->x + inset, rect->y + row, rect->w - 2 * inset, 1};
        fill_rect_rgb565(screen, &line, rgb565_lerp(shadow, highlight, row, rect->h - 1));
        if (row > 0 && row < rect->h - 1) {
            SDL_Rect edge = {line.x, line.y, 1, 1};
            fill_rect_rgb565(screen, &edge, shadow);
            edge.x = line.x + line.w - 1;
            fill_rect_rgb565(screen, &edge, highlight);
            ++line.x;
            line.w -= 2;
            Uint16 fill = row < 4
                ? rgb565_lerp(UI_RGB565(22, 20, 18), top, row - 1, 2)
                : rgb565_lerp(top, bottom, row - 1, rect->h - 3);
            fill_rect_rgb565(screen, &line, fill);
            if (row > 1 && row < rect->h - 2) {
                edge = (SDL_Rect){line.x, line.y, 1, 1};
                fill_rect_rgb565(screen, &edge, UI_RGB565(32, 30, 26));
            }
        }
    }
}

static bool inside(const PointerState *p, const SDL_Rect *r)
{
    return p->visible && p->x >= r->x && p->y >= r->y && p->x < r->x + r->w && p->y < r->y + r->h;
}

int unlock_movie_prompt(SDL_Surface *screen, const Fonts *fonts, const char *path,
                        UnlockBackdropDraw draw_backdrop, void *backdrop_context)
{
    if (movie_crypto_keys(path)) return 1;
    NveHeader header;
    const NveHeader *cached_header = NULL;
    if (g_picker_cache.valid) {
        for (size_t i = 0; i < g_picker_cache.count; ++i) {
            const MovieFile *entry = &g_picker_cache.files[i];
            if (entry->encrypted_header && !strcmp(entry->path, path)) {
                cached_header = entry->encrypted_header;
                break;
            }
        }
    }
    if (cached_header) header = *cached_header;
    else {
        uint8_t prefix[NVE_HEADER_BYTES];
        FILE *file = fopen(path, "rb");
        /* Direct launches may not have visited the picker yet. */
        if (!file) return 1;
        size_t read = fread(prefix, 1, sizeof(prefix), file);
        long bytes = -1;
        if (!fseek(file, 0, SEEK_END)) bytes = ftell(file);
        fclose(file);
        if (read < 4 || memcmp(prefix, "NVE1", 4)) return 1;
        if (read != sizeof(prefix) || bytes < 0 || (uint64_t)bytes > INT32_MAX ||
            !nve_header_parse(&header, prefix, (uint32_t)bytes)) {
            show_msgbox("Encrypted video", "The encrypted header is damaged or unsupported.");
            return 0;
        }
    }

    SDL_Surface *background = capture_screen_surface(screen);
    NveKdf *kdf = calloc(1, sizeof(*kdf));
    if (!background || !kdf) {
        if (background) SDL_FreeSurface(background);
        free(kdf);
        show_msgbox("Encrypted video", "Not enough memory to unlock the video.");
        return 0;
    }
    char password[NVE_PASSWORD_MAX + 1] = {0};
    size_t length = 0;
    bool previous[sizeof(text_keys) / sizeof(text_keys[0])];
    for (unsigned i = 0; i < sizeof(previous) / sizeof(previous[0]); ++i)
        previous[i] = isKeyPressed(*text_keys[i]);
    bool prev_enter = isKeyPressed(KEY_NSPIRE_ENTER), prev_esc = isKeyPressed(KEY_NSPIRE_ESC);
    bool prev_return = isKeyPressed(KEY_NSPIRE_RET), return_armed = false;
    bool prev_del = isKeyPressed(KEY_NSPIRE_DEL), prev_tab = isKeyPressed(KEY_NSPIRE_TAB);
    bool prev_on = on_key_pressed(), prev_scratch = isKeyPressed(KEY_NSPIRE_SCRATCHPAD);
    bool prev_up = false, prev_down = false, prev_left = false, prev_right = false;
    bool symbol_page = false, working = false, closing = false;
    bool enter_hover_target = false, pointer_hover_target = false, symbol_keyboard_focus = false;
    int result = 0, armed = -1, enter_target = -1, symbol_selection = 0;
    uint32_t started = monotonic_clock_now_ms(), closed_at = 0, last_draw = 0, delete_at = 0;
    const char *notice = "";
    PointerState pointer;
    pointer_init(&pointer); pointer_update(&pointer);
    UiTransition hover[3] = {{0}}, press[3] = {{0}};
    UiTransition symbol_expansion = {0};
    ScreenshotPreviewState preview = {0};
    char filename[96];
    copy_fitted_text(fonts->white, filename_from_path(path), filename, sizeof(filename), 264);

    while (1) {
        pointer_update(&pointer);
        uint32_t now = monotonic_clock_now_ms();
        night_mode_poll(now, false);
        clock_menu_sync_shortcut();
        bool esc = key_pressed_edge(KEY_NSPIRE_ESC, &prev_esc);
        bool on = on_key_pressed_edge(&prev_on);
        bool scratch = key_pressed_edge(KEY_NSPIRE_SCRATCHPAD, &prev_scratch);
        bool enter = key_pressed_edge(KEY_NSPIRE_ENTER, &prev_enter);
        bool return_edge = key_pressed_edge(KEY_NSPIRE_RET, &prev_return);
        bool tab = key_pressed_edge(KEY_NSPIRE_TAB, &prev_tab);
        bool del = key_pressed_edge(KEY_NSPIRE_DEL, &prev_del);
        if (g_display_power_state.off_fade_active || g_display_power_state.idle_restore_active)
            display_power_tick_transition(&g_display_power_state, now);
        bool activity = pointer.moved || pointer.down || esc || on || scratch || enter || tab || del || any_key_pressed();
        if (activity && (g_display_power_state.idle_dim_active ||
                        (g_display_power_state.off && g_display_power_state.off_from_idle)))
            display_power_restore_animated(&g_display_power_state, now);
        if (on) {
            if (g_display_power_state.off || g_display_power_state.off_fade_active)
                display_power_restore_animated(&g_display_power_state, now);
            else display_power_request_off(&g_display_power_state, true, now);
        }
        if (!closing && (esc || scratch)) {
            result = scratch ? PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT : 0;
            closing = true; closed_at = now;
            working = false;
            nve_wipe(password, sizeof(password)); nve_wipe(kdf, sizeof(*kdf)); length = 0;
        }
        if (g_display_power_state.off || g_display_power_state.off_fade_active) {
            if (closing) break;
            /* A release while the screen is off must not activate a stale
             * Enter/touchpad press when the screen wakes again. */
            if (armed >= 0 || enter_target >= 0 || return_armed) {
                armed = enter_target = -1;
                return_armed = false;
                for (unsigned i = 0; i < 3; ++i) ui_transition_init(&press[i], false);
            }
            if (display_power_should_suspend(&g_display_power_state, now)) {
                if (!player_standby(screen, NULL, path, false, &preview)) {
                    result = PLAY_MOVIE_RESULT_SUSPEND_EXIT;
                    break;
                }
                prev_on = true;
            }
            player_delay_ms(16);
            continue;
        }
        uint8_t mix = closing ? 255U - ui_ease_out_cubic(now - closed_at, 160U)
                              : ui_ease_out_cubic(now - started, 180U);
        uint8_t symbol_mix = ui_transition_update(&symbol_expansion, symbol_page, now, 220U);
        uint8_t symbol_content_mix = symbol_mix > 64U ? (symbol_mix - 64U) * 255U / 191U : 0;
        bool shown_symbols = symbol_mix > 0;
        bool symbols_ready = symbol_page && symbol_mix == 255U;
        bool shown_notice = *notice != '\0';
        int symbol_height = (74 * symbol_mix + 127) / 255;
        int panel_height = 128 + symbol_height + (shown_notice ? 14 : 0);
        SDL_Rect panel = {14, (Sint16)((SCREEN_H - panel_height) / 2), 292, (Uint16)panel_height};
        panel.y += ((255 - mix) * 12) / 255;
        SDL_Rect field = {24, (Sint16)(panel.y + 44), 272, 24};
        SDL_Rect buttons[3] = {{24, (Sint16)(panel.y + panel.h - 34), 76, 23},
                              {110, (Sint16)(panel.y + panel.h - 34), 88, 23},
                              {208, (Sint16)(panel.y + panel.h - 34), 88, 23}};
        int hot = -1;
        for (unsigned i = 0; i < 3; ++i) if (inside(&pointer, &buttons[i])) hot = (int)i + 100;
        SDL_Rect cells[sizeof(symbols) - 1];
        SDL_Rect symbol_clip = {24, (Sint16)(panel.y + 88), 272,
            (Uint16)(symbol_height > 2 ? symbol_height - 2 : 0)};
        if (shown_symbols) {
            for (unsigned i = 0; i < sizeof(symbols) - 1; ++i) {
                cells[i] = (SDL_Rect){(Sint16)(24 + i % 8U * 34),
                    (Sint16)(panel.y + 91 + i / 8U * 17 + (255 - symbol_content_mix) * 6 / 255), 32, 16};
                if (symbols_ready && inside(&pointer, &cells[i])) {
                    hot = (int)i;
                    if (pointer.moved) symbol_selection = (int)i;
                }
            }
        }
        int action = -1;
        if (!closing && !working) {
            bool shift = isKeyPressed(KEY_NSPIRE_SHIFT);
            for (unsigned i = 0; i < sizeof(previous) / sizeof(previous[0]); ++i) {
                bool down = isKeyPressed(*text_keys[i]);
                if (down && !previous[i]) {
                    if (length < NVE_PASSWORD_MAX) password[length++] = shift ? text_shift[i] : text_plain[i];
                    notice = ""; activity = true;
                }
                previous[i] = down;
            }
            if (del || (prev_del && (int32_t)(now - delete_at) >= 0)) {
                if (length) password[--length] = 0;
                delete_at = now + (del ? 350U : 65U); activity = true;
            }
            if (tab) { symbol_page = !symbol_page; symbol_keyboard_focus = false; }
            if (symbols_ready) {
                bool left = key_pressed_edge(KEY_NSPIRE_LEFT, &prev_left);
                bool right = key_pressed_edge(KEY_NSPIRE_RIGHT, &prev_right);
                bool up = key_pressed_edge(KEY_NSPIRE_UP, &prev_up);
                bool down = key_pressed_edge(KEY_NSPIRE_DOWN, &prev_down);
                if (left) symbol_selection = (symbol_selection + 31) % 32;
                if (right) symbol_selection = (symbol_selection + 1) % 32;
                if (up) symbol_selection = (symbol_selection + 24) % 32;
                if (down) symbol_selection = (symbol_selection + 8) % 32;
                if (left || right || up || down) symbol_keyboard_focus = true;
            }
        }
        if (pointer.moved) symbol_keyboard_focus = false;
        if (!closing) {
            /* Both inputs choose and latch the same target on press, then
             * hold its animation and activate on release. A hovered target
             * cancels if the pointer leaves it; the default stays latched. */
            int activation_target = hot >= 0 ? hot
                : symbols_ready && symbol_keyboard_focus ? symbol_selection : 102;
            if (working && activation_target != 100) activation_target = -1;
            if (return_edge && !working) {
                return_armed = true;
                armed = enter_target = -1;
            }
            if (return_armed && !prev_return) {
                action = 102;
                return_armed = false;
            }
            if (enter && !return_armed && armed < 0 && !pointer.down) {
                enter_hover_target = hot >= 0;
                enter_target = activation_target;
            }
            if (enter_target >= 0 && !prev_enter) {
                if (!enter_hover_target || hot == enter_target) action = enter_target;
                enter_target = -1;
            }
            if (pointer.press_edge && !return_armed && enter_target < 0) {
                pointer_hover_target = hot >= 0;
                armed = activation_target;
            }
            if (pointer.release_edge) {
                if (armed >= 0 && (!pointer_hover_target || armed == hot)) action = armed;
                armed = -1;
            }
            if (working && action != 100) action = -1;
            if (action == 100) {
                closing = true; closed_at = now; working = false;
                nve_wipe(password, sizeof(password)); length = 0;
                nve_wipe(kdf, sizeof(*kdf));
            }
            else if (action >= 0 && action < (int)sizeof(symbols) - 1) {
                if (length < NVE_PASSWORD_MAX) password[length++] = symbols[action];
                notice = ""; activity = true;
            }
            else if (action == 101) { symbol_page = !symbol_page; symbol_keyboard_focus = false; }
            else if (action == 102) {
                if (!length) notice = "Enter a password.";
                else {
                    working = nve_kdf_begin(kdf, &header, password, length);
                    nve_wipe(password, sizeof(password)); length = 0;
                    notice = "";
                }
            }
            if (action >= 100 && action <= 102)
                ui_transition_begin_press_release(&press[action - 100], now);
        }
        if (activity || working) display_power_note_activity(&g_display_power_state, now);
        if (working && !closing) {
            uint64_t begin = monotonic_clock_now_ticks();
            bool done;
            do { done = nve_kdf_step(kdf, 16U); }
            while (!done && monotonic_clock_now_ticks() - begin < 262U);
            if (done) {
                NveKeys keys;
                bool correct = nve_kdf_finish(kdf, &keys);
                if (correct && movie_crypto_install(path, &keys)) {
                    result = 1; closing = true; closed_at = monotonic_clock_now_ms();
                } else notice = "Wrong password or damaged file.";
                nve_wipe(&keys, sizeof(keys));
                working = false;
                for (unsigned i = 0; i < sizeof(previous) / sizeof(previous[0]); ++i)
                    previous[i] = isKeyPressed(*text_keys[i]);
            }
        }
        if (!working || (uint32_t)(now - last_draw) >= 16U) {
            last_draw = now;
            if (closing && draw_backdrop)
                draw_backdrop(screen, backdrop_context, monotonic_clock_now_ms() - closed_at);
            else SDL_BlitSurface(background, NULL, screen, NULL);
            draw_overlay_backdrop_dim(screen, mix);
            if (mix) {
                SDL_Surface *target;
                SDL_Rect old_clip;
                int dx, dy;
                SDL_Surface *layer = begin_faded_region_draw(screen, &panel, mix, &target, &old_clip, &dx, &dy);
                SDL_Rect draw_panel = offset_sdl_rect(&panel, dx, dy);
                SDL_Rect draw_field = offset_sdl_rect(&field, dx, dy);
                draw_soft_glass_panel_body_from_y(target, &draw_panel, draw_panel.y, UI_COLOR_GUNMETAL);
                int top_inset = soft_panel_inset_for_row(0, draw_panel.h);
                SDL_Rect accent = {draw_panel.x + top_inset, draw_panel.y, draw_panel.w - 2 * top_inset, 1};
                draw_vertical_gradient(target, &accent, UI_COLOR_ACCENT, UI_COLOR_ACCENT_DEEP);
                draw_lock_icon(target, 25 + dx, panel.y + 8 + dy, 255);
                draw_ui_label(target, fonts, 39 + dx, panel.y + 9 + dy, "Unlock video");
                draw_ui_label(target, fonts, 25 + dx, panel.y + 26 + dy, filename);
                draw_password_field(target, &draw_field);
                char masked[42];
                size_t visible = length < sizeof(masked) - 1 ? length : sizeof(masked) - 1;
                memset(masked, '*', visible); masked[visible] = 0;
                char progress[48];
                snprintf(progress, sizeof(progress), "Unlocking... %lu%%",
                         (unsigned long)((uint64_t)kdf->rounds * 100U / NVE_KDF_ROUNDS));
                bool show_progress = working || (closing && result == 1);
                draw_ui_label(target, fonts, field.x + 6 + dx, field.y + (show_progress ? 5 : 8) + dy,
                              closing && result == 1 ? "Unlocked" : working ? progress : length ? masked : "Password");
                if (show_progress) {
                    SDL_Rect track = {draw_field.x + 6, draw_field.y + draw_field.h - 5, draw_field.w - 12, 2};
                    fill_rect_rgb565(target, &track, ui_theme()->modal_panel);
                    uint32_t rounds = working ? kdf->rounds : NVE_KDF_ROUNDS;
                    track.w = (Uint16)((uint64_t)track.w * rounds / NVE_KDF_ROUNDS);
                    if (track.w) fill_rect_rgb565(target, &track, UI_COLOR_ACCENT);
                }
                draw_ui_label(target, fonts, 25 + dx, panel.y + 76 + dy, "Shift: uppercase   Tab: symbols");
                if (symbol_content_mix && symbol_clip.h) {
                    SDL_Surface *grid_target;
                    SDL_Rect grid_old_clip;
                    int grid_dx, grid_dy;
                    SDL_Rect grid = offset_sdl_rect(&symbol_clip, dx, dy);
                    /* Fade one clipped grid layer, so revealing rows cannot
                     * overlap the buttons while the window grows/shrinks. */
                    SDL_Surface *grid_layer = begin_faded_region_draw(target, &grid, symbol_content_mix,
                        &grid_target, &grid_old_clip, &grid_dx, &grid_dy);
                    for (unsigned i = 0; i < sizeof(symbols) - 1; ++i) {
                        char label[2] = {symbols[i], 0};
                        SDL_Rect cell = offset_sdl_rect(&cells[i], dx + grid_dx, dy + grid_dy);
                        bool held = !closing && ((pointer.down && armed == (int)i && (!pointer_hover_target || hot == (int)i)) ||
                            (prev_enter && enter_target == (int)i && (!enter_hover_target || hot == (int)i)));
                        draw_prompt_button(grid_target, fonts, &cell, label,
                            hot == (int)i ? 255 : 0, held ? 255 : 0);
                        if (symbol_keyboard_focus && (int)i == symbol_selection)
                            draw_soft_glass_panel_rim(grid_target, &cell, UI_COLOR_GUNMETAL, 255);
                    }
                    end_faded_region_draw(target, grid_layer, &grid, &grid_old_clip, symbol_content_mix);
                }
                if (shown_notice) draw_ui_label(target, fonts, 25 + dx, panel.y + panel.h - 48 + dy, notice);
                const char *labels[] = {"Cancel", symbol_page ? "Keyboard" : "Symbols", working ? "Unlocking" : "Unlock"};
                for (unsigned i = 0; i < 3; ++i) {
                    int id = (int)i + 100;
                    bool enabled = !working || id == 100;
                    bool held = !closing && enabled && ((pointer.down && armed == id && (!pointer_hover_target || hot == id)) ||
                        (prev_enter && enter_target == id && (!enter_hover_target || hot == id)) ||
                        (return_armed && prev_return && id == 102));
                    uint8_t h = ui_transition_update(&hover[i], enabled && hot == id, now, UI_HOVER_ANIM_MS);
                    uint8_t p = ui_transition_update_press_ex(&press[i], held,
                        now, UI_PRESS_ANIM_MS, PICKER_PRESS_RELEASE_ANIM_MS);
                    SDL_Rect button = offset_sdl_rect(&buttons[i], dx, dy);
                    draw_prompt_button(target, fonts, &button, labels[i], h, p);
                    if (id == 102 && !working && !(symbols_ready && symbol_keyboard_focus)) {
                        draw_soft_glass_panel_rim(target, &button, UI_COLOR_GUNMETAL, 255);
                    }
                }
                end_faded_region_draw(screen, layer, &panel, &old_clip, mix);
            }
            if (pointer.visible) draw_cursor(screen, pointer.x, pointer.y);
            present_screen(screen);
        }
        if (closing && !mix) break;
        if (!closing) display_power_tick_idle(&g_display_power_state, screen, monotonic_clock_now_ms(), true, true);
        if (!working) player_delay_ms(16);
    }
    nve_wipe(password, sizeof(password)); nve_wipe(kdf, sizeof(*kdf)); free(kdf);
    clear_screenshot_preview(&preview);
    if (!closing || !draw_backdrop) SDL_BlitSurface(background, NULL, screen, NULL);
    SDL_FreeSurface(background);
    if (result != 1) movie_crypto_clear();
    return result;
}
