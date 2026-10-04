#include "player_internal.h"

static PlayerMessage pending_message;

void queue_player_message(const char *title, const char *text)
{
    snprintf(pending_message.title, sizeof(pending_message.title), "%s",
        title && *title ? title : "Video player");
    snprintf(pending_message.text, sizeof(pending_message.text), "%s", text ? text : "");
}

bool player_message_pending(void)
{
    return pending_message.title[0] != '\0';
}

bool take_player_message(PlayerMessage *message)
{
    if (!message || !player_message_pending()) return false;
    *message = pending_message;
    memset(&pending_message, 0, sizeof(pending_message));
    return true;
}

enum { MESSAGE_LINES = 8, MESSAGE_LINE_BYTES = 64, MESSAGE_TEXT_WIDTH = 212 };

/* Fit by pixels, including paths without spaces. Text is wrapped once on
 * entry, never allocated or measured character-by-character in the UI loop. */
static unsigned message_wrap(nSDL_Font *font, const char *text,
    char lines[MESSAGE_LINES][MESSAGE_LINE_BYTES])
{
    unsigned count = 0;
    while (*text && count < MESSAGE_LINES) {
        while (*text == ' ' || *text == '\t' || *text == '\r') ++text;
        const char *start = text;
        size_t length = 0, last_space = 0;
        bool overflow = false;
        while (text[length] && text[length] != '\n') {
            if (length + 1U >= MESSAGE_LINE_BYTES) { overflow = true; break; }
            lines[count][length] = text[length];
            lines[count][length + 1U] = '\0';
            if (nSDL_GetStringWidth(font, lines[count]) > MESSAGE_TEXT_WIDTH) {
                lines[count][length] = '\0';
                overflow = true;
                break;
            }
            if (text[length] == ' ') last_space = length;
            ++length;
        }
        if (overflow && last_space) length = last_space;
        /* A single glyph fits the panel, but still guarantee forward progress. */
        if (!length && *text && *text != '\n') length = 1;
        memcpy(lines[count], start, length);
        lines[count][length] = '\0';
        text += length;
        while (*text == ' ' || *text == '\t' || *text == '\r') ++text;
        if (*text == '\n') ++text;
        ++count;
    }
    return count;
}

enum { MESSAGE_ENTER = 1U, MESSAGE_RETURN = 2U, MESSAGE_FIVE = 4U, MESSAGE_ESCAPE = 8U };
typedef struct {
    unsigned previous_keys, armed_key;
    bool pad_was_down, armed_pointer;
} MessageInput;

static unsigned message_keys(void)
{
    return (isKeyPressed(KEY_NSPIRE_ENTER) ? MESSAGE_ENTER : 0U) |
           (isKeyPressed(KEY_NSPIRE_RET) ? MESSAGE_RETURN : 0U) |
           (isKeyPressed(KEY_NSPIRE_5) ? MESSAGE_FIVE : 0U) |
           (isKeyPressed(KEY_NSPIRE_ESC) ? MESSAGE_ESCAPE : 0U);
}

static bool message_acknowledged(MessageInput *input, unsigned keys,
    const PointerState *pointer, bool pad_down, bool enabled)
{
    unsigned pressed = keys & ~input->previous_keys;
    bool center = pointer->press_edge && (!pad_down || !input->pad_was_down);
    bool acknowledge = false;
    if (!enabled) {
        input->armed_key = 0;
        input->armed_pointer = false;
    } else {
        if (!input->armed_key && !input->armed_pointer) {
            if (pressed) input->armed_key = pressed & (0U - pressed);
            else if (center) input->armed_pointer = true;
        }
        acknowledge = (input->armed_key && !(keys & input->armed_key)) ||
                      (input->armed_pointer && !pointer->down);
        if (acknowledge) {
            input->armed_key = 0;
            input->armed_pointer = false;
        }
    }
    input->previous_keys = keys;
    input->pad_was_down = pad_down;
    return acknowledge;
}

