#include "player_internal.h"

const char *active_subtitle_track_name(const Movie *movie)
{
    if (!movie || movie->subtitle_track_count == 0 || movie->selected_subtitle_track >= movie->subtitle_track_count) {
        return "No subtitles";
    }
    if (!movie->subtitle_tracks[movie->selected_subtitle_track].name || movie->subtitle_tracks[movie->selected_subtitle_track].name[0] == '\0') {
        return "Subtitles";
    }
    return movie->subtitle_tracks[movie->selected_subtitle_track].name;
}

void draw_outlined_text(SDL_Surface *surface, nSDL_Font *white_font, nSDL_Font *outline_font, int x, int y, const char *text)
{
    nSDL_DrawString(surface, outline_font, x - 1, y, text);
    nSDL_DrawString(surface, outline_font, x + 1, y, text);
    nSDL_DrawString(surface, outline_font, x, y - 1, text);
    nSDL_DrawString(surface, outline_font, x, y + 1, text);
    nSDL_DrawString(surface, white_font, x, y, text);
}

int wrap_subtitle(nSDL_Font *font, const char *text, int max_width, char lines[MAX_SUBTITLE_LINES][MAX_SUBTITLE_LINE_LEN])
{
    char current[MAX_SUBTITLE_LINE_LEN];
    size_t pos = 0;
    int line_count = 0;

    if (!font || !text || max_width <= 0) {
        return 0;
    }

    current[0] = '\0';
    memset(lines, 0, sizeof(char) * MAX_SUBTITLE_LINES * MAX_SUBTITLE_LINE_LEN);
    while (text[pos] != '\0' && line_count < MAX_SUBTITLE_LINES) {
        char word[64];
        size_t word_len = 0;
        while (text[pos] == ' ') {
            pos++;
        }
        if (text[pos] == '\n') {
            if (current[0] != '\0') {
                strncpy(lines[line_count], current, MAX_SUBTITLE_LINE_LEN - 1);
                line_count++;
                current[0] = '\0';
            }
            pos++;
            continue;
        }
        while (text[pos] != '\0' && text[pos] != ' ' && text[pos] != '\n' && word_len + 1 < sizeof(word)) {
            word[word_len++] = text[pos++];
        }
        word[word_len] = '\0';
        if (word_len == 0) {
            continue;
        }
        if (current[0] == '\0') {
            snprintf(current, sizeof(current), "%s", word);
        } else {
            char candidate[MAX_SUBTITLE_LINE_LEN];
            size_t current_len = strlen(current);
            if (current_len + 1 + word_len >= sizeof(candidate)) {
                strncpy(lines[line_count], current, MAX_SUBTITLE_LINE_LEN - 1);
                line_count++;
                if (line_count >= MAX_SUBTITLE_LINES) {
                    break;
                }
                snprintf(current, sizeof(current), "%s", word);
            } else {
                memcpy(candidate, current, current_len);
                candidate[current_len] = ' ';
                memcpy(candidate + current_len + 1, word, word_len + 1);
                if (nSDL_GetStringWidth(font, candidate) > max_width) {
                    strncpy(lines[line_count], current, MAX_SUBTITLE_LINE_LEN - 1);
                    line_count++;
                    if (line_count >= MAX_SUBTITLE_LINES) {
                        break;
                    }
                    snprintf(current, sizeof(current), "%s", word);
                } else {
                    memcpy(current, candidate, word_len + current_len + 2);
                }
            }
        }
    }
    if (current[0] != '\0' && line_count < MAX_SUBTITLE_LINES) {
        strncpy(lines[line_count], current, MAX_SUBTITLE_LINE_LEN - 1);
        line_count++;
    }
    return line_count;
}

int subtitle_scale_num(int subtitle_size)
{
    static const int numerators[4] = {1, 4, 5, 2};
    subtitle_size = clamp_int(subtitle_size, 0, 3);
    return numerators[subtitle_size];
}

