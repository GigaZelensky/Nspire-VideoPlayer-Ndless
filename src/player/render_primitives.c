#include "player_internal.h"

SDL_Rect progress_bar_rect(void)
{
    const int width = SCREEN_W - 36;
    SDL_Rect rect = {chrome_centered_x_for_width(width), SCREEN_H - 15, width, 6};
    return rect;
}

uint32_t progress_bar_denominator(const SDL_Rect *bar)
{
    return bar && bar->w > 1 ? (uint32_t) (bar->w - 1) : 1U;
}

int progress_bar_marker_x_from_pointer(const SDL_Rect *bar, int pointer_x)
{
    if (!bar) {
        return 0;
    }
    return clamp_int(pointer_x, bar->x, bar->x + bar->w - 1);
}

uint32_t progress_bar_ms_for_marker(const Movie *movie, const SDL_Rect *bar, int marker_x)
{
    uint32_t duration_ms;

    if (!movie || !bar) {
        return 0;
    }
    duration_ms = movie_duration_ms(movie);
    if (duration_ms == 0) {
        return 0;
    }
    marker_x = progress_bar_marker_x_from_pointer(bar, marker_x);
    return (uint32_t) (((uint64_t) duration_ms * (uint32_t) (marker_x - bar->x)) / progress_bar_denominator(bar));
}

bool progress_bar_target_frame_for_marker(const Movie *movie, const SDL_Rect *bar, int marker_x, uint32_t *out_target_frame, uint32_t *out_target_ms)
{
    uint32_t target_ms;
    uint32_t target_frame;

    if (!movie || !bar || !out_target_frame || movie->header.frame_count == 0) {
        return false;
    }

    target_ms = progress_bar_ms_for_marker(movie, bar, marker_x);
    target_frame = movie_frames_from_ms(movie, target_ms);
    if (target_frame >= movie->header.frame_count) {
        target_frame = movie->header.frame_count - 1U;
    }
    *out_target_frame = target_frame;
    if (out_target_ms) {
        *out_target_ms = target_ms;
    }
    return true;
}


static const UiThemePalette g_ui_themes[UI_THEME_COUNT] = {
    {
        "DORFic",
        UI_RGB565(255, 136, 28),
        UI_RGB565(255, 82, 0),
        UI_RGB565(194, 34, 0),
        UI_RGB565(24, 24, 24),
        UI_RGB565(8, 8, 8),
        UI_RGB565(36, 36, 36),
        UI_RGB565(44, 40, 36),
        UI_RGB565(255, 232, 206),
        UI_RGB565(50, 30, 20),
        UI_RGB565(255, 142, 52),
        UI_RGB565(46, 30, 22),
        UI_RGB565(132, 40, 0),
        UI_RGB565(112, 36, 0),
        UI_RGB565(36, 30, 24),
        UI_RGB565(28, 24, 20),
        UI_RGB565(255, 142, 52),
        UI_RGB565(18, 16, 14),
        UI_RGB565(48, 40, 32),
        UI_RGB565(220, 198, 166),
        UI_RGB565(58, 38, 28),
        UI_RGB565(18, 16, 14),
        UI_RGB565(224, 210, 190),
        UI_RGB565(22, 20, 18),
        UI_RGB565(34, 28, 22),
        UI_RGB565(34, 28, 22),
        UI_RGB565(30, 26, 22),
        UI_RGB565(48, 44, 40),
        UI_RGB565(16, 14, 12),
        UI_RGB565(68, 58, 46),
        UI_RGB565(28, 20, 14),
        UI_RGB565(96, 62, 48),
        UI_RGB565(92, 48, 34),
        UI_RGB565(154, 48, 14),
        UI_RGB565(76, 28, 8),
        UI_RGB565(34, 10, 4),
        UI_RGB565(255, 142, 30),
        UI_RGB565(216, 38, 0),
        UI_RGB565(255, 96, 10),
        UI_RGB565(255, 174, 44),
        UI_RGB565(32, 32, 32),
        UI_RGB565(22, 20, 18),
        UI_RGB565(24, 22, 20),
        UI_RGB565(255, 210, 154),
        UI_RGB565(255, 132, 30),
        UI_RGB565(232, 72, 0),
        UI_RGB565(126, 36, 0),
        UI_RGB565(24, 20, 16)
    },
    {
        "Blue",
        UI_RGB565(184, 246, 255),
        UI_RGB565(72, 172, 232),
        UI_RGB565(16, 86, 168),
        UI_RGB565(20, 24, 28),
        UI_RGB565(6, 8, 10),
        UI_RGB565(34, 38, 40),
        UI_RGB565(42, 46, 48),
        UI_RGB565(232, 248, 255),
        UI_RGB565(32, 42, 46),
        UI_RGB565(156, 226, 245),
        UI_RGB565(34, 38, 40),
        UI_RGB565(18, 82, 132),
        UI_RGB565(28, 88, 130),
        UI_RGB565(34, 34, 34),
        UI_RGB565(28, 30, 32),
        UI_RGB565(156, 226, 245),
        UI_RGB565(16, 18, 20),
        UI_RGB565(42, 46, 48),
        UI_RGB565(210, 228, 236),
        UI_RGB565(44, 50, 52),
        UI_RGB565(16, 18, 20),
        UI_RGB565(210, 226, 232),
        UI_RGB565(22, 22, 24),
        UI_RGB565(30, 30, 32),
        UI_RGB565(30, 30, 32),
        UI_RGB565(26, 28, 30),
        UI_RGB565(46, 48, 50),
        UI_RGB565(14, 16, 18),
        UI_RGB565(60, 64, 66),
        UI_RGB565(24, 26, 28),
        UI_RGB565(88, 108, 118),
        UI_RGB565(54, 78, 92),
        UI_RGB565(88, 118, 132),
        UI_RGB565(44, 58, 70),
        UI_RGB565(18, 26, 34),
        UI_RGB565(158, 245, 255),
        UI_RGB565(42, 148, 218),
        UI_RGB565(112, 214, 252),
        UI_RGB565(210, 244, 255),
        UI_RGB565(32, 32, 34),
        UI_RGB565(20, 22, 24),
        UI_RGB565(22, 24, 26),
        UI_RGB565(198, 238, 255),
        UI_RGB565(132, 212, 255),
        UI_RGB565(78, 174, 230),
        UI_RGB565(42, 112, 176),
        UI_RGB565(18, 28, 38)
    },
    {
        "Green",
        UI_RGB565(210, 255, 150),
        UI_RGB565(112, 210, 92),
        UI_RGB565(38, 132, 54),
        UI_RGB565(20, 25, 20),
        UI_RGB565(6, 9, 6),
        UI_RGB565(34, 38, 34),
        UI_RGB565(42, 46, 40),
        UI_RGB565(236, 255, 220),
        UI_RGB565(36, 48, 34),
        UI_RGB565(186, 242, 126),
        UI_RGB565(34, 40, 34),
        UI_RGB565(44, 104, 42),
        UI_RGB565(52, 112, 48),
        UI_RGB565(34, 36, 32),
        UI_RGB565(28, 32, 26),
        UI_RGB565(186, 242, 126),
        UI_RGB565(14, 18, 14),
        UI_RGB565(42, 48, 38),
        UI_RGB565(214, 232, 198),
        UI_RGB565(46, 54, 42),
        UI_RGB565(16, 20, 16),
        UI_RGB565(214, 232, 198),
        UI_RGB565(22, 24, 20),
        UI_RGB565(30, 34, 28),
        UI_RGB565(30, 34, 28),
        UI_RGB565(26, 30, 24),
        UI_RGB565(46, 50, 44),
        UI_RGB565(14, 18, 14),
        UI_RGB565(60, 68, 54),
        UI_RGB565(24, 28, 22),
        UI_RGB565(90, 112, 74),
        UI_RGB565(60, 86, 52),
        UI_RGB565(92, 126, 76),
        UI_RGB565(46, 64, 38),
        UI_RGB565(18, 26, 14),
        UI_RGB565(206, 255, 118),
        UI_RGB565(66, 178, 70),
        UI_RGB565(134, 230, 80),
        UI_RGB565(230, 255, 156),
        UI_RGB565(32, 34, 30),
        UI_RGB565(20, 22, 18),
        UI_RGB565(24, 26, 22),
        UI_RGB565(218, 255, 170),
        UI_RGB565(154, 230, 92),
        UI_RGB565(80, 176, 58),
        UI_RGB565(38, 104, 38),
        UI_RGB565(18, 30, 18)
    },
    {
        "Red",
        UI_RGB565(255, 92, 92),
        UI_RGB565(232, 0, 0),
        UI_RGB565(150, 0, 0),
        UI_RGB565(24, 24, 24),
        UI_RGB565(8, 8, 8),
        UI_RGB565(36, 36, 36),
        UI_RGB565(44, 42, 42),
        UI_RGB565(255, 234, 230),
        UI_RGB565(42, 34, 34),
        UI_RGB565(255, 132, 132),
        UI_RGB565(36, 34, 34),
        UI_RGB565(168, 0, 0),
        UI_RGB565(136, 0, 0),
        UI_RGB565(36, 34, 34),
        UI_RGB565(28, 26, 26),
        UI_RGB565(255, 132, 132),
        UI_RGB565(18, 18, 18),
        UI_RGB565(46, 42, 42),
        UI_RGB565(238, 218, 214),
        UI_RGB565(54, 44, 44),
        UI_RGB565(18, 18, 18),
        UI_RGB565(238, 218, 214),
        UI_RGB565(22, 20, 20),
        UI_RGB565(34, 30, 30),
        UI_RGB565(34, 30, 30),
        UI_RGB565(30, 28, 28),
        UI_RGB565(52, 48, 48),
        UI_RGB565(16, 14, 14),
        UI_RGB565(70, 64, 64),
        UI_RGB565(26, 24, 24),
        UI_RGB565(120, 72, 72),
        UI_RGB565(88, 42, 42),
        UI_RGB565(134, 54, 54),
        UI_RGB565(76, 30, 30),
        UI_RGB565(34, 12, 12),
        UI_RGB565(255, 58, 58),
        UI_RGB565(180, 0, 0),
        UI_RGB565(255, 112, 112),
        UI_RGB565(255, 210, 210),
        UI_RGB565(32, 32, 32),
        UI_RGB565(22, 20, 20),
        UI_RGB565(24, 22, 22),
        UI_RGB565(255, 210, 210),
        UI_RGB565(255, 86, 86),
        UI_RGB565(225, 0, 0),
        UI_RGB565(132, 0, 0),
        UI_RGB565(30, 14, 14)
    }
};

