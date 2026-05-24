#include "player_internal.h"

const char *scale_mode_text(ScaleMode scale_mode)
{
    if (scale_mode == SCALE_FILL) {
        return "FILL";
    }
    if (scale_mode == SCALE_STRETCH) {
        return "STRETCH";
    }
    if (scale_mode == SCALE_NATIVE) {
        return "1:1";
    }
    return "FIT";
}

const char *playback_mode_text(PlaybackMode playback_mode)
{
    if (playback_mode == PLAYBACK_MODE_REPEAT) {
        return "REPLAY";
    }
    if (playback_mode == PLAYBACK_MODE_AUTO_NEXT) {
        return "AUTO NEXT";
    }
    return "PLAY ONCE";
}

int top_overlay_y_for_rect(const SDL_Rect *video_rect, int overlay_h)
{
    if (video_rect && video_rect->y > overlay_h) {
        return (video_rect->y - overlay_h) / 2;
    }
    return 8;
}

uint8_t ui_ease_out_cubic(uint32_t elapsed_ms, uint32_t duration_ms)
{
    uint32_t t;
    uint32_t inverse;
    uint32_t inverse_squared;

    if (duration_ms == 0 || elapsed_ms >= duration_ms) {
        return 255;
    }
    t = (elapsed_ms * 255U) / duration_ms;
    inverse = 255U - t;
    inverse_squared = (inverse * inverse + 127U) / 255U;
    return (uint8_t) (255U - ((inverse_squared * inverse + 127U) / 255U));
}

uint8_t ui_ease_in_cubic(uint32_t elapsed_ms, uint32_t duration_ms)
{
    uint32_t t;
    uint32_t t_squared;

    if (duration_ms == 0 || elapsed_ms >= duration_ms) {
        return 255;
    }
    t = (elapsed_ms * 255U) / duration_ms;
    t_squared = (t * t + 127U) / 255U;
    return (uint8_t) ((t_squared * t + 127U) / 255U);
}

uint8_t ui_ease_smoothstep(uint32_t elapsed_ms, uint32_t duration_ms)
{
    uint32_t t;
    uint32_t t_squared;

    if (duration_ms == 0 || elapsed_ms >= duration_ms) {
        return 255;
    }
    t = (elapsed_ms * 255U) / duration_ms;
    t_squared = (t * t + 127U) / 255U;
    return (uint8_t) ((t_squared * ((3U * 255U) - (2U * t)) + 127U) / 255U);
}

bool rects_equal(const SDL_Rect *a, const SDL_Rect *b)
{
    return a && b &&
        a->x == b->x &&
        a->y == b->y &&
        a->w == b->w &&
        a->h == b->h;
}

SDL_Rect mix_sdl_rects(const SDL_Rect *from, const SDL_Rect *to, uint8_t mix)
{
    SDL_Rect rect;

    rect.x = ui_mix_sint16(from->x, to->x, mix);
    rect.y = ui_mix_sint16(from->y, to->y, mix);
    rect.w = ui_mix_uint16(from->w, to->w, mix);
    rect.h = ui_mix_uint16(from->h, to->h, mix);
    return rect;
}

SDL_Rect offset_sdl_rect(const SDL_Rect *rect, int dx, int dy)
{
    SDL_Rect shifted = *rect;

    shifted.x = (Sint16) (shifted.x + dx);
    shifted.y = (Sint16) (shifted.y + dy);
    return shifted;
}

uint8_t staggered_content_mix(uint32_t elapsed_ms, uint32_t start_ms, uint32_t stagger_ms, uint32_t index, uint32_t duration_ms)
{
    uint32_t item_start_ms = start_ms + (stagger_ms * index);

    if (elapsed_ms <= item_start_ms) {
        return 0;
    }
    return ui_ease_out_cubic(elapsed_ms - item_start_ms, duration_ms);
}

void scale_morph_current_rects(
    const Movie *movie,
    ScaleMorphState *morph,
    ScaleMode scale_mode,
    VideoAlign video_align_x,
    VideoAlign video_align_y,
    uint32_t now_ms,
    SDL_Rect *src,
    SDL_Rect *dst
)
{
    uint8_t mix;

    if (!movie || !src || !dst) {
        return;
    }
    if (!morph || !morph->active) {
        compute_video_rects(movie, scale_mode, video_align_x, video_align_y, src, dst);
        return;
    }

    mix = ui_ease_out_cubic(now_ms - morph->started_ms, SCALE_MORPH_ANIM_MS);
    if (mix >= 255) {
        *src = morph->to_src;
        *dst = morph->to_dst;
        morph->active = false;
        return;
    }

    *src = mix_sdl_rects(&morph->from_src, &morph->to_src, mix);
    *dst = mix_sdl_rects(&morph->from_dst, &morph->to_dst, mix);
}

bool scale_morph_animating(const ScaleMorphState *morph, uint32_t now_ms)
{
    return morph &&
        morph->active &&
        (int32_t) (now_ms - (morph->started_ms + SCALE_MORPH_ANIM_MS)) < 0;
}

void begin_scale_mode_morph(
    Movie *movie,
    ScaleMorphState *morph,
    ScaleMode current_mode,
    ScaleMode next_mode,
    VideoAlign video_align_x,
    VideoAlign video_align_y,
    uint32_t now_ms
)
{
    SDL_Rect current_src;
    SDL_Rect current_dst;
    SDL_Rect target_src;
    SDL_Rect target_dst;

    if (!movie || !morph || current_mode == next_mode) {
        return;
    }

    scale_morph_current_rects(
        movie,
        morph,
        current_mode,
        video_align_x,
        video_align_y,
        now_ms,
        &current_src,
        &current_dst
    );
    compute_video_rects(movie, next_mode, video_align_x, video_align_y, &target_src, &target_dst);
    if (rects_equal(&current_src, &target_src) && rects_equal(&current_dst, &target_dst)) {
        morph->active = false;
        return;
    }

    morph->from_src = current_src;
    morph->from_dst = current_dst;
    morph->to_src = target_src;
    morph->to_dst = target_dst;
    morph->started_ms = now_ms ? now_ms : 1U;
    morph->active = true;
}

void cycle_scale_mode_with_morph(
    Movie *movie,
    ScaleMorphState *morph,
    ScaleMode *scale_mode,
    VideoAlign video_align_x,
    VideoAlign video_align_y,
    uint32_t now_ms
)
{
    ScaleMode next_mode;

    if (!scale_mode) {
        return;
    }
    next_mode = (ScaleMode) ((*scale_mode + 1) % 4);
    begin_scale_mode_morph(movie, morph, *scale_mode, next_mode, video_align_x, video_align_y, now_ms);
    *scale_mode = next_mode;
}

void ui_transition_init(UiTransition *transition, bool active)
{
    if (!transition) {
        return;
    }
    transition->initialized = true;
    transition->target_active = active;
    transition->start_mix = active ? 255 : 0;
    transition->current_mix = transition->start_mix;
    transition->started_ms = 0;
}

uint8_t ui_transition_value_with_ease(
    const UiTransition *transition,
    uint32_t now_ms,
    uint32_t duration_ms,
    uint8_t (*ease_fn)(uint32_t, uint32_t)
)
{
    uint8_t eased;

    if (!transition || !transition->initialized) {
        return 0;
    }
    eased = ease_fn(now_ms - transition->started_ms, duration_ms);
    if (transition->target_active) {
        return (uint8_t) (transition->start_mix +
            (((255U - transition->start_mix) * eased + 127U) / 255U));
    }
    return (uint8_t) (transition->start_mix -
        ((transition->start_mix * eased + 127U) / 255U));
}

uint8_t ui_transition_value(const UiTransition *transition, uint32_t now_ms, uint32_t duration_ms)
{
    return ui_transition_value_with_ease(transition, now_ms, duration_ms, ui_ease_out_cubic);
}

uint8_t ui_transition_update_ex(
    UiTransition *transition,
    bool active,
    uint32_t now_ms,
    uint32_t duration_ms,
    uint8_t active_min_mix
)
{
    uint8_t current_mix;

    if (!transition) {
        return active ? 255 : 0;
    }
    if (!transition->initialized) {
        ui_transition_init(transition, false);
    }
    current_mix = ui_transition_value(transition, now_ms, duration_ms);
    if (transition->target_active != active) {
        if (active && current_mix < active_min_mix) {
            current_mix = active_min_mix;
        }
        transition->target_active = active;
        transition->start_mix = current_mix;
        transition->started_ms = now_ms ? now_ms : 1U;
        current_mix = ui_transition_value(transition, now_ms, duration_ms);
    }
    transition->current_mix = current_mix;
    return current_mix;
}

uint8_t ui_transition_update(
    UiTransition *transition,
    bool active,
    uint32_t now_ms,
    uint32_t duration_ms
)
{
    return ui_transition_update_ex(transition, active, now_ms, duration_ms, 0);
}

uint8_t ui_transition_update_press_ex(
    UiTransition *transition,
    bool active,
    uint32_t now_ms,
    uint32_t press_duration_ms,
    uint32_t release_duration_ms
)
{
    uint8_t current_mix;
    uint32_t duration_ms;

    if (!transition) {
        return active ? 255 : 0;
    }
    if (!transition->initialized) {
        ui_transition_init(transition, false);
    }
    duration_ms = transition->target_active ? press_duration_ms : release_duration_ms;
    current_mix = ui_transition_value_with_ease(transition, now_ms, duration_ms, ui_ease_smoothstep);
    if (transition->target_active != active) {
        transition->target_active = active;
        transition->start_mix = current_mix;
        transition->started_ms = now_ms ? now_ms : 1U;
        duration_ms = active ? press_duration_ms : release_duration_ms;
        current_mix = ui_transition_value_with_ease(transition, now_ms, duration_ms, ui_ease_smoothstep);
    }
    transition->current_mix = current_mix;
    return current_mix;
}

uint8_t ui_transition_update_press(
    UiTransition *transition,
    bool active,
    uint32_t now_ms
)
{
    return ui_transition_update_press_ex(
        transition,
        active,
        now_ms,
        UI_PRESS_ANIM_MS,
        UI_PRESS_RELEASE_ANIM_MS
    );
}

void ui_transition_prime_press(UiTransition *transition, uint32_t now_ms)
{
    if (!transition) {
        return;
    }
    if (!transition->initialized) {
        ui_transition_init(transition, false);
    }
    if (transition->current_mix < UI_PRESS_PRIME_MIX) {
        transition->current_mix = UI_PRESS_PRIME_MIX;
    }
    transition->target_active = true;
    transition->start_mix = transition->current_mix;
    transition->started_ms = now_ms ? now_ms : 1U;
}

void ui_transition_begin_press_release(UiTransition *transition, uint32_t now_ms)
{
    if (!transition) {
        return;
    }
    if (!transition->initialized) {
        ui_transition_init(transition, false);
    }
    if (transition->current_mix < UI_PRESS_RELEASE_VISIBLE_MIX) {
        transition->current_mix = UI_PRESS_RELEASE_VISIBLE_MIX;
    }
    transition->target_active = false;
    transition->start_mix = transition->current_mix;
    transition->started_ms = now_ms ? now_ms : 1U;
}

bool ui_mix_animating(uint8_t mix)
{
    return mix > 0 && mix < 255;
}

bool playback_ui_mixes_animating(const PlaybackUiMixes *mixes)
{
    return mixes && (
        ui_mix_animating(mixes->chrome) ||
        ui_mix_animating(mixes->playback_badge) ||
        ui_mix_animating(mixes->playback_press) ||
        ui_mix_animating(mixes->scale_badge) ||
        ui_mix_animating(mixes->scale_press) ||
        ui_mix_animating(mixes->speed_badge) ||
        ui_mix_animating(mixes->speed_press) ||
        ui_mix_animating(mixes->seek_preview) ||
        ui_mix_animating(mixes->title_strip) ||
        ui_mix_animating(mixes->help_menu)
    );
}

void trigger_playback_badge_press(
    PlaybackUiTransitions *transitions,
    uint32_t *press_until_ms,
    uint32_t now_ms
)
{
    if (press_until_ms) {
        *press_until_ms = now_ms + UI_PRESS_ANIM_MS;
    }
    if (transitions) {
        ui_transition_prime_press(&transitions->playback_press, now_ms);
    }
}

void trigger_badge_press(UiTransition *transition, uint32_t *press_until_ms, uint32_t now_ms)
{
    if (press_until_ms) {
        *press_until_ms = now_ms + UI_PRESS_ANIM_MS;
    }
    ui_transition_prime_press(transition, now_ms);
}

bool ui_time_before(uint32_t now_ms, uint32_t until_ms)
{
    return until_ms != 0U && (int32_t) (now_ms - until_ms) < 0;
}