int subtitle_scale_den(int subtitle_size)
{
    static const int denominators[4] = {1, 3, 3, 1};
    subtitle_size = clamp_int(subtitle_size, 0, 3);
    return denominators[subtitle_size];
}

int subtitle_font_id_for_index(size_t subtitle_font_index)
{
    if (subtitle_font_index >= SUBTITLE_FONT_CHOICE_COUNT) {
        return g_subtitle_font_choices[SUBTITLE_FONT_DEFAULT_INDEX];
    }
    return g_subtitle_font_choices[subtitle_font_index];
}

const char *subtitle_font_name_for_index(size_t subtitle_font_index)
{
    if (subtitle_font_index >= SUBTITLE_FONT_CHOICE_COUNT) {
        return g_subtitle_font_names[SUBTITLE_FONT_DEFAULT_INDEX];
    }
    return g_subtitle_font_names[subtitle_font_index];
}

SubtitlePlacement subtitle_opposite_placement(SubtitlePlacement placement)
{
    switch (placement) {
        case SUBTITLE_POS_AUTO:
            return SUBTITLE_POS_BAR_TOP;
        case SUBTITLE_POS_VIDEO_BOTTOM:
            return SUBTITLE_POS_VIDEO_TOP;
        case SUBTITLE_POS_VIDEO_TOP:
            return SUBTITLE_POS_VIDEO_BOTTOM;
        case SUBTITLE_POS_BAR_TOP:
            return SUBTITLE_POS_BAR_BOTTOM;
        case SUBTITLE_POS_BAR_BOTTOM:
        default:
            return SUBTITLE_POS_BAR_TOP;
    }
}

const char *subtitle_placement_label(SubtitlePlacement placement)
{
    switch (placement) {
        case SUBTITLE_POS_VIDEO_BOTTOM:
            return "VIDEO BOTTOM";
        case SUBTITLE_POS_VIDEO_TOP:
            return "VIDEO TOP";
        case SUBTITLE_POS_BAR_TOP:
            return "BAR TOP";
        case SUBTITLE_POS_AUTO:
            return "AUTO";
        case SUBTITLE_POS_BAR_BOTTOM:
        default:
            return "BAR BOTTOM";
    }
}

bool subtitle_track_supports_auto_positioning(const Movie *movie, uint16_t track_index)
{
    if (!movie || !movie->subtitle_tracks || movie->subtitle_track_count == 0 || track_index >= movie->subtitle_track_count) {
        return false;
    }
    return movie->subtitle_tracks[track_index].supports_positioning != 0;
}

bool selected_subtitle_track_supports_auto_positioning(const Movie *movie)
{
    if (!movie || movie->subtitle_track_count == 0 || movie->selected_subtitle_track >= movie->subtitle_track_count) {
        return false;
    }
    return subtitle_track_supports_auto_positioning(movie, movie->selected_subtitle_track);
}

SubtitlePlacement subtitle_normalize_placement(SubtitlePlacement placement, bool auto_supported)
{
    placement = (SubtitlePlacement) clamp_int((int) placement, SUBTITLE_POS_BAR_BOTTOM, SUBTITLE_POS_COUNT - 1);
    if (!auto_supported && placement == SUBTITLE_POS_AUTO) {
        return SUBTITLE_POS_BAR_BOTTOM;
    }
    return placement;
}

SubtitlePlacement subtitle_cycle_placement(SubtitlePlacement placement, bool auto_supported)
{
    SubtitlePlacement next = subtitle_normalize_placement(placement, auto_supported);

    do {
        next = (SubtitlePlacement) ((next + 1) % SUBTITLE_POS_COUNT);
    } while (!auto_supported && next == SUBTITLE_POS_AUTO);
    return next;
}

SubtitlePlacement subtitle_effective_manual_placement(SubtitlePlacement placement, SubtitlePlacement fallback)
{
    if (placement == SUBTITLE_POS_AUTO) {
        if (fallback == SUBTITLE_POS_AUTO) {
            return SUBTITLE_POS_BAR_BOTTOM;
        }
        return fallback;
    }
    return placement;
}

