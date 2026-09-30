#include "player_internal.h"
#include "performance_clock.h"
#include "app_task_io.h"
#include "raw_player_io.h"

static bool previous_o, footer_armed, footer_key_armed, footer_key_down;
static bool footer_input_captured, footer_canceled, dialog_active, queued_shortcut;
enum { CLOCK_FOOTER_CENTER_X = 196, CLOCK_FOOTER_Y = SCREEN_H - 19,
       CLOCK_FOOTER_W = 64, CLOCK_FOOTER_H = 13 };
static Uint16 footer_backdrop[CLOCK_FOOTER_W * CLOCK_FOOTER_H];
static bool footer_backdrop_valid, footer_drawn_visible;
static SDL_Rect footer_backdrop_rect, footer_drawn_rect;
static SDL_Rect clock_footer_rect(const Fonts *fonts, int offset_y)
{
    char label[24];
    snprintf(label, sizeof(label), "%u MHz", performance_clock_current_mhz());
    int width = nSDL_GetStringWidth(fonts->white, label) + 8;
    if (width > CLOCK_FOOTER_W) width = CLOCK_FOOTER_W;
    return (SDL_Rect){CLOCK_FOOTER_CENTER_X - width / 2, CLOCK_FOOTER_Y + offset_y, width, CLOCK_FOOTER_H};
}
bool clock_menu_consumed_input(void) { return footer_input_captured; }
void clock_menu_sync_shortcut(void) { previous_o = isKeyPressed(KEY_NSPIRE_O); }
void clock_menu_queue_shortcut(void)
{
    if (key_pressed_edge(KEY_NSPIRE_O, &previous_o)) queued_shortcut = true;
}
static UiTransition footer_hover, footer_press;
static bool hit(const PointerState *p, const SDL_Rect *r)
{
    return p->visible && p->x >= r->x && p->y >= r->y && p->x < r->x + r->w && p->y < r->y + r->h;
}

typedef struct { unsigned mhz; bool test, ok, keep_after_exit; uint32_t before, after; } ClockAction;
static void clock_action_native(void *context)
{
    ClockAction *a = context;
    a->ok = a->test ? performance_clock_test(a->mhz, &a->before, &a->after)
                    : performance_clock_choose(a->mhz, a->keep_after_exit);
}
static void clock_action(Movie *movie, ClockAction *a)
{
    /* Stop controller activity and return borrowed SRAM before changing RAM
     * timings. The decoder's buffers and frame queue stay allocated. */
    screenshot_writer_drain();
    app_task_io_drain();
    raw_player_before_native();
    if (!sram_with_native_mapping(clock_action_native, a)) a->ok = false;
    raw_player_native_released();
    movie_async_resume(movie);
}

