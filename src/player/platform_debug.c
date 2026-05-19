#include "player_internal.h"

bool ensure_debug_ring_storage(void)
{
    if (g_debug_ring) {
        return true;
    }

    g_debug_ring = (char (*)[DEBUG_LINE_LEN]) calloc(DEBUG_RING_SIZE, sizeof(*g_debug_ring));
    if (!g_debug_ring) {
        g_debug_ring_count = 0;
        g_debug_ring_next = 0;
        return false;
    }
    return true;
}

void release_debug_ring_storage(void)
{
    free(g_debug_ring);
    g_debug_ring = NULL;
    g_debug_ring_count = 0;
    g_debug_ring_next = 0;
}

bool debug_is_runtime_logging_enabled(void)
{
    return g_debug_logging_enabled;
}

bool debug_should_collect_metrics(void)
{
    return g_debug_metrics_enabled || g_debug_logging_enabled;
}

scr_type_t screen_buffer_type(void)
{
    return has_colors ? SCR_320x240_565 : SCR_320x240_8;
}

scr_type_t screen_lcd_type(void)
{
    scr_type_t native_type = lcd_type();

    return native_type == SCR_TYPE_INVALID ? screen_buffer_type() : native_type;
}

void patch_cx2_lcd_edge_timing(void)
{
    volatile uint32_t *timing_1 = (volatile uint32_t *) 0xC0000004;
    uint32_t timing;

    if (!has_colors || !is_cx2) {
        return;
    }

    /* Ndless applies this same timing fix at install time for CX II panels
     * whose first/last landscape column is clipped by the LCD controller. */
    timing = *timing_1;
    if (timing == 0x0720013F) {
        return;
    }
    if (timing == 0x03780D3F || lcd_type() == SCR_240x320_565) {
        *timing_1 = 0x0720013F;
    }
}

void present_screen(SDL_Surface *screen)
{
    bool locked = false;

    if (!screen) {
        return;
    }
    patch_cx2_lcd_edge_timing();
    if (SDL_MUSTLOCK(screen)) {
        if (SDL_LockSurface(screen) != 0) {
            return;
        }
        locked = true;
    }
    lcd_blit(screen->pixels, screen_buffer_type());
    if (locked) {
        SDL_UnlockSurface(screen);
    }
}

void present_black_screen(SDL_Surface *screen)
{
    if (!screen) {
        return;
    }

    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
    present_screen(screen);
}

void debug_set_metrics_collection(bool enabled)
{
    g_debug_metrics_enabled = enabled;
}

void debug_set_runtime_logging(bool enabled)
{
    if (enabled) {
        if (ensure_debug_ring_storage()) {
            g_debug_logging_enabled = true;
            g_debug_ring_count = 0;
            g_debug_ring_next = 0;
            debug_clear_last_error();
        } else {
            g_debug_logging_enabled = false;
        }
    } else {
        g_debug_logging_enabled = false;
        release_debug_ring_storage();
        debug_clear_last_error();
    }
}

void debug_tracevf(bool force, const char *fmt, va_list args)
{
    char line[DEBUG_LINE_LEN];
    size_t slot_index;
    uint32_t now_ms = g_clock.initialized ? monotonic_clock_now_ms() : 0;

    if (!force && !g_debug_logging_enabled) {
        return;
    }

    vsnprintf(line, sizeof(line), fmt, args);
    if (!ensure_debug_ring_storage()) {
        return;
    }

    slot_index = g_debug_ring_next;
    snprintf(
        g_debug_ring[slot_index],
        sizeof(g_debug_ring[slot_index]),
        "[%10lu] %.*s",
        (unsigned long) now_ms,
        (int) (sizeof(g_debug_ring[slot_index]) - 14),
        line
    );
    g_debug_ring_next = (g_debug_ring_next + 1U) % DEBUG_RING_SIZE;
    if (g_debug_ring_count < DEBUG_RING_SIZE) {
        g_debug_ring_count++;
    }
}

void debug_tracef(const char *fmt, ...)
{
    va_list args;

    if (!g_debug_logging_enabled) {
        return;
    }

    va_start(args, fmt);
    debug_tracevf(false, fmt, args);
    va_end(args);
}