int subtitle_align_column(uint8_t align)
{
    int value = clamp_int((int) align, 1, 9) - 1;
    return value % 3;
}

int subtitle_align_row(uint8_t align)
{
    int value = clamp_int((int) align, 1, 9) - 1;
    return value / 3;
}

int subtitle_scale_coord(uint16_t value, int extent)
{
    if (extent <= 0) {
        return 0;
    }
    return (int) (((uint32_t) value * (uint32_t) extent + (SUBTITLE_COORD_SCALE / 2U)) / SUBTITLE_COORD_SCALE);
}

bool subtitle_resolve_layout_spec(
    const SDL_Rect *video_rect,
    uint8_t overlay_mix,
    SubtitlePlacement placement,
    SubtitlePlacement manual_fallback,
    const SubtitleCue *cue,
    SubtitleLayoutSpec *layout
)
{
    SubtitlePlacement effective_manual;

    if (!video_rect || !layout) {
        return false;
    }

    memset(layout, 0, sizeof(*layout));
    layout->video_rect = *video_rect;
    layout->overlay_mix = overlay_mix;
    effective_manual = subtitle_effective_manual_placement(placement, manual_fallback);
    layout->manual_placement = effective_manual;

    if (placement == SUBTITLE_POS_AUTO &&
        cue &&
        cue->position_mode != SUBTITLE_CUE_POSITION_NONE &&
        cue->align >= 1 &&
        cue->align <= 9) {
        int column = subtitle_align_column(cue->align);

        layout->mode = cue->position_mode;
        layout->align = cue->align;
        layout->margin_l = subtitle_scale_coord(cue->margin_l, video_rect->w);
        layout->margin_r = subtitle_scale_coord(cue->margin_r, video_rect->w);
        layout->margin_v = subtitle_scale_coord(cue->margin_v, video_rect->h);
        layout->absolute_x = video_rect->x + subtitle_scale_coord(cue->pos_x, video_rect->w);
        layout->absolute_y = video_rect->y + subtitle_scale_coord(cue->pos_y, video_rect->h);

        if (layout->mode == SUBTITLE_CUE_POSITION_ABSOLUTE) {
            int left_space = layout->absolute_x - video_rect->x;
            int right_space = (video_rect->x + video_rect->w) - layout->absolute_x;

            if (column == 0) {
                layout->wrap_width = right_space - 6;
            } else if (column == 2) {
                layout->wrap_width = left_space - 6;
            } else {
                layout->wrap_width = (2 * (left_space < right_space ? left_space : right_space)) - 6;
            }
        } else if (layout->mode == SUBTITLE_CUE_POSITION_MARGIN) {
            if (column == 0) {
                layout->wrap_width = video_rect->w - layout->margin_l - 6;
            } else if (column == 2) {
                layout->wrap_width = video_rect->w - layout->margin_r - 6;
            } else {
                layout->wrap_width = video_rect->w - layout->margin_l - layout->margin_r - 12;
            }
        }

        if (layout->wrap_width >= 32) {
            return true;
        }

        memset(layout, 0, sizeof(*layout));
        layout->video_rect = *video_rect;
        layout->overlay_mix = overlay_mix;
        layout->manual_placement = effective_manual;
    }

    if (layout->manual_placement == SUBTITLE_POS_BAR_BOTTOM || layout->manual_placement == SUBTITLE_POS_BAR_TOP) {
        layout->wrap_width = SCREEN_W - 12;
    } else {
        layout->wrap_width = video_rect->w - 12;
    }
    return layout->wrap_width > 0;
}

int ui_bar_hidden_offset_for_mix(uint8_t chrome_mix)
{
    return (int) (((uint32_t) UI_BAR_H * (255U - chrome_mix) + 127U) / 255U);
}

int ui_bar_visible_height_for_mix(uint8_t chrome_mix)
{
    return UI_BAR_H - ui_bar_hidden_offset_for_mix(chrome_mix);
}