void status_overlay_show(uint32_t now_ms, bool restart_animation, uint32_t *started_ms, uint32_t *until_ms)
{
    if (now_ms == 0U) {
        now_ms = 1U;
    }
    if (started_ms && (restart_animation || *started_ms == 0U)) {
        *started_ms = now_ms;
    }
    if (until_ms) {
        *until_ms = now_ms + STATUS_OVERLAY_MS;
    }
}

bool seek_preview_surface_animating(const SeekBarPreviewState *preview, uint32_t now_ms)
{
    return preview &&
        preview->surface &&
        (preview->surface_fade_pending ||
            (preview->surface_started_ms != 0U &&
                ui_time_before(now_ms, preview->surface_started_ms + UI_TOOLTIP_ANIM_MS)));
}

void note_pause_transition(
    bool was_paused,
    bool now_paused,
    uint32_t now_ms,
    uint32_t *quiet_until_ms
)
{
    if (!quiet_until_ms) {
        return;
    }
    if (!was_paused && now_paused) {
        *quiet_until_ms = now_ms + UI_PAUSE_QUIET_MS;
    } else if (!now_paused) {
        *quiet_until_ms = 0;
    }
}

Uint16 animated_control_color(Uint16 idle_color, Uint16 active_color, uint8_t active_mix)
{
    return rgb565_lerp(idle_color, active_color, active_mix, 255);
}

SDL_Rect text_badge_rect(const Fonts *fonts, int right_x, int y, const char *label)
{
    int text_w = nSDL_GetStringWidth(fonts->white, label);
    SDL_Rect badge = {(Sint16) (right_x - text_w - 12), (Sint16) y, (Uint16) (text_w + 12), 16};

    return badge;
}

int pressed_control_offset_y(uint8_t press_mix)
{
    return press_mix >= 92 ? 1 : 0;
}

int pressed_control_offset_x(uint8_t press_mix)
{
    return press_mix >= 178 ? 1 : 0;
}

uint8_t max_u8(uint8_t a, uint8_t b)
{
    return a > b ? a : b;
}

static uint8_t mix_product_u8(uint8_t a, uint8_t b)
{
    return (uint8_t) (((uint16_t) a * (uint16_t) b + 127U) / 255U);
}

Uint16 pressed_control_base(Uint16 base, uint8_t press_mix)
{
    return rgb565_lerp(base, UI_COLOR_BLACK, (uint8_t) ((42U * press_mix) / 255U), 255);
}

void draw_pressed_control_reflection(SDL_Surface *screen, const SDL_Rect *rect, uint8_t press_mix)
{
    SDL_Rect line;
    int row;
    int inner_rows;

    if (!screen || !rect || rect->w < 4 || rect->h < 4 || press_mix == 0) {
        return;
    }

    inner_rows = rect->h > 2 ? rect->h - 2 : 0;
    for (row = 0; row < inner_rows; ++row) {
        int half = inner_rows > 1 ? inner_rows / 2 : 1;
        int alpha;
        Uint16 overlay;

        if (row < half) {
            alpha = 64 - ((row * 42) / half);
            overlay = UI_COLOR_BLACK;
        } else {
            int lower_row = row - half;
            int lower_span = inner_rows - half;

            if (lower_span < 1) {
                lower_span = 1;
            }
            alpha = 18 + ((lower_row * 54) / lower_span);
            overlay = UI_COLOR_WARM_WHITE;
        }
        alpha = clamp_int(alpha, 8, 82);
        alpha = (alpha * press_mix) / 255;
        line.x = (Sint16) (rect->x + 2);
        line.y = (Sint16) (rect->y + 1 + row);
        line.w = (Uint16) (rect->w - 4);
        line.h = 1;
        fill_rect_rgb565_mix(screen, &line, overlay, (uint8_t) alpha);
    }

    line.x = (Sint16) (rect->x + 1);
    line.y = (Sint16) (rect->y + 1);
    line.w = (Uint16) (rect->w - 2);
    line.h = 1;
    fill_rect_rgb565_mix(screen, &line, UI_COLOR_BLACK, (uint8_t) ((64U * press_mix) / 255U));
    line.x = (Sint16) (rect->x + 1);
    line.y = (Sint16) (rect->y + 1);
    line.w = 1;
    line.h = (Uint16) (rect->h - 2);
    fill_rect_rgb565_mix(screen, &line, UI_COLOR_BLACK, (uint8_t) ((58U * press_mix) / 255U));

    line.x = (Sint16) (rect->x + 2);
    line.y = (Sint16) (rect->y + rect->h - 2);
    line.w = (Uint16) (rect->w - 4);
    line.h = 1;
    fill_rect_rgb565_mix(screen, &line, UI_COLOR_WARM_WHITE, (uint8_t) ((104U * press_mix) / 255U));
    line.x = (Sint16) (rect->x + rect->w - 2);
    line.y = (Sint16) (rect->y + 2);
    line.w = 1;
    line.h = (Uint16) (rect->h - 4);
    fill_rect_rgb565_mix(screen, &line, UI_COLOR_WARM_WHITE, (uint8_t) ((84U * press_mix) / 255U));

    if (rect->h > 10 && rect->w > 8) {
        int lower_y = rect->y + rect->h - 4;

        line.x = (Sint16) (rect->x + 3);
        line.y = (Sint16) lower_y;
        line.w = (Uint16) (rect->w - 6);
        line.h = 1;
        fill_rect_rgb565_mix(screen, &line, UI_COLOR_WHITE, (uint8_t) ((42U * press_mix) / 255U));
    }

    line.x = (Sint16) (rect->x + 1);
    line.y = (Sint16) (rect->y + 1);
    line.w = 1;
    line.h = 1;
    fill_rect_rgb565_mix(screen, &line, UI_COLOR_BLACK, (uint8_t) ((52U * press_mix) / 255U));
    line.x = (Sint16) (rect->x + rect->w - 2);
    line.y = (Sint16) (rect->y + 1);
    line.w = 1;
    line.h = 1;
    fill_rect_rgb565_mix(screen, &line, UI_COLOR_BLACK, (uint8_t) ((24U * press_mix) / 255U));
}

static int draw_text_badge_visible(
    SDL_Surface *screen,
    const Fonts *fonts,
    int right_x,
    int y,
    const char *label,
    uint8_t hover_mix,
    uint8_t press_mix,
    uint8_t visible_mix
)
{
    SDL_Rect badge = text_badge_rect(fonts, right_x, y, label);
    uint8_t visible_hover_mix;
    uint8_t visible_press_mix;
    int press_offset_x;
    int press_offset_y;
    Uint16 base;

    if (visible_mix == 0) {
        return badge.x - 6;
    }
    visible_hover_mix = mix_product_u8(hover_mix, visible_mix);
    visible_press_mix = mix_product_u8(press_mix, visible_mix);
    press_offset_x = pressed_control_offset_x(visible_press_mix);
    press_offset_y = pressed_control_offset_y(visible_press_mix);
    base = rgb565_lerp(
        UI_COLOR_BLACK,
        animated_control_color(UI_COLOR_GUNMETAL, ui_theme()->row_selected, visible_hover_mix),
        visible_mix,
        255
    );

    draw_soft_glass_panel_mix(
        screen,
        &badge,
        pressed_control_base(base, visible_press_mix),
        visible_hover_mix
    );
    draw_pressed_control_reflection(screen, &badge, visible_press_mix);
    draw_soft_glass_panel_rim(screen, &badge, base, max_u8(visible_hover_mix, visible_press_mix));
    if (visible_mix > 48) {
        draw_ui_label(screen, fonts, badge.x + 6 + press_offset_x, badge.y + 4 + press_offset_y, label);
    }
    return badge.x - 6;
}

int draw_text_badge(
    SDL_Surface *screen,
    const Fonts *fonts,
    int right_x,
    int y,
    const char *label,
    uint8_t hover_mix,
    uint8_t press_mix
)
{
    return draw_text_badge_visible(screen, fonts, right_x, y, label, hover_mix, press_mix, 255);
}

int draw_left_text_badge(SDL_Surface *screen, const Fonts *fonts, int left_x, int y, const char *label)
{
    int text_w = nSDL_GetStringWidth(fonts->white, label);
    SDL_Rect badge = {(Sint16) left_x, (Sint16) y, (Uint16) (text_w + 12), 16};

    draw_soft_glass_panel(screen, &badge, UI_COLOR_GUNMETAL, false);
    draw_ui_label(screen, fonts, badge.x + 6, badge.y + 4, label);
    return badge.x + badge.w + 6;
}

int draw_left_text_badge_animated(
    SDL_Surface *screen,
    const Fonts *fonts,
    int left_x,
    int y,
    const char *label,
    uint8_t mix,
    int offset_x
)
{
    int text_w;
    SDL_Rect badge;

    if (!screen || !fonts || !label || mix == 0) {
        return left_x;
    }
    text_w = nSDL_GetStringWidth(fonts->white, label);
    badge.x = (Sint16) (left_x + offset_x);
    badge.y = (Sint16) y;
    badge.w = (Uint16) (text_w + 12);
    badge.h = 16;

    draw_soft_glass_panel(screen, &badge, rgb565_lerp(UI_COLOR_BLACK, UI_COLOR_GUNMETAL, mix, 255), false);
    if (mix > 48) {
        draw_ui_label(screen, fonts, badge.x + 6, badge.y + 4, label);
    }
    return badge.x + badge.w + 6;
}

static bool parse_brightness_status_label(const char *label, unsigned *out_percent)
{
    const char *cursor;
    unsigned percent = 0;
    bool saw_digit = false;

    if (!label || strncmp(label, "BRIGHT ", 7) != 0) {
        return false;
    }
    cursor = label + 7;
    while (*cursor == ' ') {
        ++cursor;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        saw_digit = true;
        percent = (percent * 10U) + (unsigned) (*cursor - '0');
        ++cursor;
    }
    if (!saw_digit || *cursor != '%') {
        return false;
    }
    if (out_percent) {
        *out_percent = percent;
    }
    return true;
}

static int draw_left_brightness_badge_animated(
    SDL_Surface *screen,
    const Fonts *fonts,
    int left_x,
    int y,
    unsigned percent,
    uint8_t mix,
    int offset_x
)
{
    const char *label_text = "BRIGHT";
    char percent_text[8];
    int label_w;
    int digit_w;
    int gap_w;
    int percent_w;
    SDL_Rect badge;

    if (!screen || !fonts || mix == 0) {
        return left_x;
    }

    snprintf(percent_text, sizeof(percent_text), "%u%%", percent);
    label_w = nSDL_GetStringWidth(fonts->white, label_text);
    digit_w = nSDL_GetStringWidth(fonts->white, "100%");
    gap_w = nSDL_GetStringWidth(fonts->white, "  ");
    percent_w = nSDL_GetStringWidth(fonts->white, percent_text);
    badge.x = (Sint16) (left_x + offset_x);
    badge.y = (Sint16) y;
    badge.w = (Uint16) (label_w + gap_w + digit_w + 12);
    badge.h = 16;

    draw_soft_glass_panel(screen, &badge, rgb565_lerp(UI_COLOR_BLACK, UI_COLOR_GUNMETAL, mix, 255), false);
    if (mix > 48) {
        draw_ui_label(screen, fonts, badge.x + 6, badge.y + 4, label_text);
        draw_ui_label(screen, fonts, badge.x + badge.w - 6 - percent_w, badge.y + 4, percent_text);
    }
    return badge.x + badge.w + 6;
}

void format_seek_delta(int32_t delta_ms, char *buffer, size_t buffer_size);
void copy_fitted_text(nSDL_Font *font, const char *text, char *buffer, size_t buffer_size, int max_width);

void draw_seek_delta_badge(
    SDL_Surface *screen,
    const Fonts *fonts,
    int32_t seek_ms,
    uint32_t started_ms,
    uint32_t hide_elapsed_ms,
    uint32_t now_ms
)
{
    char label[24];
    int text_w;
    SDL_Rect badge;
    uint8_t mix;
    bool exiting = hide_elapsed_ms != 0U && hide_elapsed_ms != SEEK_BADGE_HIDE_PENDING;
    bool negative = seek_ms < 0;
    int offset;

    if (!screen || !fonts || seek_ms == 0) {
        return;
    }
    if (exiting) {
        uint8_t out_mix = ui_ease_smoothstep(hide_elapsed_ms, SEEK_BADGE_EXIT_ANIM_MS);
        if (out_mix >= 255) {
            return;
        }
        mix = (uint8_t) (255U - out_mix);
        offset = ((int) out_mix * 20 + 127) / 255;
    } else {
        mix = ui_ease_out_cubic(now_ms - started_ms, SEEK_BADGE_ANIM_MS);
        offset = ((255 - (int) mix) * 18 + 127) / 255;
    }
    if (mix == 0) {
        return;
    }
    format_seek_delta(seek_ms, label, sizeof(label));
    text_w = nSDL_GetStringWidth(fonts->white, label);
    badge.y = (Sint16) ((SCREEN_H / 2) - 8);
    badge.w = (Uint16) (text_w + 12);
    badge.h = 16;
    if (negative) {
        badge.x = (Sint16) (chrome_left_x_for_margin(10) - offset);
    } else {
        badge.x = (Sint16) (chrome_right_x_for_margin(10) - badge.w + offset);
    }

    draw_soft_glass_panel(screen, &badge, rgb565_lerp(UI_COLOR_BLACK, UI_COLOR_GUNMETAL, mix, 255), false);
    if (mix > 48) {
        draw_ui_label(screen, fonts, badge.x + 6, badge.y + 4, label);
    }
}