static void clock_dialog(SDL_Surface *screen, const Fonts *fonts, Movie *movie,
                         PointerState *pointer, const char *path)
{
    SDL_Surface *background = capture_screen_surface(screen);
    if (!background) return;
    remove_captured_cursor(screen, background);
    unsigned choice = 0, count = performance_clock_option_count();
    unsigned applied_mhz = performance_clock_current_mhz();
    for (unsigned i = 0; i < count; ++i)
        if (performance_clock_option(i) == (performance_clock_selection() ? performance_clock_selection() : performance_clock_default_mhz())) choice = i;
    bool pad_was_down = player_touchpad_button_down();
    bool prev_enter = isKeyPressed(KEY_NSPIRE_ENTER), prev_ret = isKeyPressed(KEY_NSPIRE_RET);
    bool prev_esc = isKeyPressed(KEY_NSPIRE_ESC), prev_o = isKeyPressed(KEY_NSPIRE_O);
    bool prev_4 = isKeyPressed(KEY_NSPIRE_4), prev_6 = isKeyPressed(KEY_NSPIRE_6);
    bool prev_left = isKeyPressed(KEY_NSPIRE_LEFT), prev_right = isKeyPressed(KEY_NSPIRE_RIGHT);
    bool prev_on = on_key_pressed(), dragging = false, track_pressed = false, slider_key = false;
    bool closing = false, key_press = false, forced_apply = false;
    bool keep_after_exit = performance_clock_keep_after_exit();
    int armed = -1, pending = -1, grab_offset = 0;
    uint32_t started = monotonic_clock_now_ms(), closed = 0, action_at = 0, repeat_at = 0;
    uint32_t thumb_started = started, thumb_duration = 130U;
    int thumb_from = ((int)choice * 240 + (int)(count - 1) / 2) / (int)(count - 1), thumb_target = thumb_from, thumb_x = thumb_from;
    UiTransition hover[4] = {{0}}, press[4] = {{0}}, slider_hover = {0}, slider_press = {0}, check = {0};
    ui_transition_init(&check, keep_after_exit);
    char notice[64] = "Test compares with default speed.";
    ScreenshotPreviewState preview = {0};
    while (1) {
        if (((dragging || track_pressed) && !slider_key) ||
            (pad_was_down && armed >= 0 && !key_press))
            pointer_update_captured(pointer);
        else pointer_update(pointer);
        bool pad_down = player_touchpad_button_down();
        bool pad_press = pad_down && !pad_was_down;
        bool click_press = pointer->press_edge && (!pad_down || pad_press);
        pad_was_down = pad_down;
        uint32_t now = monotonic_clock_now_ms();
        bool enter = key_pressed_edge(KEY_NSPIRE_ENTER, &prev_enter);
        bool ret = key_pressed_edge(KEY_NSPIRE_RET, &prev_ret);
        bool esc = key_pressed_edge(KEY_NSPIRE_ESC, &prev_esc);
        bool o = key_pressed_edge(KEY_NSPIRE_O, &prev_o);
        bool left = key_pressed_edge(KEY_NSPIRE_LEFT, &prev_left);
        bool right = key_pressed_edge(KEY_NSPIRE_RIGHT, &prev_right);
        bool key4 = key_pressed_edge(KEY_NSPIRE_4, &prev_4);
        bool key6 = key_pressed_edge(KEY_NSPIRE_6, &prev_6);
        left = left || key4; right = right || key6;
        bool left_down = prev_left || prev_4, right_down = prev_right || prev_6;
        bool on = on_key_pressed_edge(&prev_on);
        night_mode_poll(now, !g_display_power_state.off);
        bool activity = pointer->moved || pointer->down || any_key_pressed();
        if (activity) {
            display_power_note_activity(&g_display_power_state, now);
            if (g_display_power_state.idle_dim_active || (g_display_power_state.off && g_display_power_state.off_from_idle))
                display_power_restore_animated(&g_display_power_state, now);
        }
        if (on) {
            if (g_display_power_state.off || g_display_power_state.off_fade_active)
                display_power_restore_animated(&g_display_power_state, now);
            else display_power_request_off(&g_display_power_state, true, now);
        }
        if (g_display_power_state.off_fade_active || g_display_power_state.idle_restore_active)
            display_power_tick_transition(&g_display_power_state, now);
        if ((esc || o) && !closing && pending < 0) { closing = true; closed = now; }
        if (g_display_power_state.off || g_display_power_state.off_fade_active) {
            armed = -1; dragging = track_pressed = false; pending = -1;
            if (closing) break;
            if (display_power_should_suspend(&g_display_power_state, now)) {
                /* A clock dialog must not discard the caller's navigation
                 * state. Native standby can return here without closing it. */
                if (!player_standby(screen, movie, path, !movie, &preview))
                    display_power_note_activity(&g_display_power_state, now);
                applied_mhz = performance_clock_current_mhz();
                prev_on = true;
            }
            player_delay_ms(16);
            continue;
        }
        uint8_t mix = closing ? 255U - ui_ease_out_cubic(now - closed, 160U)
                              : ui_ease_out_cubic(now - started, 180U);
        SDL_Rect panel = {22, 26 + (255 - mix) * 12 / 255, 276, 188};
        SDL_Rect track = {40, panel.y + 70, 240, 18};
        SDL_Rect buttons[3] = {{36, panel.y + 153, 76, 23}, {122, panel.y + 153, 76, 23}, {208, panel.y + 153, 76, 23}};
        SDL_Rect checkbox_row = {36, panel.y + 128, 248, 15};
        int hot = -1;
        for (unsigned i = 0; i < 3; ++i) if (hit(pointer, &buttons[i])) hot = i;
        if (hit(pointer, &checkbox_row)) hot = 3;
        SDL_Rect knob_hit = {track.x + thumb_x - 5, track.y, 11, 19};
        bool knob_hot = hit(pointer, &knob_hit);
        /* The whole band selects the slider, including space beyond its
         * endpoints and below the rail. Only the rest defaults to Apply. */
        SDL_Rect slider_hit = {panel.x, track.y - 13, panel.w, 47};
        bool slider_hot = hit(pointer, &slider_hit);
        bool select_press = (enter || click_press) && armed < 0 && !dragging && !track_pressed;
        bool grab = !closing && pending < 0 && select_press && knob_hot;
        /* A press targets the handle that was actually drawn, including
         * during a keyboard transition; do not move it before hit-testing. */
        if (!dragging && !grab)
            thumb_x = thumb_from + (thumb_target - thumb_from) * ui_ease_smoothstep(now - thumb_started, thumb_duration) / 255;
        bool drag_gesture = false;
        int drag_position = thumb_x, drag_previous = thumb_x;
        unsigned next = choice;
        int action = -1;
        if (!closing && pending < 0) {
            /* A rail/mark click selects a slot; only grabbing the drawn knob
             * starts a drag. Neither gesture may fall through to Apply. */
            if (select_press && slider_hot && !knob_hot) {
                track_pressed = true;
                slider_key = enter;
                int position = clamp_int(pointer->x - track.x, 0, 240);
                next = (unsigned)(position * (int)(count - 1) + 120) / 240;
            }
            bool track_gesture = track_pressed;
            if (track_pressed && !(slider_key ? prev_enter : pointer->down)) {
                track_pressed = false;
                ui_transition_begin_press_release(&slider_press, now);
            }
            if (grab) {
                dragging = true;
                slider_key = enter;
                grab_offset = pointer->x - track.x - thumb_x;
            }
            drag_gesture = dragging;
            if (dragging) {
                /* Pointer motion chooses an actual speed slot. A short
                 * retargetable animation follows it without the old long lag. */
                drag_position = clamp_int(pointer->x - track.x - grab_offset, 0, 240);
                next = (unsigned)(drag_position * (int)(count - 1) + 120) / 240;
                int target = ((int)next * 240 + (int)(count - 1) / 2) / (int)(count - 1);
                if (grab || target != thumb_target) {
                    thumb_from = thumb_x;
                    thumb_target = target;
                    thumb_duration = 48U;
                    thumb_started = now - 16U; /* Show a response on this frame. */
                }
                if (!(slider_key ? prev_enter : pointer->down)) {
                    dragging = false;
                    thumb_from = thumb_x;
                    thumb_target = target;
                    thumb_started = now;
                    thumb_duration = 48U;
                    ui_transition_begin_press_release(&slider_press, now);
                }
            } else if (!track_gesture && armed < 0) {
                if (left || right) repeat_at = now + 350U;
                bool repeat = (left_down || right_down) && (int32_t)(now - repeat_at) >= 0;
                if (repeat) repeat_at = now + 100U;
                if ((left || (repeat && left_down)) && next) --next;
                if ((right || (repeat && right_down)) && next + 1 < count) ++next;
            }
            if ((ret || (select_press && !slider_hot)) && armed < 0 && !drag_gesture && !track_gesture) {
                armed = ret ? 2 : hot >= 0 ? hot : 2;
                key_press = ret || enter; forced_apply = ret || hot < 0;
            }
            if (armed >= 0 && ((key_press && !prev_enter && !prev_ret) || (!key_press && pointer->release_edge))) {
                if (forced_apply || hot == armed) action = armed;
                armed = -1;
            }
            if (next != choice) {
                choice = next;
                if (!drag_gesture) {
                    thumb_from = thumb_x;
                    thumb_target = ((int)choice * 240 + (int)(count - 1) / 2) / (int)(count - 1);
                    thumb_started = now;
                    thumb_duration = 130U;
                }
                snprintf(notice, sizeof(notice), "Test compares with default speed.");
            }
            if (action >= 0) {
                ui_transition_begin_press_release(&press[action], now);
                if (!action) { closing = true; closed = now; }
                else if (action == 3) keep_after_exit = !keep_after_exit;
                else { pending = action; action_at = now + 120U;
                    snprintf(notice, sizeof(notice), action == 1 ? "Testing default and selected speed..." : "Applying clock settings..."); }
            }
        }
        thumb_x = thumb_from + (thumb_target - thumb_from) * ui_ease_smoothstep(now - thumb_started, thumb_duration) / 255;
        if (dragging) {
            /* A nearest-step target may lie beyond the cursor. Keep held
             * motion between the last drawn position and the grab point;
             * finish the small snap only after release. */
            int lower = drag_previous < drag_position ? drag_previous : drag_position;
            int upper = drag_previous > drag_position ? drag_previous : drag_position;
            thumb_x = clamp_int(thumb_x, lower, upper);
        }
        SDL_BlitSurface(background, NULL, screen, NULL);
        if (!movie) {
            ui_transition_update(&footer_hover, false, now, UI_HOVER_ANIM_MS);
            clock_menu_draw_footer(screen, fonts, 0, 255);
        }
        draw_overlay_backdrop_dim(screen, mix);
        SDL_Surface *target; SDL_Rect old_clip; int dx, dy;
        SDL_Surface *layer = begin_faded_region_draw(screen, &panel, mix, &target, &old_clip, &dx, &dy);
        SDL_Rect box = offset_sdl_rect(&panel, dx, dy);
        draw_soft_glass_panel_body_from_y(target, &box, box.y, UI_COLOR_GUNMETAL);
        int top_inset = soft_panel_inset_for_row(0, box.h);
        SDL_Rect accent = {box.x + top_inset, box.y, box.w - 2 * top_inset, 1};
        fill_rect_rgb565(target, &accent, UI_COLOR_ACCENT);
        draw_ui_label(target, fonts, 36 + dx, panel.y + 10 + dy, "Processor speed");
        char label[64];
        unsigned mhz = performance_clock_option(choice);
        snprintf(label, sizeof(label), "%u MHz", mhz);
        draw_ui_label(target, fonts, (SCREEN_W - nSDL_GetStringWidth(fonts->white, label)) / 2 + dx, panel.y + 36 + dy, label);
        snprintf(label, sizeof(label), "%u", performance_clock_option(0));
        draw_ui_label(target, fonts, track.x + dx, panel.y + 59 + dy, label);
        snprintf(label, sizeof(label), "%u MHz", performance_clock_option(count - 1));
        draw_ui_label(target, fonts, track.x + track.w - nSDL_GetStringWidth(fonts->white, label) + dx, panel.y + 59 + dy, label);
        /* Each mark is a selectable multiplier, including both endpoints. */
        for (unsigned i = 0; i < count; ++i) {
            int position = (i * 240U + (count - 1U) / 2U) / (count - 1U);
            SDL_Rect tick = {track.x + position + dx, track.y + dy, 1, 4};
            fill_rect_rgb565(target, &tick,
                performance_clock_option(i) == applied_mhz ? UI_COLOR_ACCENT :
                blend_rgb565(UI_COLOR_GUNMETAL, UI_COLOR_WARM_WHITE, 144));
        }
        SDL_Rect line = {track.x + dx, track.y + 6 + dy, track.w + 1, 7};
        draw_progress_track(target, &line, &box);
        line.w = thumb_x + 1;
        draw_vertical_gradient(target, &line, ui_theme()->progress_fill_top, ui_theme()->progress_fill_bottom);
        SDL_Rect glow = {line.x, line.y + 1, line.w, 1};
        SDL_Rect cap = {line.x + line.w - 1, line.y, 1, line.h};
        fill_rect_rgb565(target, &glow, ui_theme()->progress_fill_glow);
        fill_rect_rgb565(target, &cap, ui_theme()->progress_fill_cap);
        SDL_Rect thumb = {track.x + thumb_x - 5 + dx, track.y + dy, 11, 19};
        SDL_Rect visible_knob = {track.x + thumb_x - 5, track.y, 11, 19};
        uint8_t slider_mix = ui_transition_update(&slider_hover,
            !closing && pending < 0 && (hit(pointer, &visible_knob) || dragging), now, UI_HOVER_ANIM_MS);
        uint8_t slider_press_mix = ui_transition_update_press_ex(&slider_press,
            !closing && (dragging || track_pressed) && (slider_key ? prev_enter : pointer->down),
            now, UI_PRESS_ANIM_MS, PICKER_PRESS_RELEASE_ANIM_MS);
        draw_prompt_button(target, fonts, &thumb, "", slider_mix, slider_press_mix);
        /* Keep the handle visible against the dark rail even when idle. */
        draw_soft_glass_panel_rim(target, &thumb, UI_COLOR_GUNMETAL, 255);
        draw_ui_label(target, fonts, 36 + dx, panel.y + 104 + dy, notice);
        bool check_held = !closing && armed == 3 && hot == 3;
        uint8_t check_hover = ui_transition_update(&hover[3], hot == 3 && pending < 0, now, UI_HOVER_ANIM_MS);
        uint8_t check_press = ui_transition_update_press_ex(&press[3], check_held, now,
            UI_PRESS_ANIM_MS, PICKER_PRESS_RELEASE_ANIM_MS);
        uint8_t check_mix = ui_transition_update(&check, keep_after_exit, now, UI_HOVER_ANIM_MS);
        SDL_Rect check_box = {checkbox_row.x + dx, checkbox_row.y + 1 + dy, 13, 13};
        draw_prompt_button(target, fonts, &check_box, "", check_hover, check_press);
        draw_soft_glass_panel_rim(target, &check_box, UI_COLOR_GUNMETAL, 255);
        if (check_mix) {
            Uint16 color = blend_rgb565(UI_COLOR_GUNMETAL, UI_COLOR_ACCENT, check_mix);
            for (int i = 0; i < 8; ++i) {
                SDL_Rect dot = {check_box.x + 2 + i + pressed_control_offset_x(check_press),
                    check_box.y + (i < 3 ? 6 + i : 10 - i) + pressed_control_offset_y(check_press), 2, 2};
                fill_rect_rgb565(target, &dot, color);
            }
        }
        draw_ui_label(target, fonts, checkbox_row.x + 19 + dx + pressed_control_offset_x(check_press),
            checkbox_row.y + (checkbox_row.h - NSP_FONT_HEIGHT) / 2 + dy + pressed_control_offset_y(check_press),
            "Keep speed after exit");
        const char *names[] = {"Close", "Test", "Apply"};
        for (unsigned i = 0; i < 3; ++i) {
            bool held = !closing && armed == (int)i && (forced_apply || hot == (int)i);
            uint8_t h = ui_transition_update(&hover[i], hot == (int)i && pending < 0, now, UI_HOVER_ANIM_MS);
            uint8_t p = ui_transition_update_press_ex(&press[i], held, now, UI_PRESS_ANIM_MS, PICKER_PRESS_RELEASE_ANIM_MS);
            SDL_Rect button = offset_sdl_rect(&buttons[i], dx, dy);
            draw_prompt_button(target, fonts, &button, names[i], h, p);
            if (i == 2) draw_soft_glass_panel_rim(target, &button, UI_COLOR_GUNMETAL, 255);
        }
        end_faded_region_draw(screen, layer, &panel, &old_clip, mix);
        if (pointer->visible) draw_cursor(screen, pointer->x, pointer->y);
        present_screen(screen);
        if (pending >= 0 && (int32_t)(now - action_at) >= 0) {
            ClockAction a = {performance_clock_option(choice), pending == 1, false, keep_after_exit, 0, 0};
            clock_action(movie, &a);
            applied_mhz = performance_clock_current_mhz();
            if (!a.ok) snprintf(notice, sizeof(notice), "Clock change unavailable on this setup.");
            else if (a.test) {
                unsigned ratio = (uint64_t)a.before * 100U / a.after;
                snprintf(notice, sizeof(notice), "Test: %u.%02ux (speed restored)", ratio / 100U, ratio % 100U);
            } else { closing = true; closed = monotonic_clock_now_ms(); }
            pending = -1;
        }
        if (closing && !mix) break;
        display_power_tick_idle(&g_display_power_state, screen, monotonic_clock_now_ms(), true, true);
        player_delay_ms(16);
    }
    SDL_BlitSurface(background, NULL, screen, NULL);
    SDL_FreeSurface(background);
    clear_screenshot_preview(&preview);
    pointer->press_edge = pointer->release_edge = false;
}