int subtitle_visible_bottom_limit(const SubtitleLayoutSpec *layout, int bottom_margin)
{
    int bottom;

    if (!layout) {
        return SCREEN_H;
    }

    bottom = layout->video_rect.y + layout->video_rect.h - bottom_margin;
    if (layout->overlay_mix > 0) {
        int ui_top = SCREEN_H - 2 - ui_bar_visible_height_for_mix(layout->overlay_mix);
        if (bottom > ui_top) {
            bottom = ui_top;
        }
    }
    return bottom;
}

int subtitle_visible_max_y(const SubtitleLayoutSpec *layout, int surface_h)
{
    int min_y;
    int max_y;

    if (!layout) {
        return 0;
    }

    min_y = layout->video_rect.y;
    max_y = subtitle_visible_bottom_limit(layout, 0) - surface_h;
    return max_y < min_y ? min_y : max_y;
}

void subtitle_layout_dst_rect(
    const SubtitleLayoutSpec *layout,
    int surface_w,
    int surface_h,
    SDL_Rect *dst
)
{
    int x = 0;
    int y = 0;
    const SDL_Rect *video_rect;

    if (!layout || !dst) {
        return;
    }

    video_rect = &layout->video_rect;
    if (layout->mode == SUBTITLE_CUE_POSITION_ABSOLUTE && layout->align >= 1 && layout->align <= 9) {
        int column = subtitle_align_column(layout->align);
        int row = subtitle_align_row(layout->align);

        if (column == 0) {
            x = layout->absolute_x;
        } else if (column == 2) {
            x = layout->absolute_x - surface_w;
        } else {
            x = layout->absolute_x - (surface_w / 2);
        }

        if (row == 0) {
            y = layout->absolute_y - surface_h;
        } else if (row == 2) {
            y = layout->absolute_y;
        } else {
            y = layout->absolute_y - (surface_h / 2);
        }

        x = clamp_int(x, video_rect->x, video_rect->x + video_rect->w - surface_w);
        y = clamp_int(y, video_rect->y, subtitle_visible_max_y(layout, surface_h));
    } else if (layout->mode == SUBTITLE_CUE_POSITION_MARGIN && layout->align >= 1 && layout->align <= 9) {
        int column = subtitle_align_column(layout->align);
        int row = subtitle_align_row(layout->align);

        if (column == 0) {
            x = video_rect->x + layout->margin_l;
        } else if (column == 2) {
            x = (video_rect->x + video_rect->w) - layout->margin_r - surface_w;
        } else {
            x = video_rect->x + ((video_rect->w - surface_w) / 2) + ((layout->margin_l - layout->margin_r) / 2);
        }

        if (row == 0) {
            y = subtitle_visible_bottom_limit(layout, layout->margin_v) - surface_h;
        } else if (row == 2) {
            y = video_rect->y + layout->margin_v;
        } else {
            y = video_rect->y + ((video_rect->h - surface_h) / 2);
        }

        x = clamp_int(x, video_rect->x, video_rect->x + video_rect->w - surface_w);
        y = clamp_int(y, video_rect->y, subtitle_visible_max_y(layout, surface_h));
    } else {
        int area_x;
        int area_y;
        int area_w;
        int area_top;
        int area_bottom;
        int area_height;

        if (layout->manual_placement == SUBTITLE_POS_BAR_BOTTOM || layout->manual_placement == SUBTITLE_POS_BAR_TOP) {
            area_x = 0;
            area_w = SCREEN_W;
        } else {
            area_x = video_rect->x;
            area_w = video_rect->w;
        }

        switch (layout->manual_placement) {
            case SUBTITLE_POS_BAR_BOTTOM:
                area_top = video_rect->y + video_rect->h + 2;
                area_bottom = SCREEN_H - 2 - ui_bar_visible_height_for_mix(layout->overlay_mix);
                area_height = area_bottom - area_top;
                if (area_height >= surface_h) {
                    area_y = area_top + (area_height - surface_h) / 2;
                } else {
                    area_y = area_bottom - surface_h;
                    if (area_y < 2) {
                        area_y = 2;
                    }
                }
                break;
            case SUBTITLE_POS_VIDEO_TOP:
                area_y = video_rect->y + 4;
                break;
            case SUBTITLE_POS_BAR_TOP:
                area_top = 2;
                area_bottom = video_rect->y - 2;
                area_height = area_bottom - area_top;
                if (area_height >= surface_h) {
                    area_y = area_top + (area_height - surface_h) / 2;
                } else {
                    area_y = 4;
                }
                break;
            case SUBTITLE_POS_VIDEO_BOTTOM:
            default:
                area_y = subtitle_visible_bottom_limit(layout, 8) - surface_h;
                if (area_y < video_rect->y + 4) {
                    area_y = video_rect->y + 4;
                }
                break;
        }

        x = area_x + (area_w - surface_w) / 2;
        y = area_y;
    }

    dst->x = (Sint16) x;
    dst->y = (Sint16) y;
    dst->w = (Uint16) surface_w;
    dst->h = (Uint16) surface_h;
}