void debug_tracef_force(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    debug_tracevf(true, fmt, args);
    va_end(args);
}

void debug_clear_last_error(void)
{
    g_last_error_message[0] = '\0';
}

const char *debug_last_error(void)
{
    return g_last_error_message[0] != '\0' ? g_last_error_message : "unknown";
}

void debug_failf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(g_last_error_message, sizeof(g_last_error_message), fmt, args);
    va_end(args);
    debug_tracef_force("%s", g_last_error_message);
}

size_t total_prefetched_chunk_bytes(const Movie *movie)
{
    size_t total = 0;
    int index;

    if (!movie) {
        return 0;
    }
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        if (movie->prefetched[index].chunk_index >= 0 && movie->prefetched[index].chunk_storage) {
            total += movie->prefetched[index].chunk_storage_size;
        }
    }
    return total;
}

void clear_all_prefetched_chunks(Movie *movie)
{
    int index;

    if (!movie) {
        return;
    }
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        clear_prefetched_chunk(&movie->prefetched[index]);
    }
}

PrefetchedChunk *find_farthest_prefetched_chunk(Movie *movie)
{
    PrefetchedChunk *victim = NULL;
    int index;

    if (!movie) {
        return NULL;
    }
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        PrefetchedChunk *candidate = &movie->prefetched[index];
        if (candidate->chunk_index < 0) {
            continue;
        }
        if (!victim || candidate->chunk_index > victim->chunk_index) {
            victim = candidate;
        }
    }
    return victim;
}

bool ensure_prefetch_budget(Movie *movie, int requested_chunk, size_t required_bytes)
{
    size_t total_bytes;

    if (!movie) {
        return false;
    }
    total_bytes = total_prefetched_chunk_bytes(movie);
    while (total_bytes + required_bytes > PREFETCH_MAX_TOTAL_BYTES) {
        PrefetchedChunk *victim = find_farthest_prefetched_chunk(movie);
        if (!victim || victim->chunk_index <= requested_chunk) {
            debug_tracef(
                "prefetch budget skip chunk=%d need=%lu total=%lu cap=%lu",
                requested_chunk,
                (unsigned long) required_bytes,
                (unsigned long) total_bytes,
                (unsigned long) PREFETCH_MAX_TOTAL_BYTES
            );
            return false;
        }
        debug_tracef(
            "prefetch evict chunk=%d for chunk=%d total=%lu need=%lu",
            victim->chunk_index,
            requested_chunk,
            (unsigned long) total_bytes,
            (unsigned long) required_bytes
        );
        clear_prefetched_chunk(victim);
        total_bytes = total_prefetched_chunk_bytes(movie);
    }
    return true;
}

void debug_log_path_for_movie(const char *movie_path, char *log_path, size_t log_path_size)
{
    char directory[MAX_PATH_LEN];
    char *slash;

    if (!log_path || log_path_size == 0) {
        return;
    }
    if (!movie_path || movie_path[0] == '\0') {
        snprintf(log_path, log_path_size, "ndvideo-debug.log");
        return;
    }

    snprintf(directory, sizeof(directory), "%s", movie_path);
    slash = strrchr(directory, '/');
    if (!slash) {
        slash = strrchr(directory, '\\');
    }
    if (slash) {
        *slash = '\0';
        snprintf(log_path, log_path_size, "%s/%s", directory, "ndvideo-debug.log");
    } else {
        snprintf(log_path, log_path_size, "ndvideo-debug.log");
    }
}

void report_movie_decode_failure(const Movie *movie, const char *movie_path, const char *reason)
{
    char log_path[MAX_PATH_LEN];
    char message[192];

    if (movie) {
        debug_tracef_force(
            "decode failed reason=%s frame=%lu loaded_chunk=%d decoded_local=%d prefetched=%lu",
            reason ? reason : "unknown",
            (unsigned long) movie->current_frame,
            movie->loaded_chunk,
            movie->decoded_local_frame,
            (unsigned long) total_prefetched_chunk_bytes(movie)
        );
    } else {
        debug_tracef_force("decode failed reason=%s", reason ? reason : "unknown");
    }
    debug_log_path_for_movie(movie_path, log_path, sizeof(log_path));
    debug_dump_session(log_path, movie, "decode-failure");
    snprintf(
        message,
        sizeof(message),
        "Movie decode failed.\n%s\nSee ndvideo-debug.log.",
        debug_last_error()
    );
    show_msgbox("ND Video Player", message);
}