bool clock_menu_poll(SDL_Surface *screen, const Fonts *fonts, Movie *movie, PointerState *pointer, bool footer, const char *path)
{
    bool edge = key_pressed_edge(KEY_NSPIRE_O, &previous_o);
    edge = edge || queued_shortcut;
    bool enabled = footer && !g_display_power_state.off && !g_display_power_state.off_fade_active;
    bool hot = enabled && footer_drawn_visible && hit(pointer, &footer_drawn_rect);
    uint32_t now = monotonic_clock_now_ms();
    bool key_down = isKeyPressed(KEY_NSPIRE_ENTER) ||
        (!isKeyPressed(KEY_NSPIRE_CTRL) && isKeyPressed(KEY_NSPIRE_5));
    bool key_edge = key_down && !footer_key_down;
    footer_key_down = key_down;
    if (enabled && pointer->press_edge && !footer_key_armed) {
        footer_armed = hot;
        footer_canceled = false;
    }
    if (enabled && key_edge && hot && !pointer->down && !footer_armed) {
        footer_key_armed = true;
        footer_canceled = false;
    }
    /* Capture from press through release, even after dragging off the button.
     * The picker must not treat any part of this gesture as a selected-row click. */
    footer_input_captured = footer && (footer_armed || footer_key_armed);
    if (footer_input_captured && (!enabled || isKeyPressed(KEY_NSPIRE_ESC))) footer_canceled = true;
    bool released = (footer_armed && pointer->release_edge) || (footer_key_armed && !key_down);
    bool click = released && hot && !footer_canceled;
    if (!pointer->down) footer_armed = false;
    if (!key_down) footer_key_armed = false;
    ui_transition_update(&footer_hover, hot, now, UI_HOVER_ANIM_MS);
    ui_transition_update_press_ex(&footer_press, !footer_canceled && (footer_armed || footer_key_armed) && hot,
        now, UI_PRESS_ANIM_MS, PICKER_PRESS_RELEASE_ANIM_MS);
    if ((!edge && !click) || g_display_power_state.off || g_display_power_state.off_fade_active) return false;
    queued_shortcut = false;
    if (click) ui_transition_begin_press_release(&footer_press, now);
    if (movie) playback_capture_tick(movie, monotonic_clock_now_ticks(), true);
    dialog_active = true;
    clock_dialog(screen, fonts, movie, pointer, path);
    dialog_active = false;
    previous_o = isKeyPressed(KEY_NSPIRE_O);
    footer_armed = footer_key_armed = false;
    footer_input_captured = false;
    return true;
}