void subtitle_fonts_for_style(const Fonts *fonts, size_t subtitle_font_index, nSDL_Font **white_font, nSDL_Font **outline_font)
{
    int font_id = subtitle_font_id_for_index(subtitle_font_index);

    *white_font = fonts->subtitle_white[font_id];
    *outline_font = fonts->subtitle_outline[font_id];
    if (!*white_font || !*outline_font) {
        *white_font = fonts->subtitle_white[NSDL_FONT_TINYTYPE];
        *outline_font = fonts->subtitle_outline[NSDL_FONT_TINYTYPE];
    }
}

void draw_scaled_outlined_text(
    SDL_Surface *screen,
    nSDL_Font *white_font,
    nSDL_Font *outline_font,
    int x,
    int y,
    const char *text,
    int scale_num,
    int scale_den
)
{
    int text_w = nSDL_GetStringWidth(white_font, text);
    int text_h = nSDL_GetStringHeight(white_font, text);
    SDL_Surface *text_surface;
    Uint32 key;
    int dst_w;
    int dst_h;
    int dst_x;
    int dst_y;
    Uint16 key16;
    Uint16 *src_pixels;
    Uint16 *dst_pixels;
    int src_pitch;
    int dst_pitch;
    if (scale_num <= 0 || scale_den <= 0) {
        return;
    }
    if (scale_num == scale_den) {
        draw_outlined_text(screen, white_font, outline_font, x, y, text);
        return;
    }
    text_surface = SDL_CreateRGBSurface(
        SDL_SWSURFACE,
        text_w + 4,
        text_h + 4,
        screen->format->BitsPerPixel,
        screen->format->Rmask,
        screen->format->Gmask,
        screen->format->Bmask,
        screen->format->Amask
    );
    if (!text_surface) {
        draw_outlined_text(screen, white_font, outline_font, x, y, text);
        return;
    }
    key = SDL_MapRGB(text_surface->format, 255, 0, 255);
    SDL_FillRect(text_surface, NULL, key);
    draw_outlined_text(text_surface, white_font, outline_font, 2, 2, text);
    dst_w = (text_surface->w * scale_num) / scale_den;
    dst_h = (text_surface->h * scale_num) / scale_den;
    if (dst_w <= 0 || dst_h <= 0 || screen->format->BitsPerPixel != 16 || text_surface->format->BitsPerPixel != 16) {
        SDL_FreeSurface(text_surface);
        draw_outlined_text(screen, white_font, outline_font, x, y, text);
        return;
    }
    if (SDL_MUSTLOCK(text_surface)) {
        SDL_LockSurface(text_surface);
    }
    if (SDL_MUSTLOCK(screen)) {
        SDL_LockSurface(screen);
    }
    key16 = (Uint16) key;
    src_pixels = (Uint16 *) text_surface->pixels;
    dst_pixels = (Uint16 *) screen->pixels;
    src_pitch = text_surface->pitch / 2;
    dst_pitch = screen->pitch / 2;
    for (dst_y = 0; dst_y < dst_h; ++dst_y) {
        int src_y;
        int draw_y = y + dst_y;
        if (draw_y < 0 || draw_y >= screen->h) {
            continue;
        }
        if (dst_h > 1 && text_surface->h > 1) {
            src_y = (dst_y * (text_surface->h - 1) + ((dst_h - 1) / 2)) / (dst_h - 1);
        } else {
            src_y = 0;
        }
        for (dst_x = 0; dst_x < dst_w; ++dst_x) {
            int src_x;
            Uint16 sample;
            int draw_x = x + dst_x;
            if (draw_x < 0 || draw_x >= screen->w) {
                continue;
            }
            if (dst_w > 1 && text_surface->w > 1) {
                src_x = (dst_x * (text_surface->w - 1) + ((dst_w - 1) / 2)) / (dst_w - 1);
            } else {
                src_x = 0;
            }
            sample = src_pixels[src_y * src_pitch + src_x];
            if (sample != key16) {
                dst_pixels[draw_y * dst_pitch + draw_x] = sample;
            }
        }
    }
    if (SDL_MUSTLOCK(screen)) {
        SDL_UnlockSurface(screen);
    }
    if (SDL_MUSTLOCK(text_surface)) {
        SDL_UnlockSurface(text_surface);
    }
    SDL_FreeSurface(text_surface);
}