UiThemeId g_ui_theme_id = UI_THEME_DORFIC;

UiThemeId ui_theme_clamp(int theme_id)
{
    if (theme_id < 0 || theme_id >= UI_THEME_COUNT) {
        return UI_THEME_DORFIC;
    }
    return (UiThemeId) theme_id;
}

const UiThemePalette *ui_theme(void)
{
    return &g_ui_themes[g_ui_theme_id];
}

const char *ui_theme_name(UiThemeId theme_id)
{
    return g_ui_themes[ui_theme_clamp((int) theme_id)].name;
}

void ui_set_theme(UiThemeId theme_id)
{
    g_ui_theme_id = ui_theme_clamp((int) theme_id);
}

UiThemeId ui_cycle_theme(void)
{
    g_ui_theme_id = (UiThemeId) ((g_ui_theme_id + 1) % UI_THEME_COUNT);
    return g_ui_theme_id;
}


bool surface_is_rgb565(const SDL_Surface *surface)
{
    return surface &&
        surface->format &&
        surface->format->BitsPerPixel == 16 &&
        surface->format->Rmask == 0xF800 &&
        surface->format->Gmask == 0x07E0 &&
        surface->format->Bmask == 0x001F;
}

void rgb565_to_rgb888(Uint16 color, int *r, int *g, int *b)
{
    int r5 = (color >> 11) & 0x1F;
    int g6 = (color >> 5) & 0x3F;
    int b5 = color & 0x1F;

    if (r) {
        *r = (r5 << 3) | (r5 >> 2);
    }
    if (g) {
        *g = (g6 << 2) | (g6 >> 4);
    }
    if (b) {
        *b = (b5 << 3) | (b5 >> 2);
    }
}

Uint32 map_rgb565(SDL_Surface *screen, Uint16 color)
{
    int r;
    int g;
    int b;

    rgb565_to_rgb888(color, &r, &g, &b);
    return SDL_MapRGB(screen->format, r, g, b);
}

Uint16 rgb565_lerp(Uint16 from, Uint16 to, int step, int steps)
{
    int ar = (from >> 11) & 0x1F;
    int ag = (from >> 5) & 0x3F;
    int ab = from & 0x1F;
    int br = (to >> 11) & 0x1F;
    int bg = (to >> 5) & 0x3F;
    int bb = to & 0x1F;
    int r;
    int g;
    int b;

    if (steps <= 0) {
        return to;
    }
    step = clamp_int(step, 0, steps);
    r = ar + (((br - ar) * step + (steps / 2)) / steps);
    g = ag + (((bg - ag) * step + (steps / 2)) / steps);
    b = ab + (((bb - ab) * step + (steps / 2)) / steps);
    return (Uint16) ((r << 11) | (g << 5) | b);
}

Uint16 blend_rgb565(Uint16 base, Uint16 overlay, int alpha)
{
    return rgb565_lerp(base, overlay, alpha, 255);
}

void fill_rect_rgb565(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color)
{
    if (!screen || !rect || rect->w == 0 || rect->h == 0) {
        return;
    }
    SDL_FillRect(screen, (SDL_Rect *) rect, map_rgb565(screen, color));
}