void clock_menu_draw_footer(SDL_Surface *screen, const Fonts *fonts, int offset_y, uint8_t mix)
{
    SDL_Rect box = clock_footer_rect(fonts, offset_y);
    if (!dialog_active) { footer_drawn_rect = box; footer_drawn_visible = mix > 54; }
    if (screen->format->BitsPerPixel == 16 && box.y >= 0 && box.y + box.h <= screen->h) {
        if (dialog_active && footer_backdrop_valid) {
            SDL_Rect saved = footer_backdrop_rect;
            for (int y = 0; y < saved.h; ++y)
                memcpy((uint8_t *)screen->pixels + (saved.y + y) * screen->pitch + saved.x * 2,
                       footer_backdrop + y * CLOCK_FOOTER_W, saved.w * 2);
        } else if (!dialog_active) {
            for (int y = 0; y < box.h; ++y)
                memcpy(footer_backdrop + y * CLOCK_FOOTER_W,
                       (uint8_t *)screen->pixels + (box.y + y) * screen->pitch + box.x * 2, box.w * 2);
            footer_backdrop_rect = box;
            footer_backdrop_valid = true;
        }
    }
    uint8_t hover = ui_transition_value(&footer_hover, monotonic_clock_now_ms(), UI_HOVER_ANIM_MS);
    uint8_t press = ui_transition_value_with_ease(&footer_press, monotonic_clock_now_ms(),
        footer_press.target_active ? UI_PRESS_ANIM_MS : PICKER_PRESS_RELEASE_ANIM_MS, ui_ease_smoothstep);
    char text[24];
    snprintf(text, sizeof(text), "%u MHz", performance_clock_current_mhz());
    uint8_t surface_mix = (uint16_t)max_u8(hover, press) * mix / 255U;
    if (surface_mix) {
        SDL_Surface *target;
        SDL_Rect old_clip;
        int dx, dy;
        SDL_Surface *layer = begin_faded_region_draw(screen, &box, surface_mix, &target, &old_clip, &dx, &dy);
        SDL_Rect button = offset_sdl_rect(&box, dx, dy);
        /* Use the standard buttons' hover fill, press shading, reflection
         * and outline. The surrounding fade preserves the idle text-only style. */
        draw_prompt_button(target, fonts, &button, text, hover, press);
        end_faded_region_draw(screen, layer, &box, &old_clip, surface_mix);
    }
    if (mix > 54) {
        draw_ui_label(screen, fonts, box.x + (box.w - nSDL_GetStringWidth(fonts->white, text)) / 2 + pressed_control_offset_x(press),
            box.y + (box.h - NSP_FONT_HEIGHT) / 2 + pressed_control_offset_y(press), text);
    }
}