void draw_status_overlay_badge(
    SDL_Surface *screen,
    const Fonts *fonts,
    int left_x,
    int y,
    const char *label,
    uint32_t started_ms,
    uint32_t until_ms,
    uint32_t now_ms,
    uint8_t chrome_mix
)
{
    uint8_t mix;
    int offset_x;
    int offset_y;
    unsigned brightness_percent;

    if (!label || label[0] == '\0' || started_ms == 0U || until_ms == 0U || chrome_mix == 0) {
        return;
    }
    if ((int32_t) (now_ms - until_ms) <= 0) {
        mix = ui_ease_out_cubic(now_ms - started_ms, STATUS_BADGE_ANIM_MS);
        offset_x = -((255 - (int) mix) * 14 + 127) / 255;
    } else if ((int32_t) (now_ms - (until_ms + STATUS_BADGE_EXIT_ANIM_MS)) < 0) {
        uint8_t out_mix = ui_ease_smoothstep(now_ms - until_ms, STATUS_BADGE_EXIT_ANIM_MS);

        mix = (uint8_t) (255U - out_mix);
        offset_x = -(((int) out_mix * 12 + 127) / 255);
    } else {
        return;
    }
    mix = mix_product_u8(mix, chrome_mix);
    offset_y = -(((255 - chrome_mix) * 4 + 127) / 255);
    if (parse_brightness_status_label(label, &brightness_percent)) {
        draw_left_brightness_badge_animated(screen, fonts, left_x, y + offset_y, brightness_percent, mix, offset_x);
    } else {
        draw_left_text_badge_animated(screen, fonts, left_x, y + offset_y, label, mix, offset_x);
    }
}

SDL_Rect playback_badge_rect(const SDL_Rect *video_rect);
void status_badge_rects(
    const Fonts *fonts,
    const SDL_Rect *video_rect,
    ScaleMode scale_mode,
    const PlaybackRate *playback_rate,
    SDL_Rect *scale_badge,
    SDL_Rect *speed_badge
);

void draw_playback_title_strip(
    SDL_Surface *screen,
    const Fonts *fonts,
    const SDL_Rect *video_rect,
    ScaleMode scale_mode,
    const PlaybackRate *playback_rate,
    const char *title,
    const char *detail,
    uint8_t mix
)
{
    SDL_Rect strip;
    SDL_Rect glint;
    SDL_Rect playback_badge;
    SDL_Rect scale_badge;
    SDL_Rect speed_badge;
    char fitted_title[160];
    char fitted_detail[160];
    int title_w;
    int detail_w;
    int required_w;
    int left_x;
    int right_x;
    int level_1_right;
    int level_2_right;
    int level_3_right;
    int text_x;
    int detail_x = 0;
    int y;
    bool has_detail;

    if (!screen || !fonts || !video_rect || !title || title[0] == '\0' || mix == 0) {
        return;
    }

    has_detail = detail && detail[0] != '\0';
    playback_badge = playback_badge_rect(video_rect);
    status_badge_rects(fonts, video_rect, scale_mode, playback_rate, &scale_badge, &speed_badge);
    left_x = playback_badge.x + playback_badge.w + 6;
    level_1_right = scale_badge.x - 6;
    level_2_right = speed_badge.x - 6;
    level_3_right = chrome_right_x_for_margin(8);
    title_w = nSDL_GetStringWidth(fonts->white, title);
    detail_w = has_detail ? nSDL_GetStringWidth(fonts->white, detail) : 0;
    required_w = (title_w > detail_w ? title_w : detail_w) + 16;
    if (required_w <= level_1_right - left_x) {
        right_x = level_1_right;
    } else if (required_w <= level_2_right - left_x) {
        right_x = level_2_right;
    } else {
        right_x = level_3_right;
    }
    if (right_x <= left_x + 16) {
        return;
    }

    y = (has_detail ? -30 : -20) + (((int) mix * (has_detail ? 34 : 26) + 127) / 255);
    strip.x = (Sint16) left_x;
    strip.y = (Sint16) y;
    strip.w = (Uint16) (right_x - left_x);
    strip.h = has_detail ? 30 : 20;
    copy_fitted_text(fonts->white, title, fitted_title, sizeof(fitted_title), strip.w - 16);
    text_x = strip.x + 8;
    if (has_detail) {
        copy_fitted_text(fonts->white, detail, fitted_detail, sizeof(fitted_detail), strip.w - 16);
        detail_x = strip.x + 8;
    }

    draw_soft_glass_panel_mix(
        screen,
        &strip,
        rgb565_lerp(ui_theme()->gunmetal, ui_theme()->row_selected, mix, 255),
        mix
    );
    if (mix > 40) {
        glint.x = (Sint16) (strip.x + 3);
        glint.y = (Sint16) (strip.y + 2);
        glint.w = (Uint16) (strip.w - 6);
        glint.h = 1;
        fill_rect_rgb565(screen, &glint, rgb565_lerp(ui_theme()->row_selected, UI_COLOR_WARM_WHITE, 72, 255));
    }
    if (mix > 48) {
        draw_ui_label(screen, fonts, text_x, strip.y + (has_detail ? 5 : 6), fitted_title);
        if (has_detail) {
            draw_ui_label(screen, fonts, detail_x, strip.y + 17, fitted_detail);
        }
    }
}

void draw_centered_text_badge(SDL_Surface *screen, const Fonts *fonts, int center_x, int y, const char *label)
{
    int text_w = nSDL_GetStringWidth(fonts->white, label);
    int width = text_w + 12;
    SDL_Rect badge = {
        (Sint16) clamp_int(center_x - (width / 2), 0, SCREEN_W - width),
        (Sint16) y,
        (Uint16) width,
        16
    };

    draw_soft_glass_panel(screen, &badge, UI_COLOR_GUNMETAL, false);
    draw_ui_label(screen, fonts, badge.x + 6, badge.y + 4, label);
}

int header_shortcut_badge_width(const Fonts *fonts, const char *key, const char *action)
{
    if (!fonts || !key || !action) {
        return 0;
    }
    return nSDL_GetStringWidth(fonts->white, key) +
        nSDL_GetStringWidth(fonts->white, action) + 22;
}

void draw_header_shortcut_badge(
    SDL_Surface *screen,
    const Fonts *fonts,
    int x,
    int y,
    const char *key,
    const char *action,
    uint8_t mix
)
{
    int key_w;
    SDL_Rect badge;
    SDL_Rect glint;
    SDL_Rect dot;

    if (!screen || !fonts || !key || !action || mix == 0) {
        return;
    }

    key_w = nSDL_GetStringWidth(fonts->white, key);
    badge.x = (Sint16) x;
    badge.y = (Sint16) y;
    badge.w = (Uint16) header_shortcut_badge_width(fonts, key, action);
    badge.h = 13;
    glint.x = (Sint16) (badge.x + 3);
    glint.y = (Sint16) (badge.y + 2);
    glint.w = (Uint16) (badge.w - 6);
    glint.h = 1;
    dot.x = (Sint16) (badge.x + 7 + key_w + 4);
    dot.y = (Sint16) (badge.y + 6);
    dot.w = 2;
    dot.h = 2;

    draw_soft_glass_panel(screen, &badge, rgb565_lerp(UI_COLOR_BLACK, ui_theme()->shortcut_base, mix, 255), false);
    fill_rect_rgb565(screen, &glint, rgb565_lerp(UI_COLOR_BLACK, ui_theme()->shortcut_glint, mix, 255));
    fill_rect_rgb565(screen, &dot, rgb565_lerp(UI_COLOR_BLACK, UI_COLOR_ACCENT_HOT, mix, 255));
    if (mix > 54) {
        draw_ui_label(screen, fonts, badge.x + 6, badge.y + 3, key);
        draw_ui_label(screen, fonts, dot.x + 5, badge.y + 3, action);
    }
}

void draw_header_shortcuts(SDL_Surface *screen, const Fonts *fonts, int y, uint8_t mix)
{
    const char *keys[] = {
        "ENTER",
        "UP/DOWN",
        "ESC"
    };
    const char *actions[] = {
        "OPEN",
        "CHOOSE",
        "EXIT"
    };
    int widths[3];
    int total_w = 0;
    int x;
    size_t index;
    const int gap = 5;

    if (!screen || !fonts) {
        return;
    }

    for (index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        widths[index] = header_shortcut_badge_width(fonts, keys[index], actions[index]);
        total_w += widths[index];
    }
    total_w += gap * ((int) (sizeof(keys) / sizeof(keys[0])) - 1);
    x = (SCREEN_W - total_w) / 2;
    for (index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        draw_header_shortcut_badge(screen, fonts, x, y, keys[index], actions[index], mix);
        x += widths[index] + gap;
    }
}

void draw_surface_panel(SDL_Surface *screen, SDL_Surface *surface, int x, int y)
{
    SDL_Rect border;
    SDL_Rect inner;
    SDL_Rect dst;

    if (!screen || !surface) {
        return;
    }

    border.x = (Sint16) x;
    border.y = (Sint16) y;
    border.w = (Uint16) (surface->w + 4);
    border.h = (Uint16) (surface->h + 4);
    inner.x = (Sint16) (x + 1);
    inner.y = (Sint16) (y + 1);
    inner.w = (Uint16) (surface->w + 2);
    inner.h = (Uint16) (surface->h + 2);
    dst.x = (Sint16) (x + 2);
    dst.y = (Sint16) (y + 2);
    dst.w = (Uint16) surface->w;
    dst.h = (Uint16) surface->h;

    fill_rect_rgb565(screen, &border, ui_theme()->surface_outer);
    fill_rect_rgb565(screen, &inner, UI_COLOR_WARM_WHITE);
    SDL_BlitSurface(surface, NULL, screen, &dst);
}

void draw_seek_preview_panel(SDL_Surface *screen, SDL_Surface *surface, int x, int y, int marker_x, uint8_t panel_mix)
{
    SDL_Rect outer;
    SDL_Rect inner;
    SDL_Rect dst;
    SDL_Rect triangle;
    int center_x;

    if (!screen || !surface || panel_mix == 0) {
        return;
    }

    outer.x = (Sint16) x;
    outer.y = (Sint16) y;
    outer.w = (Uint16) (surface->w + 4);
    outer.h = (Uint16) (surface->h + 4);
    inner.x = (Sint16) (x + 1);
    inner.y = (Sint16) (y + 1);
    inner.w = (Uint16) (surface->w + 2);
    inner.h = (Uint16) (surface->h + 2);
    dst.x = (Sint16) (x + 2);
    dst.y = (Sint16) (y + 2);
    dst.w = (Uint16) surface->w;
    dst.h = (Uint16) surface->h;

    fill_rect_rgb565_mix(screen, &outer, ui_theme()->preview_outer, panel_mix);
    fill_rect_rgb565_mix(screen, &inner, UI_COLOR_WARM_WHITE, panel_mix);
    blit_surface_rgb565_mix(screen, surface, &dst, panel_mix);

    center_x = clamp_int(marker_x, outer.x + 5, outer.x + outer.w - 6);
    triangle.x = (Sint16) (center_x - 3);
    triangle.y = (Sint16) (outer.y + outer.h - 1);
    triangle.w = 7;
    triangle.h = 1;
    fill_rect_rgb565_mix(screen, &triangle, ui_theme()->preview_outer, panel_mix);
    triangle.x = (Sint16) (center_x - 2);
    triangle.y = (Sint16) (outer.y + outer.h);
    triangle.w = 5;
    fill_rect_rgb565_mix(screen, &triangle, ui_theme()->preview_outer, panel_mix);
    triangle.x = (Sint16) (center_x - 1);
    triangle.y = (Sint16) (outer.y + outer.h + 1);
    triangle.w = 3;
    fill_rect_rgb565_mix(screen, &triangle, ui_theme()->preview_outer, panel_mix);
    triangle.x = (Sint16) center_x;
    triangle.y = (Sint16) (outer.y + outer.h + 2);
    triangle.w = 1;
    fill_rect_rgb565_mix(screen, &triangle, ui_theme()->preview_outer, panel_mix);
    triangle.x = (Sint16) (center_x - 2);
    triangle.y = (Sint16) (outer.y + outer.h - 1);
    triangle.w = 5;
    fill_rect_rgb565_mix(screen, &triangle, UI_COLOR_WARM_WHITE, panel_mix);
    triangle.x = (Sint16) (center_x - 1);
    triangle.y = (Sint16) (outer.y + outer.h);
    triangle.w = 3;
    fill_rect_rgb565_mix(screen, &triangle, UI_COLOR_WARM_WHITE, panel_mix);
    triangle.x = (Sint16) center_x;
    triangle.y = (Sint16) (outer.y + outer.h + 1);
    triangle.w = 1;
    fill_rect_rgb565_mix(screen, &triangle, UI_COLOR_WARM_WHITE, panel_mix);
}