void fill_rect_rgb565_mix(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color, uint8_t mix)
{
    int x0;
    int y0;
    int x1;
    int y1;
    int y;
    bool locked = false;

    if (!screen || !rect || rect->w == 0 || rect->h == 0 || mix == 0) {
        return;
    }
    if (mix >= 255) {
        fill_rect_rgb565(screen, rect, color);
        return;
    }
    if (!surface_is_rgb565(screen)) {
        return;
    }

    x0 = clamp_int(rect->x, 0, screen->w);
    y0 = clamp_int(rect->y, 0, screen->h);
    x1 = clamp_int(rect->x + rect->w, 0, screen->w);
    y1 = clamp_int(rect->y + rect->h, 0, screen->h);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    if (SDL_MUSTLOCK(screen)) {
        if (SDL_LockSurface(screen) != 0) {
            return;
        }
        locked = true;
    }

    {
        Uint16 *pixels = (Uint16 *) screen->pixels;
        int pitch = screen->pitch / 2;
        for (y = y0; y < y1; ++y) {
            Uint16 *row = pixels + (y * pitch) + x0;
            int x;
            for (x = x0; x < x1; ++x) {
                *row = blend_rgb565(*row, color, mix);
                ++row;
            }
        }
    }

    if (locked) {
        SDL_UnlockSurface(screen);
    }
}