bool ensure_subtitle_surface_cache(
    SubtitleSurfaceCache *cache,
    SDL_Surface *screen,
    const Fonts *fonts,
    const char *text,
    size_t subtitle_font_index,
    int subtitle_size,
    int wrap_width
)
{
    char lines[MAX_SUBTITLE_LINES][MAX_SUBTITLE_LINE_LEN];
    int line_count;
    nSDL_Font *white_font;
    nSDL_Font *outline_font;
    int scale_num;
    int scale_den;
    int line_height;
    int total_height;
    int line_index;
    int max_line_width = 0;
    Uint32 key;

    (void) screen;

    if (!cache || !screen || !fonts || !text || !*text || subtitle_size < 0 || wrap_width <= 0) {
        invalidate_subtitle_surface_cache(cache);
        return false;
    }

    subtitle_size = clamp_int(subtitle_size, 0, 3);
    if (cache->surface &&
        cache->text == text &&
        cache->subtitle_font_index == subtitle_font_index &&
        cache->subtitle_size == subtitle_size &&
        cache->wrap_width == wrap_width) {
        return true;
    }

    invalidate_subtitle_surface_cache(cache);
    subtitle_fonts_for_style(fonts, subtitle_font_index, &white_font, &outline_font);
    scale_num = subtitle_scale_num(subtitle_size);
    scale_den = subtitle_scale_den(subtitle_size);

    line_count = wrap_subtitle(white_font, text, (wrap_width * scale_den) / scale_num, lines);
    if (line_count <= 0) {
        return false;
    }

    line_height = nSDL_GetStringHeight(white_font, "Ag");
    if (line_height < 10) {
        line_height = 10;
    }
    line_height = (line_height * scale_num) / scale_den;
    if (line_height < 10) {
        line_height = 10;
    }
    total_height = line_count * line_height;
    if (line_count > 1) {
        total_height += (line_count - 1) * 2;
    }

    for (line_index = 0; line_index < line_count; ++line_index) {
        int width = (nSDL_GetStringWidth(white_font, lines[line_index]) * scale_num) / scale_den;
        if (width > max_line_width) {
            max_line_width = width;
        }
    }
    if (max_line_width <= 0 || total_height <= 0) {
        return false;
    }

    cache->surface = create_rgb565_surface(max_line_width, total_height);
    if (!cache->surface) {
        return false;
    }
    key = SDL_MapRGB(cache->surface->format, 255, 0, 255);
    SDL_FillRect(cache->surface, NULL, key);
    SDL_SetColorKey(cache->surface, SDL_SRCCOLORKEY, key);
    for (line_index = 0; line_index < line_count; ++line_index) {
        int width = (nSDL_GetStringWidth(white_font, lines[line_index]) * scale_num) / scale_den;
        int x = (max_line_width - width) / 2;
        draw_scaled_outlined_text(
            cache->surface,
            white_font,
            outline_font,
            x,
            line_index * (line_height + 2),
            lines[line_index],
            scale_num,
            scale_den
        );
    }

    cache->text = text;
    cache->subtitle_font_index = subtitle_font_index;
    cache->subtitle_size = subtitle_size;
    cache->wrap_width = wrap_width;
    return true;
}