void draw_screenshot_preview_osd(
    SDL_Surface *screen,
    const Fonts *fonts,
    const ScreenshotPreviewState *preview,
    uint32_t now_ms
)
{
    int panel_x;
    int panel_y;

    if (!screen || !fonts || !preview || !preview->surface || now_ms > preview->until_ms) {
        return;
    }

    panel_x = 8;
    panel_y = 30;
    draw_surface_panel(screen, preview->surface, panel_x, panel_y);
    draw_left_text_badge(screen, fonts, panel_x, panel_y + preview->surface->h + 8, preview->label);
}

void format_seek_delta(int32_t delta_ms, char *buffer, size_t buffer_size)
{
    uint32_t magnitude_ms;
    char time_text[24];
    char formatted[32];
    char *out = formatted;

    if (!buffer || buffer_size == 0) {
        return;
    }

    if (delta_ms == 0) {
        copy_truncated(buffer, buffer_size, "0s");
        return;
    }

    magnitude_ms = (uint32_t) (delta_ms < 0 ? -delta_ms : delta_ms);
    format_clock(magnitude_ms, time_text, sizeof(time_text));
    *out++ = delta_ms < 0 ? '-' : '+';
    copy_truncated(out, sizeof(formatted) - 1, time_text);
    copy_truncated(buffer, buffer_size, formatted);
}

void status_badge_rects(
    const Fonts *fonts,
    const SDL_Rect *video_rect,
    ScaleMode scale_mode,
    const PlaybackRate *playback_rate,
    SDL_Rect *scale_badge,
    SDL_Rect *speed_badge
)
{
    int right_x = chrome_soft_panel_right_x_for_margin(8);
    int y = top_overlay_y_for_rect(video_rect, 16);
    SDL_Rect speed = text_badge_rect(fonts, right_x, y, playback_rate ? playback_rate->label : "1.0x");

    if (speed_badge) {
        *speed_badge = speed;
    }
    right_x = speed.x - 6;
    if (scale_badge) {
        *scale_badge = text_badge_rect(fonts, right_x, y, scale_mode_text(scale_mode));
    }
}

int draw_status_badges(
    SDL_Surface *screen,
    const Fonts *fonts,
    const SDL_Rect *video_rect,
    ScaleMode scale_mode,
    const PlaybackRate *playback_rate,
    const PlaybackUiMixes *ui_mixes,
    uint8_t chrome_mix
)
{
    SDL_Rect scale_badge;
    SDL_Rect speed_badge;
    uint8_t speed_mix = ui_mixes ? ui_mixes->speed_badge : 0;
    uint8_t scale_mix = ui_mixes ? ui_mixes->scale_badge : 0;
    uint8_t speed_press = ui_mixes ? ui_mixes->speed_press : 0;
    uint8_t scale_press = ui_mixes ? ui_mixes->scale_press : 0;
    int y_offset = -(((255 - chrome_mix) * 4 + 127) / 255);

    status_badge_rects(fonts, video_rect, scale_mode, playback_rate, &scale_badge, &speed_badge);
    draw_text_badge_visible(screen, fonts, speed_badge.x + speed_badge.w, speed_badge.y + y_offset, playback_rate ? playback_rate->label : "1.0x", speed_mix, speed_press, chrome_mix);
    draw_text_badge_visible(screen, fonts, scale_badge.x + scale_badge.w, scale_badge.y + y_offset, scale_mode_text(scale_mode), scale_mix, scale_press, chrome_mix);
    return scale_badge.x - 6;
}

void update_playback_ui_mixes(
    PlaybackUiTransitions *transitions,
    PlaybackUiMixes *mixes,
    const Fonts *fonts,
    const Movie *movie,
    ScaleMode scale_mode,
    ScaleMorphState *scale_morph,
    VideoAlign video_align_x,
    VideoAlign video_align_y,
    const PlaybackRate *playback_rate,
    bool show_ui,
    bool help_menu_open,
    PlaybackPressTarget pressed_target,
    bool force_playback_press,
    bool force_scale_press,
    bool force_speed_press,
    bool show_title_strip,
    const PointerState *pointer,
    uint32_t now_ms
)
{
    SDL_Rect src;
    SDL_Rect dst;
    SDL_Rect playback_badge;
    SDL_Rect scale_badge;
    SDL_Rect speed_badge;
    bool controls_live;
    bool playback_hovered;
    bool scale_hovered;
    bool speed_hovered;
    bool seek_hovered;

    if (!mixes) {
        return;
    }
    memset(mixes, 0, sizeof(*mixes));
    if (!transitions || !fonts || !movie) {
        return;
    }

    scale_morph_current_rects(movie, scale_morph, scale_mode, video_align_x, video_align_y, now_ms, &src, &dst);
    playback_badge = playback_badge_rect(&dst);
    status_badge_rects(fonts, &dst, scale_mode, playback_rate, &scale_badge, &speed_badge);

    mixes->chrome = ui_transition_update_ex(
        &transitions->chrome,
        show_ui,
        now_ms,
        UI_CHROME_ANIM_MS,
        UI_WAKE_MIN_MIX
    );
    controls_live = show_ui && !help_menu_open && pointer && pointer->visible;
    playback_hovered = controls_live && pointer_over_rect(pointer, &playback_badge);
    scale_hovered = controls_live && pointer_over_rect(pointer, &scale_badge);
    speed_hovered = controls_live && pointer_over_rect(pointer, &speed_badge);
    seek_hovered = controls_live && pointer->y >= SCREEN_H - UI_BAR_H && pointer->y < SCREEN_H;
    mixes->playback_badge = ui_transition_update(
        &transitions->playback_badge,
        playback_hovered,
        now_ms,
        UI_HOVER_ANIM_MS
    );
    mixes->playback_press = ui_transition_update_press(
        &transitions->playback_press,
        force_playback_press ||
            (pressed_target == PLAYBACK_PRESS_PLAY && pointer && pointer->down && playback_hovered),
        now_ms
    );
    mixes->scale_badge = ui_transition_update(
        &transitions->scale_badge,
        scale_hovered,
        now_ms,
        UI_HOVER_ANIM_MS
    );
    mixes->scale_press = ui_transition_update_press(
        &transitions->scale_press,
        force_scale_press ||
            (pressed_target == PLAYBACK_PRESS_SCALE && pointer && pointer->down && scale_hovered),
        now_ms
    );
    mixes->speed_badge = ui_transition_update(
        &transitions->speed_badge,
        speed_hovered,
        now_ms,
        UI_HOVER_ANIM_MS
    );
    mixes->speed_press = ui_transition_update_press(
        &transitions->speed_press,
        force_speed_press ||
            (pressed_target == PLAYBACK_PRESS_SPEED && pointer && pointer->down && speed_hovered),
        now_ms
    );
    mixes->seek_preview = ui_transition_update(
        &transitions->seek_preview,
        seek_hovered,
        now_ms,
        UI_TOOLTIP_ANIM_MS
    );
    mixes->title_strip = ui_transition_update(
        &transitions->title_strip,
        show_title_strip,
        now_ms,
        UI_TITLE_ANIM_MS
    );
    mixes->help_menu = ui_transition_update_press_ex(
        &transitions->help_menu,
        help_menu_open,
        now_ms,
        UI_MENU_ANIM_MS,
        UI_MENU_ANIM_MS
    );
}

SDL_Rect playback_badge_rect(const SDL_Rect *video_rect)
{
    int y = top_overlay_y_for_rect(video_rect, 22);
    SDL_Rect outer = {chrome_left_x_for_margin(8), (Sint16) y, 22, 22};

    return outer;
}

void draw_playback_badge(SDL_Surface *screen, const SDL_Rect *video_rect, bool paused, uint8_t hover_mix, uint8_t press_mix, uint8_t chrome_mix)
{
    SDL_Rect outer = playback_badge_rect(video_rect);
    int y_offset = -(((255 - chrome_mix) * 4 + 127) / 255);
    int press_offset_x = pressed_control_offset_x(press_mix);
    int press_offset_y = pressed_control_offset_y(press_mix);
    int y;
    SDL_Rect fill;
    Uint16 base;
    Uint16 pressed_base;

    outer.y = (Sint16) (outer.y + y_offset);
    y = outer.y;
    fill.x = (Sint16) (outer.x + 1 + press_offset_x);
    fill.y = (Sint16) (y + 1 + press_offset_y);
    fill.w = 20;
    fill.h = 20;
    base = rgb565_lerp(
        UI_COLOR_BLACK,
        animated_control_color(UI_COLOR_CARBON, ui_theme()->row_selected, hover_mix),
        chrome_mix,
        255
    );
    pressed_base = pressed_control_base(base, press_mix);

    draw_soft_glass_panel_mix(
        screen,
        &outer,
        pressed_base,
        hover_mix
    );
    draw_pressed_control_reflection(screen, &outer, press_mix);
    draw_soft_glass_panel_rim(screen, &outer, base, max_u8(hover_mix, press_mix));
    if (chrome_mix < 48) {
        return;
    }
    if (paused) {
        SDL_Rect left_bar = {(Sint16) (outer.x + 6 + press_offset_x), (Sint16) (y + 5 + press_offset_y), 3, 10};
        SDL_Rect right_bar = {(Sint16) (outer.x + 13 + press_offset_x), (Sint16) (y + 5 + press_offset_y), 3, 10};
        fill_rect_rgb565(screen, &left_bar, UI_COLOR_WHITE);
        fill_rect_rgb565(screen, &right_bar, UI_COLOR_WHITE);
    } else {
        int triangle_width = 9;
        int triangle_half_height = 6;
        int triangle_left = fill.x + ((fill.w - triangle_width) / 2);
        int triangle_center_y = fill.y + ((fill.h - 1) / 2);
        int triangle_denominator = triangle_width - 1;
        int column;

        for (column = 0; column < triangle_width; ++column) {
            int remaining = triangle_denominator - column;
            int half_height = (triangle_half_height * remaining + (triangle_denominator / 2)) / triangle_denominator;
            SDL_Rect slice = {
                (Sint16) (triangle_left + column),
                (Sint16) (triangle_center_y - half_height),
                1,
                (Uint16) (half_height * 2 + 1)
            };
            fill_rect_rgb565(screen, &slice, UI_COLOR_WHITE);
        }
    }
}