int show_player_message(SDL_Surface *screen, const Fonts *fonts, PointerState *pointer,
    const PlayerMessage *message, const char *directory,
    MessageBackdropDraw draw_backdrop, void *backdrop_context)
{
    char title[48], lines[MESSAGE_LINES][MESSAGE_LINE_BYTES];
    unsigned line_count = message_wrap(fonts->white, message->text, lines);
    copy_fitted_text(fonts->white, message->title, title, sizeof(title), MESSAGE_TEXT_WIDTH);
    int height = 120 + (line_count > 2 ? (int)(line_count - 2) * 13 : 0);
    MessageInput input = {message_keys(), 0, player_touchpad_button_down(), false};
    UiTransition hover = {0}, press = {0};
    ScreenshotPreviewState preview = {0};
    bool previous_on = on_key_pressed(), previous_scratch = isKeyPressed(KEY_NSPIRE_SCRATCHPAD);
    bool closing = false;
    int result = 0;
    uint32_t started = monotonic_clock_now_ms(), closed = 0;
    display_power_note_activity(&g_display_power_state, started);

    while (1) {
        if (input.armed_pointer && input.pad_was_down) pointer_update_captured(pointer);
        else pointer_update(pointer);
        bool pad_down = player_touchpad_button_down();
        unsigned keys = message_keys();
        uint32_t now = monotonic_clock_now_ms();
        bool was_off = g_display_power_state.off || g_display_power_state.off_fade_active;
        bool on = on_key_pressed_edge(&previous_on);
        bool scratch = key_pressed_edge(KEY_NSPIRE_SCRATCHPAD, &previous_scratch);
        night_mode_poll(now, false);
        clock_menu_sync_shortcut();
        if (pointer->moved || pointer->down || any_key_pressed()) {
            display_power_note_activity(&g_display_power_state, now);
            if (g_display_power_state.idle_dim_active ||
                (g_display_power_state.off && g_display_power_state.off_from_idle))
                display_power_restore_animated(&g_display_power_state, now);
        }
        if (on) {
            if (g_display_power_state.off || g_display_power_state.off_fade_active)
                display_power_restore_animated(&g_display_power_state, now);
            else display_power_request_off(&g_display_power_state, true, now);
        }
        if (g_display_power_state.off_fade_active || g_display_power_state.idle_restore_active)
            display_power_tick_transition(&g_display_power_state, now);
        bool off = g_display_power_state.off || g_display_power_state.off_fade_active;
        bool acknowledge = message_acknowledged(&input, keys, pointer, pad_down,
            !closing && !was_off && !off);
        if (was_off || off) ui_transition_init(&press, false);
        if (!closing && (acknowledge || scratch)) {
            if (acknowledge) ui_transition_begin_press_release(&press, now);
            result = scratch ? PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT : 0;
            closing = true;
            closed = now;
        }
        if (off) {
            if (closing) break;
            if (display_power_should_suspend(&g_display_power_state, now)) {
                if (!player_standby(screen, NULL, directory, true, &preview)) {
                    result = PLAY_MOVIE_RESULT_SUSPEND_EXIT;
                    break;
                }
                previous_on = true;
                pointer->tracking = false;
                input = (MessageInput){message_keys(), 0, player_touchpad_button_down(), false};
            }
            player_delay_ms(16);
            continue;
        }

        uint8_t mix = closing ? 255U - ui_ease_out_cubic(now - closed, 160U)
                              : ui_ease_out_cubic(now - started, 180U);
        SDL_Rect panel = {44, (SCREEN_H - height) / 2 + (255 - mix) * 12 / 255, 232, height};
        SDL_Rect button = {124, panel.y + panel.h - 35, 72, 23};
        bool hot = pointer->visible && pointer->x >= button.x && pointer->x < button.x + button.w &&
                   pointer->y >= button.y && pointer->y < button.y + button.h;
        uint8_t h = ui_transition_update(&hover, !closing && hot, now, UI_HOVER_ANIM_MS);
        uint8_t p = ui_transition_update_press_ex(&press,
            !closing && (input.armed_key || input.armed_pointer), now,
            UI_PRESS_ANIM_MS, PICKER_PRESS_RELEASE_ANIM_MS);

        /* The caller redraws its scene; no full-screen snapshot is needed,
         * including when the error itself was an allocation failure. */
        if (draw_backdrop) draw_backdrop(screen, backdrop_context, closing ? now - closed : 0U);
        else SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
        draw_overlay_backdrop_dim(screen, mix);
        if (mix) {
            SDL_Surface *target;
            SDL_Rect old_clip;
            int dx, dy;
            SDL_Surface *layer = begin_faded_region_draw(screen, &panel, mix, &target, &old_clip, &dx, &dy);
            SDL_Rect box = offset_sdl_rect(&panel, dx, dy);
            draw_soft_glass_panel_body_from_y(target, &box, box.y, UI_COLOR_GUNMETAL);
            int inset = soft_panel_inset_for_row(0, box.h);
            SDL_Rect accent = {box.x + inset, box.y, box.w - 2 * inset, 1};
            fill_rect_rgb565(target, &accent, UI_COLOR_ACCENT);
            draw_ui_label(target, fonts, box.x + (box.w - nSDL_GetStringWidth(fonts->white, title)) / 2,
                box.y + 13, title);
            for (unsigned i = 0; i < line_count; ++i)
                draw_ui_label(target, fonts,
                    box.x + (box.w - nSDL_GetStringWidth(fonts->white, lines[i])) / 2,
                    box.y + 39 + (int)i * 13, lines[i]);
            SDL_Rect action = offset_sdl_rect(&button, dx, dy);
            draw_prompt_button(target, fonts, &action, "OK", h, p);
            draw_soft_glass_panel_rim(target, &action, UI_COLOR_GUNMETAL, 255);
            end_faded_region_draw(screen, layer, &panel, &old_clip, mix);
        }
        if (pointer->visible) draw_cursor(screen, pointer->x, pointer->y);
        present_screen(screen);
        if (closing && !mix) break;
        display_power_tick_idle(&g_display_power_state, screen, monotonic_clock_now_ms(), true, true);
        player_delay_ms(16);
    }
    clear_screenshot_preview(&preview);
    pointer->press_edge = pointer->release_edge = false;
    return result;
}