void report_movie_open_failure(const char *movie_path)
{
    char log_path[MAX_PATH_LEN];
    char message[192];

    debug_tracef_force("open failed: %s", debug_last_error());
    debug_log_path_for_movie(movie_path, log_path, sizeof(log_path));
    debug_dump_session(log_path, NULL, "open-failure");
    snprintf(
        message,
        sizeof(message),
        "Failed to open movie file.\n%s\nSee ndvideo-debug.log.",
        debug_last_error()
    );
    show_msgbox("ND Video Player", message);
}

uint16_t read_le16(const uint8_t *src)
{
    return (uint16_t) src[0] | ((uint16_t) src[1] << 8);
}

uint32_t read_le32(const uint8_t *src)
{
    return (uint32_t) src[0]
        | ((uint32_t) src[1] << 8)
        | ((uint32_t) src[2] << 16)
        | ((uint32_t) src[3] << 24);
}

char *dup_string(const char *src)
{
    size_t length = strlen(src);
    char *copy = (char *) malloc(length + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, src, length + 1);
    return copy;
}

bool has_suffix(const char *value, const char *suffix)
{
    size_t value_length = strlen(value);
    size_t suffix_length = strlen(suffix);
    if (value_length < suffix_length) {
        return false;
    }
    value += value_length - suffix_length;
    while (*value && *suffix) {
        if (tolower((unsigned char) *value) != tolower((unsigned char) *suffix)) {
            return false;
        }
        ++value;
        ++suffix;
    }
    return true;
}

char *display_name_for_movie(const char *filename)
{
    size_t length = strlen(filename);
    size_t trim_length = length;
    char *copy;

    if (length > 8 && has_suffix(filename, ".nvp.tns")) {
        trim_length = length - 8;
    } else if (length > 4 && has_suffix(filename, ".nvp")) {
        trim_length = length - 4;
    } else if (length > 4 && has_suffix(filename, ".tns")) {
        trim_length = length - 4;
    }

    copy = (char *) malloc(trim_length + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, filename, trim_length);
    copy[trim_length] = '\0';
    return copy;
}

void normalize_display_spacing(char *text)
{
    char *src;
    char *dst;
    bool pending_space = false;

    if (!text) {
        return;
    }

    src = text;
    dst = text;
    while (*src) {
        unsigned char ch = (unsigned char) *src++;
        if (isspace(ch)) {
            pending_space = dst != text;
            continue;
        }
        if (pending_space) {
            *dst++ = ' ';
            pending_space = false;
        }
        *dst++ = (char) ch;
    }
    *dst = '\0';
}

void append_movie_filename_detail(char *detail, size_t detail_size, const char *begin, size_t length)
{
    size_t current_length;
    size_t copy_length;

    if (!detail || detail_size == 0 || !begin) {
        return;
    }
    while (length > 0 && isspace((unsigned char) *begin)) {
        ++begin;
        --length;
    }
    while (length > 0 && isspace((unsigned char) begin[length - 1])) {
        --length;
    }
    if (length == 0) {
        return;
    }

    current_length = strlen(detail);
    if (current_length > 0 && current_length + 3 < detail_size) {
        memcpy(detail + current_length, " | ", 3);
        current_length += 3;
        detail[current_length] = '\0';
    }

    if (current_length >= detail_size - 1) {
        return;
    }
    copy_length = length;
    if (copy_length > detail_size - current_length - 1) {
        copy_length = detail_size - current_length - 1;
    }
    memcpy(detail + current_length, begin, copy_length);
    detail[current_length + copy_length] = '\0';
}