void draw_memory_badge(
    SDL_Surface *screen,
    const Fonts *fonts,
    const Movie *movie,
    const SDL_Rect *video_rect,
    int right_limit,
    bool playback_badge_visible,
    uint8_t chrome_mix
)
{
    MemoryStats stats = query_memory_stats(movie);
    char app_text[16];
    char prefetched_text[16];
    char total_text[16];
    char free_text[16];
    char label_full[80];
    char label_medium[64];
    char label_short[48];
    char perf_full[104];
    char perf_medium[80];
    char perf_short[48];
    const char *label = NULL;
    const char *perf_label = NULL;
    uint32_t fps_x10 = movie ? movie->diag_display_fps_x10 : 0U;
    int left_x;
    int y;
    int y_offset;

    if (!stats.valid || chrome_mix == 0) {
        return;
    }

    format_memory_compact(stats.used_bytes, app_text, sizeof(app_text));
    format_memory_compact(stats.prefetched_bytes, prefetched_text, sizeof(prefetched_text));
    format_memory_compact(stats.total_bytes, total_text, sizeof(total_text));
    format_memory_compact(stats.free_bytes, free_text, sizeof(free_text));
    snprintf(label_full, sizeof(label_full), "RAM %s/%s C%s %u%% F%s", app_text, total_text, prefetched_text, stats.percent_used, free_text);
    snprintf(label_medium, sizeof(label_medium), "RAM %s/%s C%s", app_text, total_text, prefetched_text);
    snprintf(label_short, sizeof(label_short), "RAM %s/%s", app_text, total_text);
    snprintf(
        perf_full,
        sizeof(perf_full),
        "F%lu L%lu D%lu %lu.%luFPS%s",
        movie ? (unsigned long) movie->current_frame : 0UL,
        movie ? (unsigned long) movie->diag_lag_event_count : 0UL,
        movie ? (unsigned long) movie->diag_foreground_direct_decode_count : 0UL,
        (unsigned long) (fps_x10 / 10U),
        (unsigned long) (fps_x10 % 10U),
        debug_is_runtime_logging_enabled() ? " DBG ON" : ""
    );
    snprintf(
        perf_medium,
        sizeof(perf_medium),
        "L%lu D%lu %luFPS%s",
        movie ? (unsigned long) movie->diag_lag_event_count : 0UL,
        movie ? (unsigned long) movie->diag_foreground_direct_decode_count : 0UL,
        (unsigned long) ((fps_x10 + 5U) / 10U),
        debug_is_runtime_logging_enabled() ? " DBG ON" : ""
    );
    snprintf(
        perf_short,
        sizeof(perf_short),
        "%luFPS%s",
        (unsigned long) ((fps_x10 + 5U) / 10U),
        debug_is_runtime_logging_enabled() ? " DBG" : ""
    );

    if (playback_badge_visible) {
        SDL_Rect playback_badge = playback_badge_rect(video_rect);
        left_x = playback_badge.x + playback_badge.w + 6;
    } else {
        left_x = chrome_left_x_for_margin(8);
    }
    y_offset = -(((255 - chrome_mix) * 4 + 127) / 255);
    y = top_overlay_y_for_rect(video_rect, 16) + y_offset;

    if (left_x + nSDL_GetStringWidth(fonts->white, label_full) + 10 <= right_limit) {
        label = label_full;
    } else if (left_x + nSDL_GetStringWidth(fonts->white, label_medium) + 10 <= right_limit) {
        label = label_medium;
    } else if (left_x + nSDL_GetStringWidth(fonts->white, label_short) + 10 <= right_limit) {
        label = label_short;
    }

    if (label) {
        draw_left_text_badge_animated(screen, fonts, left_x, y, label, chrome_mix, 0);
    }
    if (left_x + nSDL_GetStringWidth(fonts->white, perf_full) + 10 <= right_limit) {
        perf_label = perf_full;
    } else if (left_x + nSDL_GetStringWidth(fonts->white, perf_medium) + 10 <= right_limit) {
        perf_label = perf_medium;
    } else if (left_x + nSDL_GetStringWidth(fonts->white, perf_short) + 10 <= right_limit) {
        perf_label = perf_short;
    }

    if (perf_label) {
        draw_left_text_badge_animated(screen, fonts, left_x, y + 18, perf_label, chrome_mix, 0);
    }
}

void draw_help_row(
    SDL_Surface *screen,
    const Fonts *fonts,
    int shortcut_x,
    int shortcut_w,
    int description_x,
    int y,
    const char *shortcut,
    const char *description
)
{
    SDL_Rect key = {(Sint16) (shortcut_x - 4), (Sint16) (y - 1), (Uint16) (shortcut_w + 8), 9};

    draw_soft_glass_panel_mix(screen, &key, ui_theme()->help_key, 0);
    draw_ui_label(screen, fonts, shortcut_x, y, shortcut);
    draw_ui_label(screen, fonts, description_x, y, description);
}

static void draw_help_menu_contents(SDL_Surface *screen, const Fonts *fonts, const SDL_Rect *menu_border, uint8_t style_mix)
{
    SDL_Rect border;
    SDL_Rect panel;
    SDL_Rect header;
    SDL_Rect accent;
    const char *title_text = "Playback Controls";
    const struct {
        const char *shortcut;
        const char *description;
    } rows[] = {
        {"SPACE", "Play or pause"},
        {"ENTER/5", "Toggle / click / seek"},
        {"L/R 4/6", "Seek -/+5s"},
        {"7 / 9", "Prev / next video"},
        {"TAB", "Step one frame"},
        {"P", "Playback mode"},
        {"R", "Realtime frame skip"},
        {"/", "Scale mode"},
        {"CTRL+NUM", "Align video / center"},
        {"U/D 8/2", "Screen brightness"},
        {"{ / }", "Playback speed"},
        {"^", "Subtitle position"},
        {"+ / -", "Subtitle size"},
        {"F", "Cycle subtitle font"},
        {"T", "Switch subtitle track"},
        {"M", "Memory overlay"},
        {"C", "Theme color"},
        {"D", "Toggle debug logging"},
        {"S", "Save BMP screenshot"},
        {"TOUCHPAD", "Move cursor / show UI"},
        {"SCRATCH", "Open OS Scratchpad"},
        {"ESC", "Close menu or exit"},
    };
    int max_shortcut_w = 0;
    int shortcut_x;
    int description_x;
    int body_top_y;
    int body_bottom_y;
    int row_span;
    int first_row_y;
    int last_row_y;
    int close_badge_w;
    size_t row_count = sizeof(rows) / sizeof(rows[0]);
    size_t index;

    if (!screen || !fonts || !menu_border || style_mix == 0) {
        return;
    }
    border = *menu_border;
    panel.x = (Sint16) (border.x + 1);
    panel.y = (Sint16) (border.y + 1);
    panel.w = (Uint16) (border.w - 2);
    panel.h = (Uint16) (border.h - 2);
    header.x = panel.x;
    header.y = panel.y;
    header.w = panel.w;
    header.h = 22;
    accent.x = panel.x;
    accent.y = (Sint16) (panel.y + header.h);
    accent.w = panel.w;
    accent.h = 2;

    fill_soft_panel_backplate(screen, &border, ui_theme()->menu_border);
    draw_soft_glass_panel_mix(
        screen,
        &panel,
        rgb565_lerp(UI_COLOR_BLACK, ui_theme()->menu_panel, style_mix, 255),
        0
    );
    draw_soft_glass_panel_mix(
        screen,
        &header,
        rgb565_lerp(UI_COLOR_BLACK, UI_COLOR_GUNMETAL, style_mix, 255),
        0
    );
    draw_vertical_gradient(
        screen,
        &accent,
        rgb565_lerp(ui_theme()->menu_panel, UI_COLOR_ACCENT, style_mix, 255),
        rgb565_lerp(ui_theme()->menu_panel, UI_COLOR_ACCENT_DEEP, style_mix, 255)
    );

    draw_ui_label(screen, fonts, panel.x + 10, panel.y + 6, title_text);
    close_badge_w = header_shortcut_badge_width(fonts, "CAT", "CLOSE");
    draw_header_shortcut_badge(
        screen,
        fonts,
        panel.x + panel.w - 10 - close_badge_w,
        panel.y + 5,
        "CAT",
        "CLOSE",
        255
    );

    for (index = 0; index < row_count; ++index) {
        int width = nSDL_GetStringWidth(fonts->white, rows[index].shortcut);
        if (width > max_shortcut_w) {
            max_shortcut_w = width;
        }
    }
    shortcut_x = panel.x + 12;
    description_x = shortcut_x + max_shortcut_w + 16;
    body_top_y = accent.y + accent.h + 5;
    body_bottom_y = panel.y + panel.h - 12;
    row_span = row_count > 1 ? ((int) row_count - 1) * 8 : 0;
    if (row_span > body_bottom_y - body_top_y) {
        row_span = body_bottom_y - body_top_y;
    }
    first_row_y = body_top_y + ((body_bottom_y - body_top_y - row_span) / 2);
    last_row_y = first_row_y + row_span;
    for (index = 0; index < row_count; ++index) {
        int y = first_row_y;
        if (row_count > 1) {
            int current_row_span = last_row_y - first_row_y;
            int row_intervals = (int) row_count - 1;
            y += (current_row_span * (int) index + row_intervals / 2) / row_intervals;
        }
        draw_help_row(screen, fonts, shortcut_x, max_shortcut_w, description_x, y, rows[index].shortcut, rows[index].description);
    }
}

static SDL_Surface *cached_help_menu_surface(const Fonts *fonts, int menu_w, int menu_h)
{
    static SDL_Surface *menu_surface = NULL;
    static UiThemeId menu_theme = UI_THEME_COUNT;

    if (!fonts || menu_w <= 0 || menu_h <= 0) {
        return NULL;
    }
    if (menu_surface && !ui_theme_transition_active() && menu_theme == g_ui_theme_id &&
        menu_surface->w == menu_w && menu_surface->h == menu_h) {
        return menu_surface;
    }
    if (menu_surface) {
        SDL_FreeSurface(menu_surface);
        menu_surface = NULL;
    }

    menu_surface = create_rgb565_surface(menu_w, menu_h);
    if (menu_surface) {
        SDL_Rect local_border = {0, 0, (Uint16) menu_w, (Uint16) menu_h};

        SDL_FillRect(menu_surface, NULL, map_rgb565(menu_surface, UI_CURSOR_KEY));
        SDL_SetColorKey(menu_surface, SDL_SRCCOLORKEY, map_rgb565(menu_surface, UI_CURSOR_KEY));
        draw_help_menu_contents(menu_surface, fonts, &local_border, 255);
        menu_theme = ui_theme_transition_active() ? UI_THEME_COUNT : g_ui_theme_id;
    }
    return menu_surface;
}

static inline Uint16 blend_rgb565_fast_alpha(Uint16 base, Uint16 overlay, uint8_t alpha)
{
    int br = (base >> 11) & 0x1F;
    int bg = (base >> 5) & 0x3F;
    int bb = base & 0x1F;
    int or = (overlay >> 11) & 0x1F;
    int og = (overlay >> 5) & 0x3F;
    int ob = overlay & 0x1F;

    br += (((or - br) * (int) alpha + 128) >> 8);
    bg += (((og - bg) * (int) alpha + 128) >> 8);
    bb += (((ob - bb) * (int) alpha + 128) >> 8);
    return (Uint16) ((br << 11) | (bg << 5) | bb);
}

static void blit_help_menu_surface_faded(SDL_Surface *screen, SDL_Surface *surface, const SDL_Rect *dst_rect, uint8_t mix)
{
    int dst_x0;
    int dst_y0;
    int dst_x1;
    int dst_y1;
    int src_x0;
    int src_y0;
    int y;
    Uint16 color_key = UI_CURSOR_KEY;
    bool screen_locked = false;
    bool surface_locked = false;

    if (!screen || !surface || !dst_rect || mix == 0) {
        return;
    }
    if (mix >= 255) {
        SDL_BlitSurface(surface, NULL, screen, (SDL_Rect *) dst_rect);
        return;
    }
    if (!surface_is_rgb565(screen) || !surface_is_rgb565(surface)) {
        SDL_BlitSurface(surface, NULL, screen, (SDL_Rect *) dst_rect);
        return;
    }
    if ((surface->flags & SDL_SRCCOLORKEY) != 0) {
        color_key = (Uint16) surface->format->colorkey;
    }

    dst_x0 = clamp_int(dst_rect->x, 0, screen->w);
    dst_y0 = clamp_int(dst_rect->y, 0, screen->h);
    dst_x1 = clamp_int(dst_rect->x + surface->w, 0, screen->w);
    dst_y1 = clamp_int(dst_rect->y + surface->h, 0, screen->h);
    if (dst_x0 >= dst_x1 || dst_y0 >= dst_y1) {
        return;
    }
    src_x0 = dst_x0 - dst_rect->x;
    src_y0 = dst_y0 - dst_rect->y;

    if (SDL_MUSTLOCK(screen)) {
        if (SDL_LockSurface(screen) != 0) {
            return;
        }
        screen_locked = true;
    }
    if (SDL_MUSTLOCK(surface)) {
        if (SDL_LockSurface(surface) != 0) {
            if (screen_locked) {
                SDL_UnlockSurface(screen);
            }
            return;
        }
        surface_locked = true;
    }

    {
        Uint16 *dst_pixels = (Uint16 *) screen->pixels;
        Uint16 *src_pixels = (Uint16 *) surface->pixels;
        int dst_pitch = screen->pitch / 2;
        int src_pitch = surface->pitch / 2;

        for (y = dst_y0; y < dst_y1; ++y) {
            int local_y = src_y0 + (y - dst_y0);
            Uint16 *dst_row = dst_pixels + (y * dst_pitch) + dst_x0;
            Uint16 *src_row = src_pixels + (local_y * src_pitch) + src_x0;
            int x;

            for (x = dst_x0; x < dst_x1; ++x) {
                Uint16 src = *src_row;

                if (src != color_key) {
                    *dst_row = blend_rgb565_fast_alpha(*dst_row, src, mix);
                }
                ++dst_row;
                ++src_row;
            }
        }
    }

    if (surface_locked) {
        SDL_UnlockSurface(surface);
    }
    if (screen_locked) {
        SDL_UnlockSurface(screen);
    }
}