void blit_surface_rgb565_mix(SDL_Surface *screen, SDL_Surface *surface, const SDL_Rect *dst_rect, uint8_t mix)
{
    int dst_x0;
    int dst_y0;
    int dst_x1;
    int dst_y1;
    int src_x0;
    int src_y0;
    int y;
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
        return;
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
            Uint16 *dst_row = dst_pixels + (y * dst_pitch) + dst_x0;
            Uint16 *src_row = src_pixels + ((src_y0 + (y - dst_y0)) * src_pitch) + src_x0;
            int x;
            for (x = dst_x0; x < dst_x1; ++x) {
                *dst_row = blend_rgb565(*dst_row, *src_row, mix);
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

void dim_rect_rgb565(SDL_Surface *screen, const SDL_Rect *rect, int alpha)
{
    int x0;
    int y0;
    int x1;
    int y1;
    int y;
    bool locked = false;

    if (!screen || !rect || rect->w == 0 || rect->h == 0 || alpha <= 0) {
        return;
    }
    if (!surface_is_rgb565(screen)) {
        return;
    }

    x0 = clamp_int(rect->x, 0, screen->w);
    y0 = clamp_int(rect->y, 0, screen->h);
    x1 = clamp_int(rect->x + rect->w, 0, screen->w);
    y1 = clamp_int(rect->y + rect->h, 0, screen->h);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    alpha = clamp_int(alpha, 0, 255);
    if (SDL_MUSTLOCK(screen)) {
        if (SDL_LockSurface(screen) != 0) {
            return;
        }
        locked = true;
    }

    {
        Uint16 *pixels = (Uint16 *) screen->pixels;
        int pitch = screen->pitch / 2;
        for (y = y0; y < y1; ++y) {
            Uint16 *row = pixels + (y * pitch) + x0;
            int x;
            for (x = x0; x < x1; ++x) {
                *row = blend_rgb565(*row, UI_COLOR_BLACK, alpha);
                ++row;
            }
        }
    }

    if (locked) {
        SDL_UnlockSurface(screen);
    }
}

void draw_vertical_gradient(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color_top, Uint16 color_bottom)
{
    int x0;
    int y0;
    int x1;
    int y1;
    int y;
    int denominator;
    bool locked = false;

    if (!screen || !rect || rect->w == 0 || rect->h == 0) {
        return;
    }

    x0 = clamp_int(rect->x, 0, screen->w);
    y0 = clamp_int(rect->y, 0, screen->h);
    x1 = clamp_int(rect->x + rect->w, 0, screen->w);
    y1 = clamp_int(rect->y + rect->h, 0, screen->h);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    denominator = rect->h > 1 ? rect->h - 1 : 1;
    if (!surface_is_rgb565(screen)) {
        for (y = y0; y < y1; ++y) {
            SDL_Rect line = {(Sint16) x0, (Sint16) y, (Uint16) (x1 - x0), 1};
            fill_rect_rgb565(screen, &line, rgb565_lerp(color_top, color_bottom, y - rect->y, denominator));
        }
        return;
    }

    if (SDL_MUSTLOCK(screen)) {
        if (SDL_LockSurface(screen) != 0) {
            return;
        }
        locked = true;
    }

    {
        Uint16 *pixels = (Uint16 *) screen->pixels;
        int pitch = screen->pitch / 2;
        for (y = y0; y < y1; ++y) {
            Uint16 color = rgb565_lerp(color_top, color_bottom, y - rect->y, denominator);
            Uint16 *row = pixels + (y * pitch) + x0;
            int x;
            for (x = x0; x < x1; ++x) {
                *row++ = color;
            }
        }
    }

    if (locked) {
        SDL_UnlockSurface(screen);
    }
}

void draw_rect_outline_rgb565(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color)
{
    SDL_Rect line;

    if (!screen || !rect || rect->w == 0 || rect->h == 0) {
        return;
    }

    line.x = rect->x;
    line.y = rect->y;
    line.w = rect->w;
    line.h = 1;
    fill_rect_rgb565(screen, &line, color);
    line.y = (Sint16) (rect->y + rect->h - 1);
    fill_rect_rgb565(screen, &line, color);
    line.x = rect->x;
    line.y = rect->y;
    line.w = 1;
    line.h = rect->h;
    fill_rect_rgb565(screen, &line, color);
    line.x = (Sint16) (rect->x + rect->w - 1);
    fill_rect_rgb565(screen, &line, color);
}

void cut_rect_corners(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color)
{
    SDL_Rect pixel;

    if (!screen || !rect || rect->w < 2 || rect->h < 2) {
        return;
    }
    pixel.w = 1;
    pixel.h = 1;
    pixel.x = rect->x;
    pixel.y = rect->y;
    fill_rect_rgb565(screen, &pixel, color);
    pixel.x = (Sint16) (rect->x + rect->w - 1);
    fill_rect_rgb565(screen, &pixel, color);
    pixel.x = rect->x;
    pixel.y = (Sint16) (rect->y + rect->h - 1);
    fill_rect_rgb565(screen, &pixel, color);
    pixel.x = (Sint16) (rect->x + rect->w - 1);
    fill_rect_rgb565(screen, &pixel, color);
}

Uint16 picker_background_color_at_y(int y)
{
    return rgb565_lerp(UI_COLOR_BG_TOP, UI_COLOR_BG_BOTTOM, clamp_int(y, 0, SCREEN_H - 1), SCREEN_H - 1);
}

Uint16 picker_background_color_at_y_mix(int y, uint8_t mix)
{
    return rgb565_lerp(UI_COLOR_BLACK, picker_background_color_at_y(y), mix, 255);
}

int ui_mix_int(int from, int to, uint8_t mix)
{
    return from + (((to - from) * (int) mix + 127) / 255);
}

Sint16 ui_mix_sint16(int from, int to, uint8_t mix)
{
    return (Sint16) ui_mix_int(from, to, mix);
}

Uint16 ui_mix_uint16(int from, int to, uint8_t mix)
{
    return (Uint16) clamp_int(ui_mix_int(from, to, mix), 0, 32767);
}

void fill_picker_selection_line(
    SDL_Surface *screen,
    const SDL_Rect *line,
    Uint16 target_color,
    uint8_t mix
)
{
    fill_rect_rgb565(
        screen,
        line,
        rgb565_lerp(picker_background_color_at_y(line->y), target_color, mix, 255)
    );
}

Uint16 control_outline_color(Uint16 base_color, uint8_t selected_mix)
{
    return rgb565_lerp(
        blend_rgb565(base_color, UI_COLOR_BLACK, 120),
        UI_COLOR_ACCENT,
        selected_mix,
        255
    );
}

void draw_picker_selection_panel_mix(
    SDL_Surface *screen,
    const SDL_Rect *rect,
    Uint16 base_color,
    uint8_t selection_mix
)
{
    int row;
    int denominator;
    int gloss_rows;
    uint8_t highlight_mix;
    Uint16 top;
    Uint16 bottom;
    Uint16 outline;
    SDL_Rect line;
    SDL_Rect side;

    if (!screen || !rect || rect->w < 2 || rect->h < 2 || selection_mix == 0) {
        return;
    }

    highlight_mix = (uint8_t) clamp_int(
        selection_mix + (((255 - selection_mix) * (int) selection_mix + 768) / 1536),
        0,
        255
    );
    top = blend_rgb565(base_color, UI_COLOR_ACCENT_HOT, 74);
    top = blend_rgb565(top, UI_COLOR_WHITE, (48 * highlight_mix + 127) / 255);
    bottom = blend_rgb565(base_color, UI_COLOR_BLACK, 72);
    denominator = rect->h > 1 ? rect->h - 1 : 1;

    line.x = rect->x;
    line.w = rect->w;
    line.h = 1;
    for (row = 0; row < rect->h; ++row) {
        line.y = (Sint16) (rect->y + row);
        fill_picker_selection_line(
            screen,
            &line,
            rgb565_lerp(top, bottom, row, denominator),
            selection_mix
        );
    }

    if (rect->h >= 4 && rect->w >= 4) {
        gloss_rows = rect->h >= 48 ? rect->h / 4 : (rect->h / 2) - 1;
        line.x = (Sint16) (rect->x + 1);
        line.w = (Uint16) (rect->w - 2);
        for (row = 1; row < gloss_rows; ++row) {
            int alpha = clamp_int(130 - (row * 9), 18, 130);

            line.y = (Sint16) (rect->y + row);
            fill_picker_selection_line(
                screen,
                &line,
                blend_rgb565(base_color, UI_COLOR_WHITE, alpha),
                highlight_mix
            );
        }
    }

    if (rect->w > 2) {
        line.x = (Sint16) (rect->x + 1);
        line.y = (Sint16) (rect->y + 1);
        line.w = (Uint16) (rect->w - 2);
        line.h = 1;
        fill_picker_selection_line(
            screen,
            &line,
            blend_rgb565(base_color, UI_COLOR_ACCENT_HOT, 146),
            highlight_mix
        );
        line.y = (Sint16) (rect->y + rect->h - 1);
        fill_picker_selection_line(
            screen,
            &line,
            blend_rgb565(base_color, UI_COLOR_BLACK, 150),
            selection_mix
        );
    }

    outline = control_outline_color(base_color, 255);
    if (rect->w > 2) {
        line.x = (Sint16) (rect->x + 1);
        line.y = rect->y;
        line.w = (Uint16) (rect->w - 2);
        line.h = 1;
        fill_picker_selection_line(screen, &line, outline, selection_mix);
        line.y = (Sint16) (rect->y + rect->h - 1);
        fill_picker_selection_line(screen, &line, outline, selection_mix);
    }
    if (rect->h > 2) {
        side.x = rect->x;
        side.y = (Sint16) (rect->y + 1);
        side.w = 1;
        side.h = (Uint16) (rect->h - 2);
        fill_picker_selection_line(screen, &side, outline, selection_mix);
        side.x = (Sint16) (rect->x + rect->w - 1);
        fill_picker_selection_line(screen, &side, outline, selection_mix);
    }
}

void draw_glass_panel_mix(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, uint8_t selected_mix)
{
    SDL_Rect gloss;
    SDL_Rect line;
    Uint16 top;
    Uint16 bottom;
    Uint16 gloss_top;
    Uint16 gloss_bottom;

    if (!screen || !rect || rect->w == 0 || rect->h == 0) {
        return;
    }

    top = blend_rgb565(base_color, UI_COLOR_WHITE, ui_mix_int(28, 48, selected_mix));
    bottom = blend_rgb565(base_color, UI_COLOR_BLACK, ui_mix_int(92, 70, selected_mix));
    draw_vertical_gradient(screen, rect, top, bottom);

    if (rect->h >= 4 && rect->w >= 4) {
        int gloss_height = rect->h >= 48 ? (rect->h / 4) : ((rect->h / 2) - 1);

        gloss.x = (Sint16) (rect->x + 1);
        gloss.y = (Sint16) (rect->y + 1);
        gloss.w = (Uint16) (rect->w - 2);
        gloss.h = (Uint16) gloss_height;
        if (gloss.h > 0) {
            gloss_top = blend_rgb565(
                base_color,
                UI_COLOR_WHITE,
                ui_mix_int(rect->h >= 48 ? 62 : 104, 150, selected_mix)
            );
            gloss_bottom = blend_rgb565(
                base_color,
                UI_COLOR_WHITE,
                ui_mix_int(rect->h >= 48 ? 8 : 20, 34, selected_mix)
            );
            draw_vertical_gradient(screen, &gloss, gloss_top, gloss_bottom);
        }
    }

    line.x = (Sint16) (rect->x + 1);
    line.y = (Sint16) (rect->y + 1);
    line.w = rect->w > 2 ? (Uint16) (rect->w - 2) : rect->w;
    line.h = 1;
    fill_rect_rgb565(screen, &line, blend_rgb565(base_color, UI_COLOR_WHITE, ui_mix_int(128, 210, selected_mix)));
    line.y = (Sint16) (rect->y + rect->h - 1);
    fill_rect_rgb565(screen, &line, blend_rgb565(base_color, UI_COLOR_BLACK, 170));

    draw_rect_outline_rgb565(
        screen,
        rect,
        control_outline_color(base_color, selected_mix)
    );
}

void draw_glass_panel(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, bool is_selected)
{
    draw_glass_panel_mix(screen, rect, base_color, is_selected ? 255 : 0);
}

void draw_glass_panel_faded(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, bool is_selected, uint8_t mix)
{
    SDL_Surface *composite;

    if (!screen || !rect || rect->w == 0 || rect->h == 0 || mix == 0) {
        return;
    }
    if (mix >= 255) {
        draw_glass_panel(screen, rect, base_color, is_selected);
        return;
    }
    if (rect->x >= 0 && rect->y >= 0 &&
        rect->x + rect->w <= screen->w && rect->y + rect->h <= screen->h) {
        SDL_Rect source = *rect;
        SDL_Rect local = {0, 0, rect->w, rect->h};

        composite = create_rgb565_surface(rect->w, rect->h);
        if (composite) {
            SDL_BlitSurface(screen, &source, composite, NULL);
            draw_glass_panel(composite, &local, base_color, is_selected);
            blit_surface_rgb565_mix(screen, composite, rect, mix);
            SDL_FreeSurface(composite);
            return;
        }
    }
    draw_glass_panel(screen, rect, rgb565_lerp(UI_COLOR_BLACK, base_color, mix, 255), is_selected);
}

int soft_panel_inset_for_row(int row, int height)
{
    if (height >= 10 && (row == 0 || row == height - 1)) {
        return 2;
    }
    if (height >= 6 && (row == 0 || row == height - 1)) {
        return 1;
    }
    if (height >= 6 && (row == 1 || row == height - 2)) {
        return 1;
    }
    return 0;
}

int soft_panel_top_inset_for_row(int row, int height)
{
    if (height >= 10 && row == 0) {
        return 2;
    }
    if (height >= 6 && row <= 1) {
        return 1;
    }
    return 0;
}

void fill_soft_panel_backplate(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color)
{
    int row;
    SDL_Rect line;

    if (!screen || !rect || rect->w < 4 || rect->h < 4) {
        return;
    }

    line.h = 1;
    for (row = 0; row < rect->h; ++row) {
        int inset = soft_panel_inset_for_row(row, rect->h);

        line.x = (Sint16) (rect->x + inset);
        line.y = (Sint16) (rect->y + row);
        line.w = (Uint16) (rect->w - (inset * 2));
        fill_rect_rgb565(screen, &line, color);
    }
}

void draw_soft_glass_panel_mix(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, uint8_t selected_mix)
{
    int row;
    int denominator;
    int gloss_rows;
    Uint16 top;
    Uint16 bottom;
    Uint16 outline;
    SDL_Rect line;
    SDL_Rect side;
    SDL_Rect pixel;

    if (!screen || !rect || rect->w < 4 || rect->h < 4) {
        return;
    }

    top = blend_rgb565(base_color, UI_COLOR_WHITE, ui_mix_int(32, 54, selected_mix));
    bottom = blend_rgb565(base_color, UI_COLOR_BLACK, ui_mix_int(92, 66, selected_mix));
    denominator = rect->h > 1 ? rect->h - 1 : 1;
    for (row = 0; row < rect->h; ++row) {
        int inset = soft_panel_inset_for_row(row, rect->h);
        line.x = (Sint16) (rect->x + inset);
        line.y = (Sint16) (rect->y + row);
        line.w = (Uint16) (rect->w - (inset * 2));
        line.h = 1;
        fill_rect_rgb565(screen, &line, rgb565_lerp(top, bottom, row, denominator));
    }

    gloss_rows = rect->h >= 10 ? rect->h / 3 : rect->h / 2;
    for (row = 1; row < gloss_rows; ++row) {
        int inset = soft_panel_inset_for_row(row, rect->h) + 1;
        int alpha = ui_mix_int(84 - (row * 6), 128 - (row * 8), selected_mix);

        if (alpha <= 0) {
            break;
        }
        line.x = (Sint16) (rect->x + inset);
        line.y = (Sint16) (rect->y + row);
        line.w = (Uint16) (rect->w - (inset * 2));
        line.h = 1;
        fill_rect_rgb565(
            screen,
            &line,
            blend_rgb565(base_color, UI_COLOR_WHITE, alpha)
        );
    }

    outline = control_outline_color(base_color, selected_mix);
    line.x = (Sint16) (rect->x + 2);
    line.y = rect->y;
    line.w = (Uint16) (rect->w - 4);
    line.h = 1;
    fill_rect_rgb565(screen, &line, outline);
    line.y = (Sint16) (rect->y + rect->h - 1);
    fill_rect_rgb565(screen, &line, outline);

    side.x = rect->x;
    side.y = (Sint16) (rect->y + 2);
    side.w = 1;
    side.h = (Uint16) (rect->h - 4);
    fill_rect_rgb565(screen, &side, outline);
    side.x = (Sint16) (rect->x + rect->w - 1);
    fill_rect_rgb565(screen, &side, outline);

    pixel.w = 1;
    pixel.h = 1;
    pixel.x = (Sint16) (rect->x + 1);
    pixel.y = (Sint16) (rect->y + 1);
    fill_rect_rgb565(screen, &pixel, outline);
    pixel.x = (Sint16) (rect->x + rect->w - 2);
    fill_rect_rgb565(screen, &pixel, outline);
    pixel.x = (Sint16) (rect->x + 1);
    pixel.y = (Sint16) (rect->y + rect->h - 2);
    fill_rect_rgb565(screen, &pixel, outline);
    pixel.x = (Sint16) (rect->x + rect->w - 2);
    fill_rect_rgb565(screen, &pixel, outline);
}

void draw_soft_glass_panel_top_mix(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, uint8_t selected_mix)
{
    int row;
    int denominator;
    int gloss_rows;
    Uint16 top;
    Uint16 bottom;
    Uint16 outline;
    SDL_Rect line;
    SDL_Rect side;
    SDL_Rect pixel;

    if (!screen || !rect || rect->w < 4 || rect->h < 4) {
        return;
    }

    top = blend_rgb565(base_color, UI_COLOR_WHITE, ui_mix_int(32, 54, selected_mix));
    bottom = blend_rgb565(base_color, UI_COLOR_BLACK, ui_mix_int(92, 66, selected_mix));
    denominator = rect->h > 1 ? rect->h - 1 : 1;
    for (row = 0; row < rect->h; ++row) {
        int inset = soft_panel_top_inset_for_row(row, rect->h);

        line.x = (Sint16) (rect->x + inset);
        line.y = (Sint16) (rect->y + row);
        line.w = (Uint16) (rect->w - (inset * 2));
        line.h = 1;
        fill_rect_rgb565(screen, &line, rgb565_lerp(top, bottom, row, denominator));
    }

    gloss_rows = rect->h >= 10 ? rect->h / 3 : rect->h / 2;
    for (row = 1; row < gloss_rows; ++row) {
        int inset = soft_panel_top_inset_for_row(row, rect->h) + 1;
        int alpha = ui_mix_int(84 - (row * 6), 128 - (row * 8), selected_mix);

        if (alpha <= 0) {
            break;
        }
        line.x = (Sint16) (rect->x + inset);
        line.y = (Sint16) (rect->y + row);
        line.w = (Uint16) (rect->w - (inset * 2));
        line.h = 1;
        fill_rect_rgb565(screen, &line, blend_rgb565(base_color, UI_COLOR_WHITE, alpha));
    }

    outline = control_outline_color(base_color, selected_mix);
    line.x = (Sint16) (rect->x + 2);
    line.y = rect->y;
    line.w = (Uint16) (rect->w - 4);
    line.h = 1;
    fill_rect_rgb565(screen, &line, outline);
    line.x = rect->x;
    line.y = (Sint16) (rect->y + rect->h - 1);
    line.w = rect->w;
    fill_rect_rgb565(screen, &line, outline);

    side.x = rect->x;
    side.y = (Sint16) (rect->y + 2);
    side.w = 1;
    side.h = (Uint16) (rect->h - 2);
    fill_rect_rgb565(screen, &side, outline);
    side.x = (Sint16) (rect->x + rect->w - 1);
    fill_rect_rgb565(screen, &side, outline);

    pixel.w = 1;
    pixel.h = 1;
    pixel.x = (Sint16) (rect->x + 1);
    pixel.y = (Sint16) (rect->y + 1);
    fill_rect_rgb565(screen, &pixel, outline);
    pixel.x = (Sint16) (rect->x + rect->w - 2);
    fill_rect_rgb565(screen, &pixel, outline);
}

void draw_soft_glass_panel_body_from_y(
    SDL_Surface *screen,
    const SDL_Rect *panel,
    int body_y,
    Uint16 base_color
)
{
    int row;
    int body_start_row;
    int body_height;
    int denominator;
    Uint16 top;
    Uint16 bottom;
    SDL_Rect line;

    if (!screen || !panel || panel->w < 4 || panel->h < 4) {
        return;
    }

    body_start_row = clamp_int(body_y - panel->y, 0, panel->h - 1);
    body_height = panel->h - body_start_row;
    denominator = body_height > 1 ? body_height - 1 : 1;
    top = blend_rgb565(base_color, UI_COLOR_WHITE, 32);
    bottom = blend_rgb565(base_color, UI_COLOR_BLACK, 92);
    line.h = 1;
    for (row = body_start_row; row < panel->h; ++row) {
        int body_row = row - body_start_row;
        int inset = soft_panel_inset_for_row(row, panel->h);

        line.x = (Sint16) (panel->x + inset);
        line.y = (Sint16) (panel->y + row);
        line.w = (Uint16) (panel->w - (inset * 2));
        fill_rect_rgb565(screen, &line, rgb565_lerp(top, bottom, body_row, denominator));
    }
}

void draw_soft_glass_panel_rim(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, uint8_t selected_mix)
{
    Uint16 outline;
    SDL_Rect line;
    SDL_Rect side;
    SDL_Rect pixel;

    if (!screen || !rect || rect->w < 4 || rect->h < 4 || selected_mix == 0) {
        return;
    }

    outline = control_outline_color(base_color, selected_mix);
    line.x = (Sint16) (rect->x + 2);
    line.y = rect->y;
    line.w = (Uint16) (rect->w - 4);
    line.h = 1;
    fill_rect_rgb565(screen, &line, outline);
    line.y = (Sint16) (rect->y + rect->h - 1);
    fill_rect_rgb565(screen, &line, outline);

    side.x = rect->x;
    side.y = (Sint16) (rect->y + 2);
    side.w = 1;
    side.h = (Uint16) (rect->h - 4);
    fill_rect_rgb565(screen, &side, outline);
    side.x = (Sint16) (rect->x + rect->w - 1);
    fill_rect_rgb565(screen, &side, outline);

    pixel.w = 1;
    pixel.h = 1;
    pixel.x = (Sint16) (rect->x + 1);
    pixel.y = (Sint16) (rect->y + 1);
    fill_rect_rgb565(screen, &pixel, outline);
    pixel.x = (Sint16) (rect->x + rect->w - 2);
    fill_rect_rgb565(screen, &pixel, outline);
    pixel.x = (Sint16) (rect->x + 1);
    pixel.y = (Sint16) (rect->y + rect->h - 2);
    fill_rect_rgb565(screen, &pixel, outline);
    pixel.x = (Sint16) (rect->x + rect->w - 2);
    fill_rect_rgb565(screen, &pixel, outline);
}

void draw_soft_glass_panel(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, bool is_selected)
{
    draw_soft_glass_panel_mix(screen, rect, base_color, is_selected ? 255 : 0);
}

void draw_ui_label(SDL_Surface *screen, const Fonts *fonts, int x, int y, const char *label)
{
    if (!screen || !fonts || !label) {
        return;
    }
    nSDL_DrawString(screen, fonts->white, x, y, "%s", label);
}

typedef struct {
    nSDL_Font *font;
    bool valid[NSP_FONT_NUMCHARS];
    int min_x[NSP_FONT_NUMCHARS];
    int max_x[NSP_FONT_NUMCHARS];
} FontInkBoundsCache;

bool surface_pixel_has_ink(SDL_Surface *surface, int x, int y)
{
    Uint8 *pixel;
    Uint32 value = 0;
    Uint32 color_key = 0;
    bool has_color_key = false;

    if (!surface || x < 0 || y < 0 || x >= surface->w || y >= surface->h) {
        return false;
    }

    if ((surface->flags & SDL_SRCCOLORKEY) != 0) {
        color_key = surface->format->colorkey;
        has_color_key = true;
    }

    pixel = (Uint8 *) surface->pixels + (y * surface->pitch) + (x * surface->format->BytesPerPixel);
    switch (surface->format->BytesPerPixel) {
        case 1:
            value = *pixel;
            break;
        case 2:
            value = *(Uint16 *) pixel;
            break;
        case 3:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
            value = ((Uint32) pixel[0] << 16) | ((Uint32) pixel[1] << 8) | pixel[2];
#else
            value = pixel[0] | ((Uint32) pixel[1] << 8) | ((Uint32) pixel[2] << 16);
#endif
            break;
        case 4:
            value = *(Uint32 *) pixel;
            break;
        default:
            return false;
    }
    if (has_color_key && value == color_key) {
        return false;
    }
    return value != 0;
}

bool font_char_ink_bounds(nSDL_Font *font, unsigned char ch, int *out_min_x, int *out_max_x)
{
    static FontInkBoundsCache cache;
    SDL_Surface *glyph;
    int scan_w;
    int min_x;
    int max_x;
    int x;
    int y;
    bool locked = false;

    if (!font || !out_min_x || !out_max_x) {
        return false;
    }
    if (cache.font != font) {
        memset(&cache, 0, sizeof(cache));
        cache.font = font;
    }
    if (cache.valid[ch]) {
        *out_min_x = cache.min_x[ch];
        *out_max_x = cache.max_x[ch];
        return cache.max_x[ch] >= cache.min_x[ch];
    }

    glyph = font->chars[ch];
    scan_w = font->monospaced ? NSP_FONT_WIDTH : font->char_width[ch];
    if (!glyph || scan_w <= 0) {
        cache.valid[ch] = true;
        cache.min_x[ch] = 0;
        cache.max_x[ch] = -1;
        *out_min_x = 0;
        *out_max_x = -1;
        return false;
    }
    if (scan_w > glyph->w) {
        scan_w = glyph->w;
    }

    if (SDL_MUSTLOCK(glyph)) {
        if (SDL_LockSurface(glyph) != 0) {
            return false;
        }
        locked = true;
    }

    min_x = scan_w;
    max_x = -1;
    for (y = 0; y < glyph->h; ++y) {
        for (x = 0; x < scan_w; ++x) {
            if (surface_pixel_has_ink(glyph, x, y)) {
                if (x < min_x) {
                    min_x = x;
                }
                if (x > max_x) {
                    max_x = x;
                }
            }
        }
    }

    if (locked) {
        SDL_UnlockSurface(glyph);
    }

    cache.valid[ch] = true;
    cache.min_x[ch] = min_x;
    cache.max_x[ch] = max_x;
    *out_min_x = min_x;
    *out_max_x = max_x;
    return max_x >= min_x;
}

int font_char_advance_width(nSDL_Font *font, unsigned char ch)
{
    int width;

    if (!font) {
        return 0;
    }
    width = font->monospaced ? NSP_FONT_WIDTH : font->char_width[ch];
    if (width < 0) {
        width = 0;
    }
    return width + font->hspacing;
}

bool font_string_ink_bounds(nSDL_Font *font, const char *text, int *out_min_x, int *out_max_x)
{
    int pen_x = 0;
    int min_x = 32767;
    int max_x = -32768;
    bool has_ink = false;

    if (!font || !text || !out_min_x || !out_max_x) {
        return false;
    }

    while (*text) {
        unsigned char ch = (unsigned char) *text++;
        int char_min_x;
        int char_max_x;

        if (font_char_ink_bounds(font, ch, &char_min_x, &char_max_x)) {
            int glyph_min = pen_x + char_min_x;
            int glyph_max = pen_x + char_max_x;
            if (glyph_min < min_x) {
                min_x = glyph_min;
            }
            if (glyph_max > max_x) {
                max_x = glyph_max;
            }
            has_ink = true;
        }
        pen_x += font_char_advance_width(font, ch);
    }

    if (!has_ink) {
        return false;
    }
    *out_min_x = min_x;
    *out_max_x = max_x;
    return true;
}

void draw_ui_label_ink_left(SDL_Surface *screen, const Fonts *fonts, int ink_left_x, int y, const char *label)
{
    int min_x;
    int max_x;
    int draw_x = ink_left_x;

    if (fonts && font_string_ink_bounds(fonts->white, label, &min_x, &max_x)) {
        draw_x = ink_left_x - min_x;
    }
    draw_ui_label(screen, fonts, draw_x, y, label);
}

void draw_ui_label_ink_right(SDL_Surface *screen, const Fonts *fonts, int ink_right_x, int y, const char *label)
{
    int min_x;
    int max_x;
    int draw_x = fonts ? ink_right_x - nSDL_GetStringWidth(fonts->white, label) + 1 : ink_right_x;

    if (fonts && font_string_ink_bounds(fonts->white, label, &min_x, &max_x)) {
        draw_x = ink_right_x - max_x;
    }
    draw_ui_label(screen, fonts, draw_x, y, label);
}

void draw_overlay_backdrop_dim(SDL_Surface *screen, uint8_t dim_mix)
{
    SDL_Rect veil = {0, 0, SCREEN_W, SCREEN_H};
    int alpha = (112 * dim_mix + 127) / 255;

    if (!screen || dim_mix == 0) {
        return;
    }
    if (!surface_is_rgb565(screen)) {
        SDL_FillRect(screen, &veil, SDL_MapRGB(screen->format, 0, 0, 0));
        return;
    }
    dim_rect_rgb565(screen, &veil, alpha);
}

void draw_cursor(SDL_Surface *screen, int x, int y)
{
    static Uint16 cursor_pixels[12 * 12];
    static SDL_Surface *cursor_surface = NULL;
    static UiThemeId cursor_theme = UI_THEME_COUNT;
    SDL_Rect dst = {(Sint16) x, (Sint16) y, 12, 12};

    if (!screen) {
        return;
    }
    if (cursor_theme != g_ui_theme_id) {
        const Uint16 pale = ui_theme()->cursor_pale;
        const Uint16 mid = ui_theme()->cursor_mid;
        const Uint16 deep = ui_theme()->cursor_deep;
        const Uint16 dark = ui_theme()->cursor_dark;
        const Uint16 outer = ui_theme()->surface_outer;
        const Uint16 shadow = ui_theme()->cursor_shadow;
        const Uint16 themed_pixels[12 * 12] = {
            UI_COLOR_WHITE, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY,
            UI_COLOR_WHITE, UI_COLOR_WHITE, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY,
            UI_COLOR_WHITE, pale, UI_COLOR_WHITE, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY,
            UI_COLOR_WHITE, pale, pale, UI_COLOR_WHITE, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY,
            UI_COLOR_WHITE, pale, mid, pale, UI_COLOR_WHITE, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY,
            UI_COLOR_WHITE, pale, mid, mid, pale, UI_COLOR_WHITE, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY,
            UI_COLOR_WHITE, pale, mid, deep, deep, pale, UI_COLOR_WHITE, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY,
            UI_COLOR_WHITE, pale, mid, deep, dark, dark, pale, UI_COLOR_WHITE, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY,
            UI_COLOR_WHITE, pale, mid, deep, dark, shadow, shadow, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY,
            UI_CURSOR_KEY, outer, UI_CURSOR_KEY, UI_CURSOR_KEY, deep, shadow, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY,
            UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, outer, shadow, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY,
            UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, outer, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY, UI_CURSOR_KEY
        };
        memcpy(cursor_pixels, themed_pixels, sizeof(cursor_pixels));
        cursor_theme = g_ui_theme_id;
    }
    if (!cursor_surface) {
        cursor_surface = SDL_CreateRGBSurfaceFrom(
            (void *) cursor_pixels,
            12,
            12,
            16,
            12 * 2,
            0xF800,
            0x07E0,
            0x001F,
            0
        );
        if (cursor_surface) {
            SDL_SetColorKey(cursor_surface, SDL_SRCCOLORKEY, UI_CURSOR_KEY);
        }
    }
    if (cursor_surface) {
        SDL_BlitSurface(cursor_surface, NULL, screen, &dst);
    }
}

void compute_video_rects(
    const Movie *movie,
    ScaleMode scale_mode,
    VideoAlign video_align_x,
    VideoAlign video_align_y,
    SDL_Rect *src,
    SDL_Rect *dst
)
{
    double video_aspect = (double) movie->header.video_width / movie->header.video_height;
    double screen_aspect = (double) SCREEN_W / SCREEN_H;

    src->x = 0;
    src->y = 0;
    src->w = movie->header.video_width;
    src->h = movie->header.video_height;

    if (scale_mode == SCALE_NATIVE) {
        dst->w = movie->header.video_width;
        dst->h = movie->header.video_height;
        dst->x = aligned_axis_position(SCREEN_W, dst->w, video_align_x);
        dst->y = aligned_axis_position(SCREEN_H, dst->h, video_align_y);
        return;
    }

    if (scale_mode == SCALE_FILL) {
        dst->x = 0;
        dst->y = 0;
        dst->w = SCREEN_W;
        dst->h = SCREEN_H;
        if (video_aspect > screen_aspect) {
            src->w = (Uint16) ((double) movie->header.video_height * screen_aspect);
            src->x = (Uint16) aligned_axis_position(movie->header.video_width, src->w, video_align_x);
        } else {
            src->h = (Uint16) ((double) movie->header.video_width / screen_aspect);
            src->y = (Uint16) aligned_axis_position(movie->header.video_height, src->h, video_align_y);
        }
        return;
    }

    if (scale_mode == SCALE_STRETCH) {
        dst->x = 0;
        dst->y = 0;
        dst->w = SCREEN_W;
        dst->h = SCREEN_H;
        return;
    }

    if (video_aspect > screen_aspect) {
        dst->w = SCREEN_W;
        dst->h = (Uint16) ((double) SCREEN_W / video_aspect);
    } else {
        dst->h = SCREEN_H;
        dst->w = (Uint16) ((double) SCREEN_H * video_aspect);
    }
    dst->x = aligned_axis_position(SCREEN_W, dst->w, video_align_x);
    dst->y = aligned_axis_position(SCREEN_H, dst->h, video_align_y);
}

bool clip_scaled_rects_to_screen(
    const SDL_Rect *src,
    const SDL_Rect *dst,
    int source_w,
    int source_h,
    SDL_Rect *clipped_src,
    SDL_Rect *clipped_dst
)
{
    int dst_x0;
    int dst_y0;
    int dst_x1;
    int dst_y1;
    int clip_x0;
    int clip_y0;
    int clip_x1;
    int clip_y1;
    int src_x0;
    int src_y0;
    int src_x1;
    int src_y1;

    if (!src || !dst || !clipped_src || !clipped_dst ||
        src->w == 0 || src->h == 0 || dst->w == 0 || dst->h == 0 ||
        source_w <= 0 || source_h <= 0) {
        return false;
    }

    dst_x0 = dst->x;
    dst_y0 = dst->y;
    dst_x1 = dst_x0 + dst->w;
    dst_y1 = dst_y0 + dst->h;
    clip_x0 = clamp_int(dst_x0, 0, SCREEN_W);
    clip_y0 = clamp_int(dst_y0, 0, SCREEN_H);
    clip_x1 = clamp_int(dst_x1, 0, SCREEN_W);
    clip_y1 = clamp_int(dst_y1, 0, SCREEN_H);
    if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0) {
        return false;
    }

    src_x0 = src->x + (int) (((int64_t) (clip_x0 - dst_x0) * src->w) / dst->w);
    src_y0 = src->y + (int) (((int64_t) (clip_y0 - dst_y0) * src->h) / dst->h);
    src_x1 = src->x + (int) (((int64_t) (clip_x1 - dst_x0) * src->w) / dst->w);
    src_y1 = src->y + (int) (((int64_t) (clip_y1 - dst_y0) * src->h) / dst->h);
    src_x0 = clamp_int(src_x0, 0, source_w);
    src_y0 = clamp_int(src_y0, 0, source_h);
    src_x1 = clamp_int(src_x1, 0, source_w);
    src_y1 = clamp_int(src_y1, 0, source_h);
    if (src_x1 <= src_x0 || src_y1 <= src_y0) {
        return false;
    }

    clipped_dst->x = (Sint16) clip_x0;
    clipped_dst->y = (Sint16) clip_y0;
    clipped_dst->w = (Uint16) (clip_x1 - clip_x0);
    clipped_dst->h = (Uint16) (clip_y1 - clip_y0);
    clipped_src->x = (Sint16) src_x0;
    clipped_src->y = (Sint16) src_y0;
    clipped_src->w = (Uint16) (src_x1 - src_x0);
    clipped_src->h = (Uint16) (src_y1 - src_y0);
    return true;
}

void draw_surface_frame_scaled_clipped(
    SDL_Surface *screen,
    SDL_Surface *surface,
    const SDL_Rect *src,
    const SDL_Rect *dst
)
{
    SDL_Rect clipped_src;
    SDL_Rect clipped_dst;

    if (!screen || !surface ||
        !clip_scaled_rects_to_screen(
            src,
            dst,
            surface->w,
            surface->h,
            &clipped_src,
            &clipped_dst)) {
        return;
    }

    if (clipped_src.w == clipped_dst.w && clipped_src.h == clipped_dst.h) {
        SDL_BlitSurface(surface, &clipped_src, screen, &clipped_dst);
    } else {
        SDL_SoftStretch(surface, &clipped_src, screen, &clipped_dst);
    }
}

void draw_surface_frame_scaled_clipped_mix(
    SDL_Surface *screen,
    SDL_Surface *surface,
    const SDL_Rect *src,
    const SDL_Rect *dst,
    uint8_t mix
)
{
    SDL_Rect clipped_src;
    SDL_Rect clipped_dst;
    SDL_Rect local_dst;
    SDL_Surface *scaled;

    if (mix == 0) {
        return;
    }
    if (mix >= 255) {
        draw_surface_frame_scaled_clipped(screen, surface, src, dst);
        return;
    }
    if (!screen || !surface ||
        !clip_scaled_rects_to_screen(
            src,
            dst,
            surface->w,
            surface->h,
            &clipped_src,
            &clipped_dst)) {
        return;
    }

    scaled = create_rgb565_surface(clipped_dst.w, clipped_dst.h);
    if (!scaled) {
        if (mix >= 128) {
            draw_surface_frame_scaled_clipped(screen, surface, src, dst);
        }
        return;
    }
    local_dst.x = 0;
    local_dst.y = 0;
    local_dst.w = clipped_dst.w;
    local_dst.h = clipped_dst.h;
    if (clipped_src.w == clipped_dst.w && clipped_src.h == clipped_dst.h) {
        SDL_BlitSurface(surface, &clipped_src, scaled, &local_dst);
    } else {
        SDL_SoftStretch(surface, &clipped_src, scaled, &local_dst);
    }
    blit_surface_rgb565_mix(screen, scaled, &clipped_dst, mix);
    SDL_FreeSurface(scaled);
}

void draw_movie_frame_scaled_clipped(
    SDL_Surface *screen,
    Movie *movie,
    const SDL_Rect *src,
    const SDL_Rect *dst
)
{
    if (!movie) {
        return;
    }
    draw_surface_frame_scaled_clipped(screen, movie->frame_surface, src, dst);
}

void draw_movie_frame_background_rects(
    SDL_Surface *screen,
    Movie *movie,
    const SDL_Rect *src,
    const SDL_Rect *dst
)
{
    SDL_Rect top_bar;
    SDL_Rect bottom_bar;
    SDL_Rect left_bar;
    SDL_Rect right_bar;
    Uint32 black;

    if (!screen || !movie || !movie->frame_surface || !src || !dst) {
        return;
    }
    black = SDL_MapRGB(screen->format, 0, 0, 0);
    top_bar.x = 0;
    top_bar.y = 0;
    top_bar.w = SCREEN_W;
    top_bar.h = (Uint16) clamp_int(dst->y, 0, SCREEN_H);
    if (top_bar.h > 0) {
        SDL_FillRect(screen, &top_bar, black);
    }
    bottom_bar.x = 0;
    bottom_bar.y = (Sint16) clamp_int(dst->y + dst->h, 0, SCREEN_H);
    bottom_bar.w = SCREEN_W;
    bottom_bar.h = (Uint16) (SCREEN_H - bottom_bar.y);
    if (bottom_bar.h > 0) {
        SDL_FillRect(screen, &bottom_bar, black);
    }
    left_bar.x = 0;
    left_bar.y = (Sint16) clamp_int(dst->y, 0, SCREEN_H);
    left_bar.w = (Uint16) clamp_int(dst->x, 0, SCREEN_W);
    left_bar.h = (Uint16) clamp_int(dst->h, 0, SCREEN_H - left_bar.y);
    if (left_bar.w > 0 && left_bar.h > 0) {
        SDL_FillRect(screen, &left_bar, black);
    }
    right_bar.x = (Sint16) clamp_int(dst->x + dst->w, 0, SCREEN_W);
    right_bar.y = left_bar.y;
    right_bar.w = (Uint16) (SCREEN_W - right_bar.x);
    right_bar.h = left_bar.h;
    if (right_bar.w > 0 && right_bar.h > 0) {
        SDL_FillRect(screen, &right_bar, black);
    }
    draw_movie_frame_scaled_clipped(screen, movie, src, dst);
}