bool movie_display_fields_for_filename(const char *filename, char **out_name, char **out_detail)
{
    char *base_name;
    char *title;
    char *detail;
    const char *src;
    char *dst;
    size_t base_length;

    if (!out_name || !out_detail) {
        return false;
    }
    *out_name = NULL;
    *out_detail = NULL;

    base_name = display_name_for_movie(filename ? filename : "");
    if (!base_name) {
        return false;
    }

    base_length = strlen(base_name);
    title = (char *) calloc(base_length + 1, 1);
    detail = (char *) calloc(base_length + 1, 1);
    if (!title || !detail) {
        free(base_name);
        free(title);
        free(detail);
        return false;
    }

    src = base_name;
    dst = title;
    while (*src) {
        if (*src == '[') {
            const char *close = strchr(src + 1, ']');
            if (close) {
                append_movie_filename_detail(detail, base_length + 1, src + 1, (size_t) (close - src - 1));
                src = close + 1;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';

    normalize_display_spacing(title);
    normalize_display_spacing(detail);
    if (title[0] == '\0') {
        snprintf(title, base_length + 1, "%s", base_name);
        normalize_display_spacing(title);
    }

    *out_name = title;
    if (detail[0] != '\0') {
        *out_detail = detail;
    } else {
        free(detail);
        *out_detail = NULL;
    }

    free(base_name);
    return true;
}

int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

Sint16 chrome_centered_x_for_width(int width)
{
    if (width >= SCREEN_W) {
        return 0;
    }
    return (Sint16) clamp_int(UI_CHROME_CENTER_X - (width / 2), 0, SCREEN_W - width);
}

Sint16 chrome_left_x_for_margin(int margin)
{
    return (Sint16) clamp_int(margin + UI_CHROME_VISUAL_X_OFFSET, 0, SCREEN_W);
}

Sint16 chrome_right_x_for_margin(int margin)
{
    return (Sint16) clamp_int(SCREEN_W - margin + UI_CHROME_VISUAL_X_OFFSET, 0, SCREEN_W);
}

Sint16 chrome_soft_panel_right_x_for_margin(int margin)
{
    return (Sint16) clamp_int(
        chrome_right_x_for_margin(margin) + UI_SOFT_PANEL_RIGHT_PERCEIVED_EDGE_INSET,
        0,
        SCREEN_W
    );
}

bool rect_contains_point(const SDL_Rect *rect, int x, int y)
{
    return rect &&
        x >= rect->x && x < rect->x + rect->w &&
        y >= rect->y && y < rect->y + rect->h;
}

bool pointer_over_rect(const PointerState *pointer, const SDL_Rect *rect)
{
    return pointer && pointer->visible && rect_contains_point(rect, pointer->x, pointer->y);
}

VideoAlign clamp_video_align(int value)
{
    return (VideoAlign) clamp_int(value, (int) VIDEO_ALIGN_NEGATIVE, (int) VIDEO_ALIGN_POSITIVE);
}

void apply_video_align_preset(
    VideoAlign *horizontal,
    VideoAlign *vertical,
    VideoAlign target_horizontal,
    VideoAlign target_vertical
)
{
    if (!horizontal || !vertical) {
        return;
    }

    if (*horizontal == target_horizontal && *vertical == target_vertical) {
        *horizontal = VIDEO_ALIGN_CENTER;
        *vertical = VIDEO_ALIGN_CENTER;
        return;
    }

    *horizontal = target_horizontal;
    *vertical = target_vertical;
}

int aligned_axis_position(int container_size, int content_size, VideoAlign align)
{
    int slack = container_size - content_size;

    if (align <= VIDEO_ALIGN_NEGATIVE) {
        return 0;
    }
    if (align >= VIDEO_ALIGN_POSITIVE) {
        return slack;
    }
    return slack / 2;
}

void format_video_align_status(
    VideoAlign horizontal,
    VideoAlign vertical,
    char *buffer,
    size_t buffer_size
)
{
    const char *vertical_label = NULL;
    const char *horizontal_label = NULL;

    if (!buffer || buffer_size == 0) {
        return;
    }

    if (vertical < VIDEO_ALIGN_CENTER) {
        vertical_label = "TOP";
    } else if (vertical > VIDEO_ALIGN_CENTER) {
        vertical_label = "BOTTOM";
    }
    if (horizontal < VIDEO_ALIGN_CENTER) {
        horizontal_label = "LEFT";
    } else if (horizontal > VIDEO_ALIGN_CENTER) {
        horizontal_label = "RIGHT";
    }

    if (!vertical_label && !horizontal_label) {
        snprintf(buffer, buffer_size, "VIDEO CENTER");
    } else if (!vertical_label) {
        snprintf(buffer, buffer_size, "VIDEO %s", horizontal_label);
    } else if (!horizontal_label) {
        snprintf(buffer, buffer_size, "VIDEO %s", vertical_label);
    } else {
        snprintf(buffer, buffer_size, "VIDEO %s %s", vertical_label, horizontal_label);
    }
}

uint32_t cx_level_to_brightness_raw(uint32_t level)
{
    uint32_t clamped;

    if (level >= LCD_BRIGHTNESS_CX_BACKLIGHT_OFF) {
        return LCD_BRIGHTNESS_MAX;
    }
    clamped = (uint32_t) clamp_int(
        (int) level,
        (int) LCD_BRIGHTNESS_CX_LEVEL_MIN,
        (int) LCD_BRIGHTNESS_CX_LEVEL_MAX
    );

    return (uint32_t) ((clamped * (uint32_t) LCD_BRIGHTNESS_MAX +
        (LCD_BRIGHTNESS_CX_LEVEL_MAX / 2U)) / LCD_BRIGHTNESS_CX_LEVEL_MAX);
}

uint32_t brightness_raw_to_cx_level(uint32_t raw_value)
{
    uint32_t clamped = (uint32_t) clamp_int((int) raw_value, LCD_BRIGHTNESS_MIN, LCD_BRIGHTNESS_MAX);

    return (uint32_t) ((clamped * LCD_BRIGHTNESS_CX_LEVEL_MAX +
        ((uint32_t) LCD_BRIGHTNESS_MAX / 2U)) / (uint32_t) LCD_BRIGHTNESS_MAX);
}

uint32_t current_lcd_brightness(void)
{
    if (!has_colors) {
        return LCD_BRIGHTNESS_MIN;
    }
    if (is_cx2) {
        return (uint32_t) clamp_int((int) *LCD_BRIGHTNESS_CX2_ADDR,
            LCD_BRIGHTNESS_MIN,
            LCD_BRIGHTNESS_MAX);
    }
    return cx_level_to_brightness_raw(*LCD_BRIGHTNESS_CX_ADDR);
}

uint32_t set_lcd_brightness(int value)
{
    uint32_t clamped = (uint32_t) clamp_int(value, LCD_BRIGHTNESS_MIN, LCD_BRIGHTNESS_MAX);

    if (!has_colors) {
        return clamped;
    }
    if (is_cx2) {
        *LCD_BRIGHTNESS_CX2_ADDR = clamped;
    } else {
        *LCD_BRIGHTNESS_CX_ADDR = brightness_raw_to_cx_level(clamped);
    }
    return clamped;
}

void set_lcd_dark_for_power_off(void)
{
    if (!has_colors) {
        return;
    }
    if (is_cx2) {
        set_lcd_brightness(LCD_BRIGHTNESS_MAX);
    } else {
        *LCD_BRIGHTNESS_CX_ADDR = LCD_BRIGHTNESS_CX_BACKLIGHT_OFF;
    }
}

uint32_t adjust_lcd_brightness(int delta)
{
    uint32_t current = current_lcd_brightness();

    if (delta > 0 && current >= LCD_BRIGHTNESS_LOWEST_NORMAL) {
        return set_lcd_brightness(LCD_BRIGHTNESS_MAX);
    }
    if (delta < 0 && current > LCD_BRIGHTNESS_LOWEST_NORMAL) {
        return set_lcd_brightness(LCD_BRIGHTNESS_LOWEST_NORMAL);
    }
    return set_lcd_brightness((int) current + delta);
}

unsigned lcd_brightness_percent(uint32_t raw_value)
{
    uint32_t clamped = (uint32_t) clamp_int((int) raw_value, LCD_BRIGHTNESS_MIN, LCD_BRIGHTNESS_MAX);
    unsigned percent;

    percent = (unsigned) ((((uint32_t) LCD_BRIGHTNESS_MAX - clamped) * 100U) +
        ((uint32_t) LCD_BRIGHTNESS_MAX / 2U)) / (uint32_t) LCD_BRIGHTNESS_MAX;
    return percent == 0U ? 1U : percent;
}

void display_power_off(DisplayPowerState *state, bool was_paused)
{
    if (!state || state->off) {
        return;
    }

    state->resume_playback_on_wake = !was_paused;
    state->saved_brightness = current_lcd_brightness();
    set_lcd_dark_for_power_off();
    state->off = true;
}

void display_power_on(DisplayPowerState *state)
{
    if (!state || !state->off) {
        return;
    }

    set_lcd_brightness((int) state->saved_brightness);
    state->off = false;
}

const char *filename_from_path(const char *path)
{
    const char *slash;
    const char *backslash;

    if (!path || path[0] == '\0') {
        return "";
    }

    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    if (slash && backslash) {
        return (slash > backslash ? slash : backslash) + 1;
    }
    if (slash) {
        return slash + 1;
    }
    if (backslash) {
        return backslash + 1;
    }
    return path;
}

SDL_Surface *create_rgb565_surface(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    return SDL_CreateRGBSurface(
        SDL_SWSURFACE,
        width,
        height,
        16,
        0xF800, 0x07E0, 0x001F, 0
    );
}

SDL_Surface *create_scaled_surface_from_surface(SDL_Surface *source, int max_width, int max_height)
{
    SDL_Surface *scaled;
    int dst_width;
    int dst_height;

    if (!source || max_width <= 0 || max_height <= 0 || source->w <= 0 || source->h <= 0) {
        return NULL;
    }

    dst_width = max_width;
    dst_height = (source->h * max_width) / source->w;
    if (dst_height <= 0) {
        dst_height = 1;
    }
    if (dst_height > max_height) {
        dst_height = max_height;
        dst_width = (source->w * max_height) / source->h;
        if (dst_width <= 0) {
            dst_width = 1;
        }
    }

    scaled = create_rgb565_surface(dst_width, dst_height);
    if (!scaled) {
        return NULL;
    }
    if (SDL_SoftStretch(source, NULL, scaled, NULL) != 0) {
        SDL_FreeSurface(scaled);
        return NULL;
    }
    return scaled;
}

void invalidate_subtitle_surface_cache(SubtitleSurfaceCache *cache)
{
    if (!cache) {
        return;
    }
    if (cache->surface) {
        SDL_FreeSurface(cache->surface);
    }
    memset(cache, 0, sizeof(*cache));
}

void free_subtitle_surface_cache(SubtitleSurfaceCache *cache)
{
    invalidate_subtitle_surface_cache(cache);
}

void clear_screenshot_preview(ScreenshotPreviewState *preview)
{
    if (!preview) {
        return;
    }
    if (preview->surface) {
        SDL_FreeSurface(preview->surface);
    }
    memset(preview, 0, sizeof(*preview));
}

void clear_seek_bar_preview_decode_job(SeekBarPreviewState *preview)
{
    SeekPreviewDecodeJob *job;

    if (!preview) {
        return;
    }

    job = &preview->decode_job;
    if (job->decoder) {
        if (job->decoder_initialized) {
            h264bsdShutdown(job->decoder);
        }
        h264bsdFree(job->decoder);
    }
    free(job->chunk_storage);
    free(job->frame_offsets);
    free(job->pixels);
    memset(job, 0, sizeof(*job));
    job->chunk_index = -1;
}

void finish_seek_bar_preview_decode_job(SeekBarPreviewState *preview)
{
    SeekPreviewDecodeJob *job;

    if (!preview) {
        return;
    }

    job = &preview->decode_job;
    if (!job->active) {
        return;
    }
    job->active = false;
    job->complete = true;
}

void clear_seek_bar_preview_surface(SeekBarPreviewState *preview)
{
    if (!preview) {
        return;
    }
    if (preview->surface) {
        SDL_FreeSurface(preview->surface);
        preview->surface = NULL;
    }
    preview->decoded_chunk_index = -1;
    preview->decoded_frame_index = UINT32_MAX;
    preview->surface_started_ms = 0;
    preview->surface_fade_pending = false;
    preview->surface_render_pending = false;
}

void clear_seek_bar_preview(SeekBarPreviewState *preview)
{
    if (!preview) {
        return;
    }
    clear_seek_bar_preview_decode_job(preview);
    clear_seek_bar_preview_surface(preview);
    memset(preview, 0, sizeof(*preview));
    preview->decoded_chunk_index = -1;
    preview->decoded_frame_index = UINT32_MAX;
    preview->last_pointer_x = -1;
    preview->suppress_marker_x = -1;
    preview->suppress_frame_index = UINT32_MAX;
    preview->decode_job.chunk_index = -1;
}

void suppress_seek_bar_preview_rebuild(SeekBarPreviewState *preview, int marker_x, uint32_t frame_index)
{
    if (!preview) {
        return;
    }
    preview->suppress_until_pointer_moves = true;
    preview->suppress_marker_x = marker_x;
    preview->suppress_frame_index = frame_index;
}

bool monotonic_clock_try_init_hw_timer(void)
{
    if (is_classic) {
        return false;
    }

    g_clock.value_reg = (volatile unsigned *) MONOTONIC_TIMER_VALUE_ADDR;
    g_clock.control_reg = (volatile unsigned *) MONOTONIC_TIMER_CONTROL_ADDR;
    g_clock.speed_reg = (volatile unsigned *) MONOTONIC_TIMER_CLOCK_SOURCE_ADDR;
    g_clock.original_control = *g_clock.control_reg;
    g_clock.original_speed = *g_clock.speed_reg;

    *g_clock.control_reg = 0;
    *g_clock.speed_reg = MONOTONIC_TIMER_CLOCK_SOURCE_32768HZ;
    *g_clock.control_reg = MONOTONIC_TIMER_CONTROL_ENABLE_32BIT;

    g_clock.last_value = (uint32_t) *g_clock.value_reg;
    g_clock.using_hw_timer = true;
    return true;
}

void monotonic_clock_init(void)
{
    memset(&g_clock, 0, sizeof(g_clock));
    g_clock.initialized = true;
    g_clock.ticks_per_second = TIMER_TICKS_PER_SEC;
    monotonic_clock_try_init_hw_timer();
}

uint32_t monotonic_clock_ticks_per_second(void)
{
    if (g_clock.ticks_per_second) {
        return g_clock.ticks_per_second;
    }
    return TIMER_TICKS_PER_SEC;
}

uint64_t monotonic_clock_now_ticks(void)
{
    uint32_t ticks_per_second = monotonic_clock_ticks_per_second();
    uint64_t sdl_ticks;
    if (!g_clock.initialized) {
        return 0;
    }
    if (g_clock.using_hw_timer) {
        uint32_t current_value = (uint32_t) *g_clock.value_reg;
        uint32_t elapsed = g_clock.last_value - current_value;
        if (elapsed > MONOTONIC_TIMER_MAX_DELTA_TICKS) {
            g_clock.using_hw_timer = false;
            sdl_ticks = (((uint64_t) SDL_GetTicks()) * ticks_per_second) / 1000ULL;
            if (sdl_ticks > g_clock.elapsed_ticks) {
                g_clock.elapsed_ticks = sdl_ticks;
            }
            return g_clock.elapsed_ticks;
        }
        g_clock.elapsed_ticks += (uint64_t) elapsed;
        g_clock.last_value = current_value;
        return g_clock.elapsed_ticks;
    }
    sdl_ticks = (((uint64_t) SDL_GetTicks()) * ticks_per_second) / 1000ULL;
    if (sdl_ticks > g_clock.elapsed_ticks) {
        g_clock.elapsed_ticks = sdl_ticks;
    }
    return g_clock.elapsed_ticks;
}

uint32_t monotonic_clock_ticks_to_ms(uint64_t ticks)
{
    return (uint32_t) ((ticks * 1000ULL) / monotonic_clock_ticks_per_second());
}

uint32_t monotonic_clock_now_ms(void)
{
    return monotonic_clock_ticks_to_ms(monotonic_clock_now_ticks());
}

void monotonic_clock_shutdown(void)
{
    if (g_clock.control_reg) {
        *g_clock.control_reg = 0;
        if (g_clock.speed_reg) {
            *g_clock.speed_reg = g_clock.original_speed;
        }
        *g_clock.control_reg = g_clock.original_control;
    }
    memset(&g_clock, 0, sizeof(g_clock));
}