void draw_help_menu(SDL_Surface *screen, const Fonts *fonts, uint8_t menu_mix)
{
    const int menu_w = 296;
    const int safe_h = SCREEN_H - UI_BAR_H;
    const int menu_h = safe_h;
    int offset_y;
    SDL_Rect border;
    SDL_Surface *menu_surface;

    if (!screen || !fonts || menu_mix == 0) {
        return;
    }

    offset_y = ((255 - menu_mix) * UI_BAR_H + 127) / 255;
    border.x = (Sint16) ((SCREEN_W - menu_w) / 2);
    border.y = (Sint16) offset_y;
    border.w = (Uint16) menu_w;
    border.h = (Uint16) menu_h;

    draw_overlay_backdrop_dim(screen, menu_mix);
    menu_surface = cached_help_menu_surface(fonts, menu_w, menu_h);
    if (menu_surface) {
        blit_help_menu_surface_faded(screen, menu_surface, &border, menu_mix);
    } else {
        draw_help_menu_contents(screen, fonts, &border, menu_mix);
    }
}

bool chunk_list_contains(const int *chunks, size_t count, int chunk_index)
{
    size_t index;

    if (!chunks || chunk_index < 0) {
        return false;
    }
    for (index = 0; index < count; ++index) {
        if (chunks[index] == chunk_index) {
            return true;
        }
    }
    return false;
}

void chunk_list_add_unique(int *chunks, size_t *count, size_t capacity, int chunk_index)
{
    if (!chunks || !count || chunk_index < 0 || *count >= capacity ||
        chunk_list_contains(chunks, *count, chunk_index)) {
        return;
    }
    chunks[*count] = chunk_index;
    ++(*count);
}

void movie_update_ui_buffer_chunks(Movie *movie, int *chunks_to_draw, size_t *num_chunks_to_draw)
{
    int fresh_chunks[UI_BUFFER_CHUNK_CACHE_COUNT];
    size_t fresh_count = 0;
    size_t real_chunk_count = 0;
    size_t index;
    int current_chunk = -1;

    if (!movie || !chunks_to_draw || !num_chunks_to_draw) {
        return;
    }

    for (index = 0; index < UI_BUFFER_CHUNK_CACHE_COUNT; ++index) {
        fresh_chunks[index] = -1;
    }
    if (movie->header.frame_count > 0 && movie->chunk_index) {
        current_chunk = movie_chunk_for_frame(movie, movie->current_frame);
    }
    chunk_list_add_unique(fresh_chunks, &fresh_count, UI_BUFFER_CHUNK_CACHE_COUNT, current_chunk);
    if (movie->loaded_chunk >= 0) {
        chunk_list_add_unique(fresh_chunks, &fresh_count, UI_BUFFER_CHUNK_CACHE_COUNT, movie->loaded_chunk);
        ++real_chunk_count;
    }
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        if (movie->prefetched[index].chunk_index >= 0) {
            chunk_list_add_unique(
                fresh_chunks,
                &fresh_count,
                UI_BUFFER_CHUNK_CACHE_COUNT,
                movie->prefetched[index].chunk_index
            );
            ++real_chunk_count;
        }
    }

    if (real_chunk_count > 0 || movie->ui_buffer_chunk_count == 0) {
        movie->ui_buffer_chunk_count = 0;
        for (index = 0; index < fresh_count; ++index) {
            chunk_list_add_unique(
                movie->ui_buffer_chunks,
                &movie->ui_buffer_chunk_count,
                UI_BUFFER_CHUNK_CACHE_COUNT,
                fresh_chunks[index]
            );
        }
    }

    *num_chunks_to_draw = 0;
    for (index = 0; index < movie->ui_buffer_chunk_count; ++index) {
        chunk_list_add_unique(
            chunks_to_draw,
            num_chunks_to_draw,
            UI_BUFFER_CHUNK_CACHE_COUNT,
            movie->ui_buffer_chunks[index]
        );
    }
}

Uint16 progress_overlay_fill_color_at_y(const SDL_Rect *overlay, int y)
{
    Uint16 base = ui_theme()->progress_overlay_base;
    Uint16 body_top = blend_rgb565(base, UI_COLOR_WHITE, 28);
    Uint16 body_bottom = blend_rgb565(base, UI_COLOR_BLACK, 92);
    Uint16 gloss_top = blend_rgb565(base, UI_COLOR_WHITE, 104);
    Uint16 gloss_bottom = blend_rgb565(base, UI_COLOR_WHITE, 20);
    int denominator;
    int gloss_height;
    int row;

    if (!overlay || overlay->h == 0) {
        return base;
    }

    row = clamp_int(y - overlay->y, 0, overlay->h - 1);
    denominator = overlay->h > 1 ? overlay->h - 1 : 1;
    gloss_height = (overlay->h / 2) - 1;

    if (gloss_height > 1 && row >= 1 && row <= gloss_height) {
        return rgb565_lerp(gloss_top, gloss_bottom, row - 1, gloss_height - 1);
    }
    return rgb565_lerp(body_top, body_bottom, row, denominator);
}

void draw_progress_track(SDL_Surface *screen, const SDL_Rect *bar_back, const SDL_Rect *overlay)
{
    SDL_Rect outer;
    SDL_Rect body;
    SDL_Rect left_cap;
    SDL_Rect right_cap;
    SDL_Rect left_rim;
    SDL_Rect right_rim;
    SDL_Rect top_edge;
    SDL_Rect bottom_edge;
    SDL_Rect pixel;
    Uint16 cap_top;
    Uint16 cap_bottom;
    Uint16 rim_top;
    Uint16 rim_bottom;
    Uint16 edge_top;
    Uint16 edge_bottom;
    Uint16 surrounding_mid;
    Uint16 corner_top;
    Uint16 corner_bottom;
    Uint16 turn_top;
    Uint16 turn_bottom;

    if (!screen || !bar_back || bar_back->w == 0 || bar_back->h == 0) {
        return;
    }

    outer.x = (Sint16) (bar_back->x - 1);
    outer.y = (Sint16) (bar_back->y - 1);
    outer.w = (Uint16) (bar_back->w + 2);
    outer.h = (Uint16) (bar_back->h + 2);
    body.x = (Sint16) (bar_back->x + 1);
    body.y = bar_back->y;
    body.w = (Uint16) (bar_back->w - 2);
    body.h = bar_back->h;
    left_cap.x = bar_back->x;
    left_cap.y = bar_back->y;
    left_cap.w = 1;
    left_cap.h = bar_back->h;
    right_cap.x = (Sint16) (bar_back->x + bar_back->w - 1);
    right_cap.y = bar_back->y;
    right_cap.w = 1;
    right_cap.h = bar_back->h;
    left_rim.x = outer.x;
    left_rim.y = bar_back->y;
    left_rim.w = 1;
    left_rim.h = bar_back->h;
    right_rim.x = (Sint16) (bar_back->x + bar_back->w);
    right_rim.y = bar_back->y;
    right_rim.w = 1;
    right_rim.h = bar_back->h;
    top_edge.x = (Sint16) (bar_back->x + 1);
    top_edge.y = (Sint16) (bar_back->y - 1);
    top_edge.w = (Uint16) (bar_back->w - 2);
    top_edge.h = 1;
    bottom_edge.x = (Sint16) (bar_back->x + 1);
    bottom_edge.y = (Sint16) (bar_back->y + bar_back->h);
    bottom_edge.w = (Uint16) (bar_back->w - 2);
    bottom_edge.h = 1;

    cap_top = blend_rgb565(ui_theme()->progress_cap_top, ui_theme()->accent_mid, 58);
    cap_bottom = blend_rgb565(ui_theme()->progress_cap_bottom, ui_theme()->accent_deep, 42);
    rim_top = blend_rgb565(ui_theme()->progress_cap_top, ui_theme()->accent_hot, 126);
    rim_bottom = blend_rgb565(ui_theme()->progress_cap_bottom, ui_theme()->accent_mid, 86);
    edge_top = blend_rgb565(ui_theme()->progress_edge_top, ui_theme()->accent_hot, 70);
    edge_bottom = blend_rgb565(ui_theme()->progress_edge_bottom, ui_theme()->accent_mid, 48);
    surrounding_mid = progress_overlay_fill_color_at_y(overlay, bar_back->y + (bar_back->h / 2));
    corner_top = progress_overlay_fill_color_at_y(overlay, outer.y);
    corner_bottom = progress_overlay_fill_color_at_y(overlay, outer.y + outer.h - 1);
    turn_top = blend_rgb565(corner_top, edge_top, 112);
    turn_bottom = blend_rgb565(corner_bottom, edge_bottom, 96);

    fill_rect_rgb565(screen, &outer, surrounding_mid);
    draw_vertical_gradient(screen, &body, ui_theme()->progress_track_top, ui_theme()->progress_track_bottom);
    draw_vertical_gradient(screen, &left_cap, cap_top, cap_bottom);
    draw_vertical_gradient(screen, &right_cap, cap_top, cap_bottom);
    draw_vertical_gradient(screen, &left_rim, rim_top, rim_bottom);
    draw_vertical_gradient(screen, &right_rim, rim_top, rim_bottom);
    fill_rect_rgb565(screen, &top_edge, edge_top);
    fill_rect_rgb565(screen, &bottom_edge, edge_bottom);

    pixel.w = 1;
    pixel.h = 1;
    pixel.y = outer.y;
    pixel.x = outer.x;
    fill_rect_rgb565(screen, &pixel, corner_top);
    pixel.x = (Sint16) (outer.x + outer.w - 1);
    fill_rect_rgb565(screen, &pixel, corner_top);
    pixel.x = bar_back->x;
    fill_rect_rgb565(screen, &pixel, turn_top);
    pixel.x = (Sint16) (bar_back->x + bar_back->w - 1);
    fill_rect_rgb565(screen, &pixel, turn_top);

    pixel.y = (Sint16) (outer.y + outer.h - 1);
    pixel.x = outer.x;
    fill_rect_rgb565(screen, &pixel, corner_bottom);
    pixel.x = (Sint16) (outer.x + outer.w - 1);
    fill_rect_rgb565(screen, &pixel, corner_bottom);
    pixel.x = bar_back->x;
    fill_rect_rgb565(screen, &pixel, turn_bottom);
    pixel.x = (Sint16) (bar_back->x + bar_back->w - 1);
    fill_rect_rgb565(screen, &pixel, turn_bottom);
}

void draw_progress_buffer_range(SDL_Surface *screen, const SDL_Rect *rect)
{
    SDL_Rect line;
    Uint16 top;
    Uint16 bottom;

    if (!screen || !rect || rect->w == 0 || rect->h == 0) {
        return;
    }

    top = blend_rgb565(
        blend_rgb565(ui_theme()->progress_fill_top, ui_theme()->progress_fill_bottom, 144),
        UI_COLOR_BLACK,
        78
    );
    bottom = blend_rgb565(ui_theme()->progress_fill_bottom, UI_COLOR_BLACK, 116);
    draw_vertical_gradient(screen, rect, top, bottom);
    if (rect->h > 2) {
        line.x = rect->x;
        line.y = rect->y;
        line.w = rect->w;
        line.h = 1;
        fill_rect_rgb565_mix(screen, &line, ui_theme()->progress_fill_glow, 42);
        line.y = (Sint16) (rect->y + rect->h - 1);
        fill_rect_rgb565_mix(screen, &line, UI_COLOR_BLACK, 18);
    }
}