void draw_subtitle_cached(
    SDL_Surface *screen,
    const Fonts *fonts,
    SubtitleSurfaceCache *cache,
    const char *text,
    size_t subtitle_font_index,
    int subtitle_size,
    const SubtitleLayoutSpec *layout
)
{
    SDL_Rect dst;

    if (!screen || !cache || !layout || !text || !*text || subtitle_size < 0) {
        invalidate_subtitle_surface_cache(cache);
        return;
    }
    if (!ensure_subtitle_surface_cache(
            cache,
            screen,
            fonts,
            text,
            subtitle_font_index,
            subtitle_size,
            layout->wrap_width)) {
        return;
    }

    subtitle_layout_dst_rect(layout, cache->surface->w, cache->surface->h, &dst);
    SDL_BlitSurface(cache->surface, NULL, screen, &dst);
}

void draw_subtitle(
    SDL_Surface *screen,
    const Fonts *fonts,
    const char *text,
    size_t subtitle_font_index,
    int subtitle_size,
    const SubtitleLayoutSpec *layout
)
{
    char lines[MAX_SUBTITLE_LINES][MAX_SUBTITLE_LINE_LEN];
    int line_count;
    int line_index;
    nSDL_Font *white_font;
    nSDL_Font *outline_font;
    int scale_num;
    int scale_den;
    int line_height;
    int total_height;
    int base_y = 0;
    int line_max_width = 0;
    SDL_Rect dst;
    if (!text || !*text) {
        return;
    }
    if (!layout || subtitle_size < 0 || layout->wrap_width <= 0) {
        return;
    }
    subtitle_size = clamp_int(subtitle_size, 0, 3);
    subtitle_fonts_for_style(fonts, subtitle_font_index, &white_font, &outline_font);
    scale_num = subtitle_scale_num(subtitle_size);
    scale_den = subtitle_scale_den(subtitle_size);
    line_height = nSDL_GetStringHeight(white_font, "Ag");
    if (line_height < 10) {
        line_height = 10;
    }
    line_height = (line_height * scale_num) / scale_den;
    if (line_height < 10) {
        line_height = 10;
    }
    line_count = wrap_subtitle(white_font, text, (layout->wrap_width * scale_den) / scale_num, lines);
    if (line_count <= 0) {
        return;
    }
    total_height = line_count * line_height;
    if (line_count > 1) {
        total_height += (line_count - 1) * 2;
    }
    for (line_index = 0; line_index < line_count; ++line_index) {
        int width = (nSDL_GetStringWidth(white_font, lines[line_index]) * scale_num) / scale_den;
        if (width > line_max_width) {
            line_max_width = width;
        }
    }
    if (line_max_width <= 0) {
        return;
    }
    subtitle_layout_dst_rect(layout, line_max_width, total_height, &dst);
    base_y = dst.y;
    for (line_index = 0; line_index < line_count; ++line_index) {
        int width = (nSDL_GetStringWidth(white_font, lines[line_index]) * scale_num) / scale_den;
        int x = dst.x + (line_max_width - width) / 2;
        draw_scaled_outlined_text(
            screen,
            white_font,
            outline_font,
            x,
            base_y + (line_index * (line_height + 2)),
            lines[line_index],
            scale_num,
            scale_den
        );
    }
}