void draw_progress_overlay(SDL_Surface *screen, const SDL_Rect *overlay)
{
    Uint16 base = ui_theme()->progress_overlay_base;
    Uint16 body_top = blend_rgb565(base, UI_COLOR_WHITE, 28);
    Uint16 body_bottom = blend_rgb565(base, UI_COLOR_BLACK, 92);
    Uint16 gloss_top = blend_rgb565(base, UI_COLOR_WHITE, 104);
    Uint16 gloss_bottom = blend_rgb565(base, UI_COLOR_WHITE, 20);
    Uint16 rim_top = blend_rgb565(base, UI_COLOR_WHITE, 128);
    Uint16 rim_bottom = blend_rgb565(base, UI_COLOR_BLACK, 120);
    Uint16 aa_top = blend_rgb565(rim_top, body_top, 48);
    Uint16 aa_side;
    SDL_Rect line;
    SDL_Rect pixel;
    int row;
    int denominator;
    int gloss_height;

    if (!screen || !overlay || overlay->w < 8 || overlay->h < 8) {
        return;
    }

    denominator = overlay->h > 1 ? overlay->h - 1 : 1;
    for (row = 0; row < overlay->h; ++row) {
        int top_cut = row == 0 ? 1 : 0;

        line.x = (Sint16) (overlay->x + top_cut);
        line.y = (Sint16) (overlay->y + row);
        line.w = (Uint16) (overlay->w - (top_cut * 2));
        line.h = 1;
        fill_rect_rgb565(screen, &line, rgb565_lerp(body_top, body_bottom, row, denominator));
    }

    gloss_height = (overlay->h / 2) - 1;
    aa_side = blend_rgb565(rgb565_lerp(rim_top, gloss_bottom, 1, gloss_height), body_top, 42);
    for (row = 1; row <= gloss_height; ++row) {
        line.x = (Sint16) (overlay->x + 1);
        line.y = (Sint16) (overlay->y + row);
        line.w = (Uint16) (overlay->w - 2);
        line.h = 1;
        fill_rect_rgb565(screen, &line, rgb565_lerp(gloss_top, gloss_bottom, row - 1, gloss_height - 1));
    }

    line.x = (Sint16) (overlay->x + 1);
    line.y = overlay->y;
    line.w = (Uint16) (overlay->w - 2);
    line.h = 1;
    fill_rect_rgb565(screen, &line, rim_top);
    line.x = overlay->x;
    line.y = (Sint16) (overlay->y + overlay->h - 1);
    line.w = overlay->w;
    fill_rect_rgb565(screen, &line, rim_bottom);

    pixel.w = 1;
    pixel.h = 1;
    for (row = 1; row < overlay->h; ++row) {
        Uint16 edge = row <= gloss_height
            ? rgb565_lerp(rim_top, gloss_bottom, row, gloss_height)
            : rgb565_lerp(gloss_bottom, rim_bottom, row - gloss_height, overlay->h - gloss_height);
        pixel.y = (Sint16) (overlay->y + row);
        pixel.x = overlay->x;
        fill_rect_rgb565(screen, &pixel, edge);
        pixel.x = (Sint16) (overlay->x + overlay->w - 1);
        fill_rect_rgb565(screen, &pixel, edge);
    }
    pixel.y = overlay->y;
    pixel.x = (Sint16) (overlay->x + 1);
    fill_rect_rgb565(screen, &pixel, aa_top);
    pixel.x = (Sint16) (overlay->x + overlay->w - 2);
    fill_rect_rgb565(screen, &pixel, aa_top);
    pixel.y = (Sint16) (overlay->y + 1);
    pixel.x = overlay->x;
    fill_rect_rgb565(screen, &pixel, aa_side);
    pixel.x = (Sint16) (overlay->x + overlay->w - 1);
    fill_rect_rgb565(screen, &pixel, aa_side);
}

static uint32_t playback_rate_scaled_remaining_ms(uint32_t remaining_ms, const PlaybackRate *playback_rate)
{
    uint64_t scaled_ms;

    if (!playback_rate || playback_rate->numerator == 0U) {
        return remaining_ms;
    }

    scaled_ms = ((uint64_t) remaining_ms * playback_rate->denominator + (playback_rate->numerator / 2U)) /
        playback_rate->numerator;
    return scaled_ms > UINT32_MAX ? UINT32_MAX : (uint32_t) scaled_ms;
}

void draw_progress(
    SDL_Surface *screen,
    const Fonts *fonts,
    Movie *movie,
    uint32_t current_ms,
    bool paused,
    const PlaybackRate *playback_rate,
    uint32_t now_ms,
    const PointerState *pointer,
    int32_t pending_seek_ms,
    int32_t seek_badge_ms,
    uint32_t seek_badge_started_ms,
    uint32_t seek_badge_hide_elapsed_ms,
    SeekBarPreviewState *seek_preview,
    uint8_t preview_mix,
    uint8_t chrome_mix
)
{
    const int overlay_width = SCREEN_W - 10;
    SDL_Rect overlay = {chrome_centered_x_for_width(overlay_width), SCREEN_H - 28, overlay_width, 28};
    SDL_Rect bar_back = progress_bar_rect();
    SDL_Rect bar_front = bar_back;
    int ui_offset = ui_bar_hidden_offset_for_mix(chrome_mix);
    size_t chunk_draw_index;
    char current_text[24];
    char total_text[24];
    char left_text[56];
    char remaining_text[24];
    char right_text[32];
    char hover_text[24];
    uint32_t duration_ms = movie_duration_ms(movie);
    uint32_t remaining_ms = duration_ms > current_ms ? (duration_ms - current_ms) : 0;
    uint32_t scaled_remaining_ms = playback_rate_scaled_remaining_ms(remaining_ms, playback_rate);
    bool hover_bar = false;
    uint32_t hover_ms = 0;
    int preview_y;
    int hover_badge_y;

    overlay.y = (Sint16) (overlay.y + ui_offset);
    bar_back.y = (Sint16) (bar_back.y + ui_offset);
    bar_front.y = bar_back.y;
    hover_badge_y = bar_back.y - 24;

    draw_progress_overlay(screen, &overlay);

    draw_progress_track(screen, &bar_back, &overlay);

    if (movie->header.frame_count > 0 && movie->chunk_index) {
        int chunks_to_draw[UI_BUFFER_CHUNK_CACHE_COUNT];
        size_t num_chunks_to_draw = 0;

        movie_update_ui_buffer_chunks(movie, chunks_to_draw, &num_chunks_to_draw);

        for (chunk_draw_index = 0; chunk_draw_index < num_chunks_to_draw; ++chunk_draw_index) {
            const ChunkIndexEntry *entry = movie->chunk_index + chunks_to_draw[chunk_draw_index];
            int bar_right = bar_back.x + bar_back.w;
            uint32_t start_frame = entry->first_frame;
            uint32_t end_frame = start_frame + entry->frame_count;

            int x1 = bar_back.x + (int) (((uint64_t) bar_back.w * start_frame) / movie->header.frame_count);
            int x2 = bar_back.x + (int) (((uint64_t) bar_back.w * end_frame) / movie->header.frame_count);

            SDL_Rect prefetch_rect = bar_back;
            prefetch_rect.x = x1;
            prefetch_rect.w = x2 - x1;

            if (prefetch_rect.w <= 0) {
                prefetch_rect.w = 1;
            }
            if (prefetch_rect.x >= bar_right) {
                prefetch_rect.w = 0;
            } else if (prefetch_rect.x + prefetch_rect.w > bar_right) {
                prefetch_rect.w = (Uint16) (bar_right - prefetch_rect.x);
            }

            if (prefetch_rect.w > 0) {
                SDL_Rect sep = {prefetch_rect.x + prefetch_rect.w - 1, prefetch_rect.y, 1, prefetch_rect.h};
                draw_progress_buffer_range(screen, &prefetch_rect);
                if (prefetch_rect.w > 2) {
                    fill_rect_rgb565_mix(screen, &sep, UI_COLOR_BLACK, 28);
                }
            }
        }
    }
    bar_front.w = 0;
    if (seek_preview &&
        seek_preview->suppress_until_pointer_moves &&
        seek_preview->suppress_frame_index == movie->current_frame &&
        seek_preview->suppress_marker_x >= bar_back.x &&
        seek_preview->suppress_marker_x < bar_back.x + bar_back.w) {
        bar_front.w = (Uint16) (seek_preview->suppress_marker_x - bar_back.x + 1);
    } else if (duration_ms > 0) {
        bar_front.w = (Uint16) (((uint64_t) bar_back.w * current_ms) / duration_ms);
        if (movie->header.frame_count > 0 && bar_front.w == 0) {
            bar_front.w = 1;
        }
        if (movie->header.frame_count > 0 && movie->current_frame + 1U >= movie->header.frame_count) {
            bar_front.w = bar_back.w;
        }
    }
    if (bar_front.w > 0) {
        SDL_Rect glow = {bar_front.x, (Sint16) (bar_front.y + 1), bar_front.w, 1};
        SDL_Rect cap = {(Sint16) (bar_front.x + bar_front.w - 1), bar_front.y, 1, bar_front.h};
        draw_vertical_gradient(screen, &bar_front, ui_theme()->progress_fill_top, ui_theme()->progress_fill_bottom);
        fill_rect_rgb565(screen, &glow, ui_theme()->progress_fill_glow);
        fill_rect_rgb565(screen, &cap, ui_theme()->progress_fill_cap);
    }
    if (pointer && pointer->visible) {
        int marker_x = progress_bar_marker_x_from_pointer(&bar_back, pointer->x);
        SDL_Rect marker = {(Sint16) marker_x, (Sint16) (bar_back.y - 3), 1, 12};
        int preview_marker_x = marker_x;
        bool has_preview_anchor = false;
        bool use_cached_anchor = false;
        bool show_surface_preview = paused && seek_preview && seek_preview->surface;
        uint8_t surface_mix = preview_mix;

        fill_rect_rgb565(screen, &marker, UI_COLOR_WHITE);
        hover_bar = pointer->y >= overlay.y && pointer->y < overlay.y + overlay.h;
        if (!hover_bar && show_surface_preview && seek_preview->marker_x >= bar_back.x &&
            seek_preview->marker_x < bar_back.x + bar_back.w) {
            preview_marker_x = seek_preview->marker_x;
            has_preview_anchor = true;
            use_cached_anchor = true;
        } else if (!hover_bar && preview_mix > 0) {
            has_preview_anchor = true;
        }
        if ((hover_bar || has_preview_anchor) && duration_ms > 0 && preview_mix > 0) {
            int preview_x;
            int preview_offset = ((255 - preview_mix) * 4 + 127) / 255;
            hover_ms = hover_bar
                ? progress_bar_ms_for_marker(movie, &bar_back, preview_marker_x)
                : (use_cached_anchor
                    ? seek_preview->hover_ms
                    : progress_bar_ms_for_marker(movie, &bar_back, preview_marker_x));
            format_clock(hover_ms, hover_text, sizeof(hover_text));
            if (show_surface_preview) {
                if (seek_preview->surface_fade_pending) {
                    seek_preview->surface_started_ms = now_ms != 0U ? now_ms : 1U;
                    seek_preview->surface_fade_pending = false;
                    surface_mix = 0;
                } else if (seek_preview->surface_started_ms != 0U) {
                    uint32_t surface_elapsed_ms = now_ms - seek_preview->surface_started_ms;
                    surface_mix = ui_ease_out_cubic(surface_elapsed_ms, UI_TOOLTIP_ANIM_MS);
                    surface_mix = (uint8_t) (((uint32_t) surface_mix * surface_mix + 127U) / 255U);
                    if (surface_mix > preview_mix) {
                        surface_mix = preview_mix;
                    }
                }
                preview_x = clamp_int(
                    preview_marker_x - ((seek_preview->surface->w + 4) / 2),
                    0,
                    SCREEN_W - (seek_preview->surface->w + 4)
                );
                preview_y = hover_badge_y - seek_preview->surface->h - 13;
                if (preview_y < 0) {
                    preview_y = 0;
                }
                if (surface_mix > 0) {
                    int surface_offset = ((255 - surface_mix) * 4 + 127) / 255;
                    draw_seek_preview_panel(screen, seek_preview->surface, preview_x, preview_y + surface_offset, preview_marker_x, surface_mix);
                    seek_preview->surface_render_pending = false;
                }
            }
            if (preview_mix > 32) {
                draw_centered_text_badge(screen, fonts, preview_marker_x, hover_badge_y + preview_offset, hover_text);
            }
        }
    }
    format_clock(current_ms, current_text, sizeof(current_text));
    format_clock(duration_ms, total_text, sizeof(total_text));
    format_clock(scaled_remaining_ms, remaining_text, sizeof(remaining_text));
    {
        char *left_out = left_text;
        char *left_end = left_text + sizeof(left_text) - 1;

        left_out = append_text_bounded(left_out, left_end, current_text);
        left_out = append_text_bounded(left_out, left_end, " / ");
        append_text_bounded(left_out, left_end, total_text);
    }
    right_text[0] = '-';
    copy_truncated(right_text + 1, sizeof(right_text) - 1, remaining_text);
    if (chrome_mix > 48) {
        int label_inset = 10;
        int label_y = overlay.y + 3;
        draw_ui_label_ink_left(screen, fonts, overlay.x + label_inset, label_y, left_text);
        draw_ui_label_ink_right(
            screen,
            fonts,
            overlay.x + overlay.w - label_inset - 1,
            label_y,
            right_text
        );
    }
    (void) pending_seek_ms;
    draw_seek_delta_badge(screen, fonts, seek_badge_ms, seek_badge_started_ms, seek_badge_hide_elapsed_ms, now_ms);
}

void render_movie(
    SDL_Surface *screen,
    const Fonts *fonts,
    Movie *movie,
    bool paused,
    bool show_ui,
    bool help_menu_open,
    ScaleMode scale_mode,
    ScaleMorphState *scale_morph,
    VideoAlign video_align_x,
    VideoAlign video_align_y,
    const PlaybackRate *playback_rate,
    MemoryOverlayMode memory_overlay_mode,
    SubtitleSurfaceCache *subtitle_cache,
    size_t subtitle_font_index,
    bool subtitle_font_overlay_visible,
    int subtitle_size,
    SubtitlePlacement subtitle_placement,
    const char *movie_title_text,
    const char *movie_detail_text,
    const char *status_overlay_text,
    uint32_t status_overlay_started_ms,
    uint32_t status_overlay_until_ms,
    const ScreenshotPreviewState *screenshot_preview,
    SeekBarPreviewState *seek_preview,
    uint32_t now_ms,
    const PointerState *pointer,
    int32_t pending_seek_ms,
    int32_t seek_badge_ms,
    uint32_t seek_badge_started_ms,
    uint32_t seek_badge_hide_elapsed_ms,
    const PlaybackUiMixes *ui_mixes
)
{
    SDL_Rect src = {0, 0, SCREEN_W, SCREEN_H};
    SDL_Rect dst = {0, 0, SCREEN_W, SCREEN_H};
    int memory_right_limit = chrome_right_x_for_margin(8);
    uint32_t current_ms = movie_frame_time_ms(movie, movie->current_frame);
    uint8_t chrome_mix = ui_mixes ? ui_mixes->chrome : (show_ui ? 255 : 0);
    uint8_t help_menu_mix = ui_mixes ? ui_mixes->help_menu : (help_menu_open ? 255 : 0);
    uint8_t top_chrome_mix = help_menu_mix > 0 ? mix_product_u8(chrome_mix, (uint8_t) (255U - help_menu_mix)) : chrome_mix;
    bool help_menu_visible = help_menu_mix > 0 || help_menu_open;
    bool chrome_visible = chrome_mix > 0 || show_ui;
    bool top_chrome_visible = top_chrome_mix > 0 && chrome_visible;
    const SubtitleCue *subtitle_cue = active_subtitle_cue(movie, current_ms);
    const char *subtitle = subtitle_cue ? subtitle_cue->text : NULL;
    SubtitlePlacement effective_subtitle_placement = subtitle_normalize_placement(
        subtitle_placement,
        selected_subtitle_track_supports_auto_positioning(movie)
    );
    bool playback_badge_visible = top_chrome_visible;
    bool memory_badge_visible = top_chrome_visible && (memory_overlay_mode == MEMORY_OVERLAY_ALWAYS);
    bool cursor_visible = chrome_visible && !help_menu_visible && pointer && pointer->visible;

    scale_morph_current_rects(movie, scale_morph, scale_mode, video_align_x, video_align_y, now_ms, &src, &dst);
    draw_movie_frame_background_rects(screen, movie, &src, &dst);
    if (subtitle && subtitle_size >= 0) {
        SubtitleLayoutSpec subtitle_layout;

        if (subtitle_resolve_layout_spec(
                &dst,
                chrome_mix,
                effective_subtitle_placement,
                SUBTITLE_POS_BAR_BOTTOM,
                subtitle_cue,
                &subtitle_layout)) {
            draw_subtitle_cached(
                screen,
                fonts,
                subtitle_cache,
                subtitle,
                subtitle_font_index,
                subtitle_size,
                &subtitle_layout
            );
        }
    }
    if (subtitle_font_overlay_visible) {
        int preview_size = subtitle_size < 0 ? 0 : clamp_int(subtitle_size, 0, 1);
        SubtitleLayoutSpec preview_layout;

        if (subtitle_resolve_layout_spec(
                &dst,
                chrome_mix,
                subtitle_opposite_placement(effective_subtitle_placement),
                SUBTITLE_POS_BAR_TOP,
                NULL,
                &preview_layout)) {
            draw_subtitle(
                screen,
                fonts,
                subtitle_font_name_for_index(subtitle_font_index),
                subtitle_font_index,
                preview_size,
                &preview_layout
            );
        }
    }
    if (chrome_visible) {
        if (top_chrome_visible) {
            memory_right_limit = draw_status_badges(screen, fonts, &dst, scale_mode, playback_rate, ui_mixes, top_chrome_mix);
            if (playback_badge_visible) {
                draw_playback_badge(
                    screen,
                    &dst,
                    paused,
                    ui_mixes ? ui_mixes->playback_badge : 0,
                    ui_mixes ? ui_mixes->playback_press : 0,
                    top_chrome_mix
                );
            }
            {
                SDL_Rect playback_badge = playback_badge_rect(&dst);
                int status_left_x = playback_badge_visible
                    ? playback_badge.x + playback_badge.w + 6
                    : chrome_left_x_for_margin(8);

                draw_status_overlay_badge(
                    screen,
                    fonts,
                    status_left_x,
                    top_overlay_y_for_rect(&dst, 16),
                    status_overlay_text,
                    status_overlay_started_ms,
                    status_overlay_until_ms,
                    now_ms,
                    top_chrome_mix
                );
            }
        }
        draw_progress(
            screen,
            fonts,
            movie,
            current_ms,
            paused,
            playback_rate,
            now_ms,
            pointer,
            pending_seek_ms,
            seek_badge_ms,
            seek_badge_started_ms,
            seek_badge_hide_elapsed_ms,
            seek_preview,
            ui_mixes ? ui_mixes->seek_preview : 0,
            chrome_mix
        );
    }
    if (memory_badge_visible) {
        draw_memory_badge(screen, fonts, movie, &dst, memory_right_limit, playback_badge_visible, top_chrome_mix);
    }
    if (top_chrome_visible && ui_mixes && ui_mixes->title_strip > 0) {
        draw_playback_title_strip(
            screen,
            fonts,
            &dst,
            scale_mode,
            playback_rate,
            movie_title_text,
            movie_detail_text,
            mix_product_u8(ui_mixes->title_strip, top_chrome_mix)
        );
    }
    if (cursor_visible) {
        draw_cursor(screen, pointer->x, pointer->y);
    }
    if (help_menu_visible) {
        draw_help_menu(screen, fonts, help_menu_mix);
    }
    draw_screenshot_preview_osd(screen, fonts, screenshot_preview, now_ms);
    present_screen(screen);
}

bool should_publish_committed_seek_frame(Movie *movie, uint32_t frame_index, void *userdata)
{
    (void) movie;
    (void) frame_index;
    (void) userdata;
    return true;
}

bool render_committed_seek_frame(Movie *movie, uint32_t frame_index, void *userdata)
{
    CommittedSeekRenderContext *context = (CommittedSeekRenderContext *) userdata;
    uint32_t now_ms;

    if (!context || !context->screen || !context->fonts || !movie) {
        return true;
    }

    now_ms = monotonic_clock_now_ms();
    if (context->abort_on_input &&
        (playback_key_snapshot_new_press(&context->abort_key_snapshot) ||
            playback_wait_touchpad_click_pending(context->pointer))) {
        context->abort_requested = true;
        return false;
    }
    if (context->pointer) {
        bool pointer_click = pointer_update(context->pointer);

        if (pointer_click && context->abort_on_input) {
            context->abort_requested = true;
            return false;
        }
        if (context->pointer->moved ||
            context->pointer->down ||
            context->pointer->press_edge ||
            context->pointer->release_edge) {
            context->show_ui = true;
        }
        context->title_strip_active =
            context->show_ui &&
            context->pointer->visible &&
            context->pointer->y < PLAYBACK_TITLE_TOP_EDGE_PX;
    }
    movie->current_frame = frame_index;
    update_playback_ui_mixes(
        context->ui_transitions,
        context->ui_mixes,
        context->fonts,
        movie,
        context->scale_mode,
        context->scale_morph,
        context->video_align_x,
        context->video_align_y,
        context->playback_rate,
        context->show_ui,
        false,
        context->playback_press_target,
        context->playback_press_active,
        context->scale_press_active,
        context->speed_press_active,
        context->title_strip_active,
        context->pointer,
        now_ms
    );
    render_movie(
        context->screen,
        context->fonts,
        movie,
        context->paused,
        context->show_ui,
        false,
        context->scale_mode,
        context->scale_morph,
        context->video_align_x,
        context->video_align_y,
        context->playback_rate,
        context->memory_overlay_mode,
        context->subtitle_cache,
        context->subtitle_font_index,
        context->subtitle_font_overlay_visible,
        context->subtitle_size,
        context->subtitle_placement,
        context->movie_title_text,
        context->movie_detail_text,
        context->status_overlay_text,
        context->status_overlay_started_ms,
        context->status_overlay_until_ms,
        context->screenshot_preview,
        context->seek_preview,
        now_ms,
        context->pointer,
        context->pending_seek_ms,
        context->seek_badge_ms,
        context->seek_badge_started_ms,
        context->seek_badge_hide_elapsed_ms,
        context->ui_mixes
    );
    return true;
}

bool commit_seek_bar_preview_to_movie(Movie *movie, SeekBarPreviewState *preview, uint32_t target_frame)
{
    SeekPreviewDecodeJob *job;
    const ChunkIndexEntry *entry;
    uint32_t decoded_frame;
    size_t frame_pixels;
    size_t chunk_bytes_offset;

    if (!movie || !preview || !movie_uses_h264(movie)) {
        return false;
    }

    job = &preview->decode_job;
    if (!job->decoder || !job->chunk_storage ||
        !job->frame_offsets || !job->chunk_bytes || !job->pixels ||
        job->chunk_index < 0 ||
        (uint32_t) job->chunk_index >= movie->header.chunk_count ||
        (!job->active && !job->complete) ||
        preview->decoded_frame_index == UINT32_MAX) {
        return false;
    }
    if (job->active && target_frame > job->target_frame) {
        job->target_frame = target_frame;
    }
    if (job->active && !finish_seek_bar_preview_pending_frame(movie, preview)) {
        return false;
    }

    entry = movie->chunk_index + job->chunk_index;
    if (target_frame < entry->first_frame || target_frame >= entry->first_frame + entry->frame_count) {
        return false;
    }
    decoded_frame = preview->decoded_frame_index;
    if (decoded_frame < entry->first_frame ||
        decoded_frame >= entry->first_frame + entry->frame_count ||
        decoded_frame > target_frame) {
        return false;
    }
    if (!sync_h264_picture_params(movie, job->decoder, true)) {
        return false;
    }

    frame_pixels = (size_t) movie->header.video_width * movie->header.video_height;
    chunk_bytes_offset = (size_t) (job->chunk_bytes - job->chunk_storage);
    if (movie->h264.decoder) {
        if (movie->h264.decoder_initialized) {
            h264bsdShutdown(movie->h264.decoder);
        }
        h264bsdFree(movie->h264.decoder);
    }
    release_movie_chunk_storage(movie);
    free(movie->frame_offsets);

    movie->h264.decoder = job->decoder;
    movie->h264.decoder_initialized = job->decoder_initialized;
    if (!adopt_movie_chunk_storage(movie, &job->chunk_storage, job->chunk_storage_size)) {
        return false;
    }
    movie->frame_offsets = job->frame_offsets;
    movie->chunk_bytes = movie->chunk_storage + chunk_bytes_offset;
    movie->chunk_size = job->chunk_size;
    movie->loaded_chunk = job->chunk_index;
    movie->decoded_local_frame = (int) (decoded_frame - entry->first_frame);
    movie->h264.chunk_dirty = true;
    memcpy(movie->framebuffer, job->pixels, frame_pixels * sizeof(uint16_t));
    movie->current_frame = decoded_frame;

    job->decoder = NULL;
    job->decoder_initialized = false;
    job->chunk_storage = NULL;
    job->chunk_storage_size = 0;
    job->frame_offsets = NULL;
    job->chunk_bytes = NULL;
    job->chunk_size = 0;
    clear_seek_bar_preview_decode_job(preview);
    return true;
}

void draw_movie_frame_background(
    SDL_Surface *screen,
    Movie *movie,
    ScaleMode scale_mode,
    VideoAlign video_align_x,
    VideoAlign video_align_y,
    SDL_Rect *out_src,
    SDL_Rect *out_dst
)
{
    SDL_Rect src;
    SDL_Rect dst;

    if (!screen || !movie || !movie->frame_surface) {
        return;
    }
    compute_video_rects(movie, scale_mode, video_align_x, video_align_y, &src, &dst);
    if (out_src) {
        *out_src = src;
    }
    if (out_dst) {
        *out_dst = dst;
    }
    draw_movie_frame_background_rects(screen, movie, &src, &dst);
}

