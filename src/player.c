#include <dirent.h>
#include <libndls.h>
#include <os.h>
#include <SDL/SDL.h>

#include "overclock.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define UI_BAR_H 40
#define SEEK_STEP_MS 5000
#define PICKER_MAX_FILES 128
#define PICKER_VISIBLE_ROWS 9
#define MAX_PATH_LEN 512
#define MAX_SUBTITLE_LINES 3
#define MAX_SUBTITLE_LINE_LEN 96
#define PREFETCH_CHUNK_COUNT 5
#define TIMER_TICKS_PER_SEC 32768U
#define POINTER_FIXED_SHIFT 6
#define POINTER_UI_TIMEOUT_MS 1800U
#define POINTER_GAIN_NUM 5
#define POINTER_GAIN_DEN 4
#define ENABLE_STARTUP_OVERCLOCK 0

#pragma pack(push, 1)
typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t flags;
    uint16_t canvas_width;
    uint16_t canvas_height;
    uint16_t video_x;
    uint16_t video_y;
    uint16_t video_width;
    uint16_t video_height;
    uint16_t fps_num;
    uint16_t fps_den;
    uint16_t block_size;
    uint16_t chunk_frames;
    uint32_t frame_count;
    uint32_t chunk_count;
    uint32_t subtitle_count;
    uint32_t index_offset;
    uint32_t subtitle_offset;
} MovieHeader;

typedef struct {
    uint32_t offset;
    uint32_t packed_size;
    uint32_t unpacked_size;
    uint32_t first_frame;
    uint32_t frame_count;
    uint32_t frame_table_offset;
} ChunkIndexEntry;
#pragma pack(pop)

typedef struct {
    uint32_t offset;
    uint32_t packed_size;
    uint32_t unpacked_size;
    uint32_t first_frame;
    uint32_t frame_count;
} LegacyChunkIndexEntry;

typedef struct {
    uint32_t start_ms;
    uint32_t end_ms;
    char *text;
} SubtitleCue;

typedef struct {
    char *name;
    char *path;
} MovieFile;

typedef struct {
    nSDL_Font *white;
    nSDL_Font *outline;
    nSDL_Font *subtitle_white;
    nSDL_Font *subtitle_outline;
} Fonts;

typedef enum {
    SCALE_FIT = 0,
    SCALE_FILL = 1,
    SCALE_STRETCH = 2,
    SCALE_NATIVE = 3,
} ScaleMode;

typedef struct {
    touchpad_info_t *info;
    int x;
    int y;
    int fx;
    int fy;
    int last_touch_x;
    int last_touch_y;
    bool visible;
    bool down;
    bool tracking;
    bool moved;
} PointerState;

typedef struct {
    uint8_t *chunk_storage;
    size_t chunk_storage_size;
    int chunk_index;
} PrefetchedChunk;

typedef struct {
    FILE *file;
    MovieHeader header;
    ChunkIndexEntry *chunk_index;
    SubtitleCue *subtitles;
    uint16_t *framebuffer;
    uint16_t *previous_framebuffer;
    uint16_t *block_scratch;
    uint8_t *chunk_storage;
    size_t chunk_storage_size;
    uint8_t *chunk_bytes;
    uint32_t *frame_offsets;
    size_t chunk_size;
    int loaded_chunk;
    PrefetchedChunk prefetched[PREFETCH_CHUNK_COUNT];
    int decoded_local_frame;
    uint32_t current_frame;
    SDL_Surface *frame_surface;
} Movie;

typedef struct {
    bool initialized;
    uint32_t last_value;
    uint64_t elapsed_ticks;
    unsigned original_load;
    unsigned original_control;
} MonotonicClock;

static MonotonicClock g_clock;

static bool decode_to_frame(Movie *movie, uint32_t frame_index);
static bool prefetch_chunk(Movie *movie, int chunk_index);
static void prefetch_ahead(Movie *movie, int current_chunk, int max_new_chunks);
static void clear_prefetched_chunk(PrefetchedChunk *chunk);

static uint16_t read_le16(const uint8_t *src)
{
    return (uint16_t) src[0] | ((uint16_t) src[1] << 8);
}

static uint32_t read_le32(const uint8_t *src)
{
    return (uint32_t) src[0]
        | ((uint32_t) src[1] << 8)
        | ((uint32_t) src[2] << 16)
        | ((uint32_t) src[3] << 24);
}

static char *dup_string(const char *src)
{
    size_t length = strlen(src);
    char *copy = (char *) malloc(length + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, src, length + 1);
    return copy;
}

static bool has_suffix(const char *value, const char *suffix)
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

static char *display_name_for_movie(const char *filename)
{
    size_t length = strlen(filename);
    char *copy;
    if (length > 8 && has_suffix(filename, ".nvp.tns")) {
        copy = (char *) malloc(length - 3);
        if (!copy) {
            return NULL;
        }
        memcpy(copy, filename, length - 4);
        copy[length - 4] = '\0';
        return copy;
    }
    return dup_string(filename);
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static void monotonic_clock_init(void)
{
    memset(&g_clock, 0, sizeof(g_clock));
    g_clock.initialized = true;
}

static uint32_t monotonic_clock_now_ms(void)
{
    if (!g_clock.initialized) {
        return 0;
    }
    return (uint32_t) SDL_GetTicks();
}

static void monotonic_clock_shutdown(void)
{
    memset(&g_clock, 0, sizeof(g_clock));
}

static void pointer_init(PointerState *pointer)
{
    memset(pointer, 0, sizeof(*pointer));
    pointer->info = touchpad_getinfo();
    pointer->x = SCREEN_W / 2;
    pointer->y = SCREEN_H / 2;
    pointer->fx = pointer->x << POINTER_FIXED_SHIFT;
    pointer->fy = pointer->y << POINTER_FIXED_SHIFT;
    pointer->visible = pointer->info != NULL;
}

static bool pointer_update(PointerState *pointer)
{
    touchpad_report_t report;
    bool click_edge = false;
    bool current_down = false;
    if (!pointer->info || touchpad_scan(&report) != 0) {
        return false;
    }
    pointer->moved = false;
    if (report.contact || report.proximity || report.pressed) {
        if (!pointer->tracking) {
            pointer->last_touch_x = report.x;
            pointer->last_touch_y = report.y;
            pointer->tracking = true;
        } else {
            int dx = (int) report.x - pointer->last_touch_x;
            int dy = (int) report.y - pointer->last_touch_y;
            pointer->last_touch_x = report.x;
            pointer->last_touch_y = report.y;
            if (dx != 0 || dy != 0) {
                pointer->fx += (dx * SCREEN_W * POINTER_GAIN_NUM << POINTER_FIXED_SHIFT) / ((int) pointer->info->width * POINTER_GAIN_DEN);
                pointer->fy -= (dy * SCREEN_H * POINTER_GAIN_NUM << POINTER_FIXED_SHIFT) / ((int) pointer->info->height * POINTER_GAIN_DEN);
                pointer->x = clamp_int(pointer->fx >> POINTER_FIXED_SHIFT, 0, SCREEN_W - 1);
                pointer->y = clamp_int(pointer->fy >> POINTER_FIXED_SHIFT, 0, SCREEN_H - 1);
                pointer->fx = pointer->x << POINTER_FIXED_SHIFT;
                pointer->fy = pointer->y << POINTER_FIXED_SHIFT;
                pointer->moved = true;
            }
        }
        pointer->visible = true;
    } else {
        pointer->tracking = false;
    }
    current_down = report.pressed ? true : false;
    click_edge = current_down && !pointer->down;
    pointer->down = current_down;
    return click_edge;
}

static void format_clock(uint32_t total_ms, char *buffer, size_t buffer_size)
{
    uint32_t total_seconds = total_ms / 1000;
    uint32_t hours = total_seconds / 3600;
    uint32_t minutes = (total_seconds / 60) % 60;
    uint32_t seconds = total_seconds % 60;
    if (hours > 0) {
        snprintf(buffer, buffer_size, "%lu:%02lu:%02lu",
            (unsigned long) hours,
            (unsigned long) minutes,
            (unsigned long) seconds);
    } else {
        snprintf(buffer, buffer_size, "%02lu:%02lu",
            (unsigned long) minutes,
            (unsigned long) seconds);
    }
}

static uint32_t movie_frame_interval_ms(const Movie *movie)
{
    if (!movie->header.fps_num) {
        return 0;
    }
    return (uint32_t) ((((uint64_t) 1000 * movie->header.fps_den) + (movie->header.fps_num / 2)) / movie->header.fps_num);
}

static uint32_t movie_frame_time_ms(const Movie *movie, uint32_t frame_index)
{
    if (!movie->header.fps_num) {
        return 0;
    }
    return (uint32_t) (((uint64_t) frame_index * 1000ULL * movie->header.fps_den) / movie->header.fps_num);
}

static uint32_t movie_frames_from_ms(const Movie *movie, uint32_t total_ms)
{
    if (!movie->header.fps_num || !movie->header.fps_den) {
        return 0;
    }
    return (uint32_t) (((uint64_t) total_ms * movie->header.fps_num) / (1000ULL * movie->header.fps_den));
}

static uint32_t movie_duration_ms(const Movie *movie)
{
    return movie_frame_time_ms(movie, movie->header.frame_count);
}

static void reset_playback_timeline(const Movie *movie, uint32_t now_ms, uint32_t *anchor_ms, uint32_t *anchor_frame, uint32_t *next_frame_due)
{
    *anchor_ms = now_ms;
    *anchor_frame = movie->current_frame;
    *next_frame_due = now_ms + movie_frame_time_ms(movie, 1);
}

static void free_movie_files(MovieFile *files, size_t count)
{
    size_t index;
    if (!files) {
        return;
    }
    for (index = 0; index < count; ++index) {
        free(files[index].name);
        free(files[index].path);
    }
    free(files);
}

static void destroy_movie(Movie *movie)
{
    uint32_t index;
    int prefetch_index;
    if (!movie) {
        return;
    }
    if (movie->file) {
        fclose(movie->file);
    }
    if (movie->frame_surface) {
        SDL_FreeSurface(movie->frame_surface);
    }
    if (movie->subtitles) {
        for (index = 0; index < movie->header.subtitle_count; ++index) {
            free(movie->subtitles[index].text);
        }
    }
    free(movie->subtitles);
    free(movie->chunk_index);
    free(movie->framebuffer);
    free(movie->previous_framebuffer);
    free(movie->block_scratch);
    free(movie->chunk_storage);
    free(movie->frame_offsets);
    for (prefetch_index = 0; prefetch_index < PREFETCH_CHUNK_COUNT; ++prefetch_index) {
        clear_prefetched_chunk(&movie->prefetched[prefetch_index]);
    }
    memset(movie, 0, sizeof(*movie));
    movie->loaded_chunk = -1;
    for (prefetch_index = 0; prefetch_index < PREFETCH_CHUNK_COUNT; ++prefetch_index) {
        movie->prefetched[prefetch_index].chunk_index = -1;
    }
    movie->decoded_local_frame = -1;
}

static bool init_fonts(Fonts *fonts)
{
    memset(fonts, 0, sizeof(*fonts));
    fonts->white = nSDL_LoadFont(NSDL_FONT_TINYTYPE, 255, 255, 255);
    fonts->outline = nSDL_LoadFont(NSDL_FONT_TINYTYPE, 0, 0, 0);
    fonts->subtitle_white = nSDL_LoadFont(NSDL_FONT_TINYTYPE, 255, 255, 255);
    fonts->subtitle_outline = nSDL_LoadFont(NSDL_FONT_TINYTYPE, 0, 0, 0);
    if (fonts->subtitle_white) {
        nSDL_SetFontSpacing(fonts->subtitle_white, 0, 0);
    }
    if (fonts->subtitle_outline) {
        nSDL_SetFontSpacing(fonts->subtitle_outline, 0, 0);
    }
    if (!fonts->white || !fonts->outline || !fonts->subtitle_white || !fonts->subtitle_outline) {
        return false;
    }
    return true;
}

static void free_fonts(Fonts *fonts)
{
    if (fonts->white) {
        nSDL_FreeFont(fonts->white);
    }
    if (fonts->outline) {
        nSDL_FreeFont(fonts->outline);
    }
    if (fonts->subtitle_white) {
        nSDL_FreeFont(fonts->subtitle_white);
    }
    if (fonts->subtitle_outline) {
        nSDL_FreeFont(fonts->subtitle_outline);
    }
    memset(fonts, 0, sizeof(*fonts));
}

static bool rle16_decode(const uint8_t *src, size_t src_size, uint16_t *dst, size_t dst_words)
{
    size_t src_pos = 0;
    size_t dst_pos = 0;
    while (src_pos < src_size && dst_pos < dst_words) {
        uint8_t token = src[src_pos++];
        size_t run_length = (token & 0x7F) + 1;
        if (token & 0x80) {
            uint16_t value;
            size_t index;
            if (src_pos + 2 > src_size || dst_pos + run_length > dst_words) {
                return false;
            }
            value = read_le16(src + src_pos);
            src_pos += 2;
            for (index = 0; index < run_length; ++index) {
                dst[dst_pos++] = value;
            }
        } else {
            size_t literal_bytes = run_length * 2;
            if (src_pos + literal_bytes > src_size || dst_pos + run_length > dst_words) {
                return false;
            }
            memcpy(dst + dst_pos, src + src_pos, literal_bytes);
            src_pos += literal_bytes;
            dst_pos += run_length;
        }
    }
    return src_pos == src_size && dst_pos == dst_words;
}

static bool block_region_for_id(
    const Movie *movie,
    uint8_t bx,
    uint8_t by,
    int sub_index,
    uint16_t *out_x,
    uint16_t *out_y,
    uint16_t *out_w,
    uint16_t *out_h
)
{
    uint16_t block_size = movie->header.block_size;
    uint16_t x = (uint16_t) (bx * block_size);
    uint16_t y = (uint16_t) (by * block_size);
    uint16_t block_w = block_size;
    uint16_t block_h = block_size;
    if (x >= movie->header.video_width || y >= movie->header.video_height) {
        return false;
    }
    if (x + block_w > movie->header.video_width) {
        block_w = movie->header.video_width - x;
    }
    if (y + block_h > movie->header.video_height) {
        block_h = movie->header.video_height - y;
    }
    if (sub_index >= 0) {
        uint16_t left_w = (uint16_t) ((block_w + 1) / 2);
        uint16_t right_w = block_w - left_w;
        uint16_t top_h = (uint16_t) ((block_h + 1) / 2);
        uint16_t bottom_h = block_h - top_h;
        switch (sub_index) {
            case 0:
                block_w = left_w;
                block_h = top_h;
                break;
            case 1:
                x = (uint16_t) (x + left_w);
                block_w = right_w;
                block_h = top_h;
                break;
            case 2:
                y = (uint16_t) (y + top_h);
                block_w = left_w;
                block_h = bottom_h;
                break;
            case 3:
                x = (uint16_t) (x + left_w);
                y = (uint16_t) (y + top_h);
                block_w = right_w;
                block_h = bottom_h;
                break;
            default:
                return false;
        }
    }
    if (block_w == 0 || block_h == 0) {
        return false;
    }
    *out_x = x;
    *out_y = y;
    *out_w = block_w;
    *out_h = block_h;
    return true;
}

static bool apply_block_region(
    Movie *movie,
    uint16_t *target,
    const uint8_t *payload,
    uint16_t payload_size,
    uint16_t x,
    uint16_t y,
    uint16_t block_w,
    uint16_t block_h
)
{
    uint16_t row;
    if (!rle16_decode(payload, payload_size, movie->block_scratch, (size_t) block_w * block_h)) {
        return false;
    }
    for (row = 0; row < block_h; ++row) {
        memcpy(
            target + ((size_t) (y + row) * movie->header.video_width + x),
            movie->block_scratch + ((size_t) row * block_w),
            (size_t) block_w * sizeof(uint16_t)
        );
    }
    return true;
}

static bool apply_block(Movie *movie, uint16_t *target, const uint8_t *payload, uint16_t payload_size, uint8_t bx, uint8_t by)
{
    uint16_t x;
    uint16_t y;
    uint16_t block_w;
    uint16_t block_h;
    if (!block_region_for_id(movie, bx, by, -1, &x, &y, &block_w, &block_h)) {
        return false;
    }
    return apply_block_region(movie, target, payload, payload_size, x, y, block_w, block_h);
}

static bool apply_motion_copy_region(
    Movie *movie,
    uint16_t *target,
    const uint16_t *source,
    uint16_t x,
    uint16_t y,
    uint16_t block_w,
    uint16_t block_h,
    int8_t dx,
    int8_t dy
)
{
    int source_x = (int) x + dx;
    int source_y = (int) y + dy;
    uint16_t row;
    if (source_x < 0 || source_y < 0) {
        return false;
    }
    if (source_x + block_w > movie->header.video_width || source_y + block_h > movie->header.video_height) {
        return false;
    }
    for (row = 0; row < block_h; ++row) {
        memcpy(
            target + ((size_t) (y + row) * movie->header.video_width + x),
            source + ((size_t) (source_y + row) * movie->header.video_width + source_x),
            (size_t) block_w * sizeof(uint16_t)
        );
    }
    return true;
}

static bool apply_motion_copy(Movie *movie, uint16_t *target, const uint16_t *source, uint8_t bx, uint8_t by, int8_t dx, int8_t dy)
{
    uint16_t x;
    uint16_t y;
    uint16_t block_w;
    uint16_t block_h;
    if (!block_region_for_id(movie, bx, by, -1, &x, &y, &block_w, &block_h)) {
        return false;
    }
    return apply_motion_copy_region(movie, target, source, x, y, block_w, block_h, dx, dy);
}

static bool apply_row_span(Movie *movie, uint16_t *target, const uint8_t *payload, uint16_t payload_size, uint8_t row, uint16_t x, uint16_t span_length)
{
    if (row >= movie->header.video_height || x >= movie->header.video_width || x + span_length > movie->header.video_width) {
        return false;
    }
    if (!rle16_decode(payload, payload_size, movie->block_scratch, span_length)) {
        return false;
    }
    memcpy(
        target + ((size_t) row * movie->header.video_width + x),
        movie->block_scratch,
        (size_t) span_length * sizeof(uint16_t)
    );
    return true;
}

static bool decode_diff_rows(Movie *movie, const uint8_t **cursor_ptr, size_t *remaining_ptr, uint16_t row_count)
{
    const uint8_t *cursor = *cursor_ptr;
    size_t remaining = *remaining_ptr;
    uint16_t row_index;

    for (row_index = 0; row_index < row_count; ++row_index) {
        uint8_t y;
        uint16_t run_count;
        uint16_t run_index;
        if (remaining < 3) {
            return false;
        }
        y = cursor[0];
        run_count = read_le16(cursor + 1);
        cursor += 3;
        remaining -= 3;
        for (run_index = 0; run_index < run_count; ++run_index) {
            uint16_t x;
            uint16_t span_length;
            uint16_t payload_size;
            if (remaining < 6) {
                return false;
            }
            x = read_le16(cursor);
            span_length = read_le16(cursor + 2);
            payload_size = read_le16(cursor + 4);
            cursor += 6;
            remaining -= 6;
            if (payload_size > remaining) {
                return false;
            }
            if (!apply_row_span(movie, movie->framebuffer, cursor, payload_size, y, x, span_length)) {
                return false;
            }
            cursor += payload_size;
            remaining -= payload_size;
        }
    }

    *cursor_ptr = cursor;
    *remaining_ptr = remaining;
    return true;
}

static bool skip_diff_rows(const uint8_t **cursor_ptr, size_t *remaining_ptr, uint16_t row_count)
{
    const uint8_t *cursor = *cursor_ptr;
    size_t remaining = *remaining_ptr;
    uint16_t row_index;

    for (row_index = 0; row_index < row_count; ++row_index) {
        uint16_t run_count;
        uint16_t run_index;
        if (remaining < 3) {
            return false;
        }
        run_count = read_le16(cursor + 1);
        cursor += 3;
        remaining -= 3;
        for (run_index = 0; run_index < run_count; ++run_index) {
            uint16_t payload_size;
            if (remaining < 6) {
                return false;
            }
            payload_size = read_le16(cursor + 4);
            cursor += 6;
            remaining -= 6;
            if (payload_size > remaining) {
                return false;
            }
            cursor += payload_size;
            remaining -= payload_size;
        }
    }

    *cursor_ptr = cursor;
    *remaining_ptr = remaining;
    return true;
}

static bool apply_global_motion_frame(Movie *movie, uint16_t *target, const uint16_t *source, int8_t dx, int8_t dy)
{
    int width = movie->header.video_width;
    int height = movie->header.video_height;
    int dest_x0;
    int src_x0;
    int overlap_w;
    int dest_y0;
    int src_y0;
    int overlap_h;
    int row;

    memcpy(target, source, (size_t) width * height * sizeof(uint16_t));

    if (dy == 0) {
        dest_y0 = 0;
        src_y0 = 0;
        overlap_h = height;
    } else if (dy > 0) {
        dest_y0 = 0;
        src_y0 = dy;
        overlap_h = height - dy;
    } else {
        dest_y0 = -dy;
        src_y0 = 0;
        overlap_h = height + dy;
    }

    if (dx == 0) {
        dest_x0 = 0;
        src_x0 = 0;
        overlap_w = width;
    } else if (dx > 0) {
        dest_x0 = 0;
        src_x0 = dx;
        overlap_w = width - dx;
    } else {
        dest_x0 = -dx;
        src_x0 = 0;
        overlap_w = width + dx;
    }

    if (overlap_w <= 0 || overlap_h <= 0) {
        return false;
    }

    for (row = 0; row < overlap_h; ++row) {
        memcpy(
            target + ((size_t) (dest_y0 + row) * width + dest_x0),
            source + ((size_t) (src_y0 + row) * width + src_x0),
            (size_t) overlap_w * sizeof(uint16_t)
        );
    }
    return true;
}

static bool decode_motion_records(
    Movie *movie,
    const uint8_t **cursor_ptr,
    size_t *remaining_ptr,
    uint16_t record_count,
    const uint16_t *reference_frame
)
{
    const uint8_t *cursor = *cursor_ptr;
    size_t remaining = *remaining_ptr;
    uint16_t record_index;

    for (record_index = 0; record_index < record_count; ++record_index) {
        uint8_t mode;
        uint8_t bx;
        uint8_t by;
        if (remaining < 3) {
            return false;
        }
        mode = cursor[0];
        bx = cursor[1];
        by = cursor[2];
        cursor += 3;
        remaining -= 3;
        if (mode == 1) {
            int8_t dx;
            int8_t dy;
            if (remaining < 2) {
                return false;
            }
            dx = (int8_t) cursor[0];
            dy = (int8_t) cursor[1];
            cursor += 2;
            remaining -= 2;
            if (!apply_motion_copy(movie, movie->framebuffer, reference_frame, bx, by, dx, dy)) {
                return false;
            }
        } else if (mode == 2) {
            uint8_t sub_count;
            uint8_t sub_index;
            if (remaining < 1) {
                return false;
            }
            sub_count = cursor[0];
            cursor++;
            remaining--;
            for (sub_index = 0; sub_index < sub_count; ++sub_index) {
                uint8_t quarter_index;
                uint8_t sub_mode;
                uint16_t x;
                uint16_t y;
                uint16_t block_w;
                uint16_t block_h;
                if (remaining < 2) {
                    return false;
                }
                quarter_index = cursor[0];
                sub_mode = cursor[1];
                cursor += 2;
                remaining -= 2;
                if (!block_region_for_id(movie, bx, by, quarter_index, &x, &y, &block_w, &block_h)) {
                    return false;
                }
                if (sub_mode == 1) {
                    int8_t dx;
                    int8_t dy;
                    if (remaining < 2) {
                        return false;
                    }
                    dx = (int8_t) cursor[0];
                    dy = (int8_t) cursor[1];
                    cursor += 2;
                    remaining -= 2;
                    if (!apply_motion_copy_region(movie, movie->framebuffer, reference_frame, x, y, block_w, block_h, dx, dy)) {
                        return false;
                    }
                } else {
                    uint16_t payload_size;
                    if (remaining < 2) {
                        return false;
                    }
                    payload_size = read_le16(cursor);
                    cursor += 2;
                    remaining -= 2;
                    if (payload_size > remaining) {
                        return false;
                    }
                    if (!apply_block_region(movie, movie->framebuffer, cursor, payload_size, x, y, block_w, block_h)) {
                        return false;
                    }
                    cursor += payload_size;
                    remaining -= payload_size;
                }
            }
        } else {
            uint16_t payload_size;
            if (remaining < 2) {
                return false;
            }
            payload_size = read_le16(cursor);
            cursor += 2;
            remaining -= 2;
            if (payload_size > remaining) {
                return false;
            }
            if (!apply_block(movie, movie->framebuffer, cursor, payload_size, bx, by)) {
                return false;
            }
            cursor += payload_size;
            remaining -= payload_size;
        }
    }

    *cursor_ptr = cursor;
    *remaining_ptr = remaining;
    return true;
}

static bool decode_frame_record(Movie *movie, const uint8_t *frame_data, size_t frame_size)
{
    const uint8_t *cursor = frame_data;
    size_t remaining = frame_size;
    size_t frame_words = (size_t) movie->header.video_width * movie->header.video_height;
    if (remaining < 1) {
        return false;
    }
    if (*cursor == 'N') {
        return true;
    }
    if (*cursor == 'I') {
        uint32_t payload_size;
        cursor++;
        remaining--;
        if (remaining < 4) {
            return false;
        }
        payload_size = read_le32(cursor);
        cursor += 4;
        remaining -= 4;
        if (payload_size > remaining) {
            return false;
        }
        return rle16_decode(
            cursor,
            payload_size,
            movie->framebuffer,
            frame_words
        );
    }
    if (*cursor == 'D') {
        uint16_t row_count;
        cursor++;
        remaining--;
        if (remaining < 2) {
            return false;
        }
        row_count = read_le16(cursor);
        cursor += 2;
        remaining -= 2;
        return decode_diff_rows(movie, &cursor, &remaining, row_count);
    }
    if (*cursor == 'H') {
        uint16_t row_count;
        int8_t dx;
        int8_t dy;
        cursor++;
        remaining--;
        if (remaining < 4) {
            return false;
        }
        dx = (int8_t) cursor[0];
        dy = (int8_t) cursor[1];
        row_count = read_le16(cursor + 2);
        cursor += 4;
        remaining -= 4;
        memcpy(movie->previous_framebuffer, movie->framebuffer, frame_words * sizeof(uint16_t));
        if (!apply_global_motion_frame(movie, movie->framebuffer, movie->previous_framebuffer, dx, dy)) {
            return false;
        }
        return decode_diff_rows(movie, &cursor, &remaining, row_count);
    }
    if (*cursor == 'P') {
        uint16_t block_count;
        uint16_t block_index;
        cursor++;
        remaining--;
        if (remaining < 2) {
            return false;
        }
        block_count = read_le16(cursor);
        cursor += 2;
        remaining -= 2;
        for (block_index = 0; block_index < block_count; ++block_index) {
            uint16_t payload_size;
            uint8_t bx;
            uint8_t by;
            if (remaining < 4) {
                return false;
            }
            bx = cursor[0];
            by = cursor[1];
            payload_size = read_le16(cursor + 2);
            cursor += 4;
            remaining -= 4;
            if (payload_size > remaining) {
                return false;
            }
            if (!apply_block(movie, movie->framebuffer, cursor, payload_size, bx, by)) {
                return false;
            }
            cursor += payload_size;
            remaining -= payload_size;
        }
        return true;
    }
    if (*cursor == 'M') {
        uint16_t record_count;
        cursor++;
        remaining--;
        if (remaining < 2) {
            return false;
        }
        memcpy(movie->previous_framebuffer, movie->framebuffer, frame_words * sizeof(uint16_t));
        record_count = read_le16(cursor);
        cursor += 2;
        remaining -= 2;
        if (!decode_motion_records(movie, &cursor, &remaining, record_count, movie->previous_framebuffer)) {
            return false;
        }
        return true;
    }
    if (*cursor == 'G') {
        uint16_t record_count;
        int8_t dx;
        int8_t dy;
        cursor++;
        remaining--;
        if (remaining < 4) {
            return false;
        }
        dx = (int8_t) cursor[0];
        dy = (int8_t) cursor[1];
        record_count = read_le16(cursor + 2);
        cursor += 4;
        remaining -= 4;
        memcpy(movie->previous_framebuffer, movie->framebuffer, frame_words * sizeof(uint16_t));
        if (!apply_global_motion_frame(movie, movie->framebuffer, movie->previous_framebuffer, dx, dy)) {
            return false;
        }
        memcpy(movie->previous_framebuffer, movie->framebuffer, frame_words * sizeof(uint16_t));
        if (!decode_motion_records(movie, &cursor, &remaining, record_count, movie->previous_framebuffer)) {
            return false;
        }
        return true;
    }
    return false;
}

static bool build_frame_offsets(
    const Movie *movie,
    const ChunkIndexEntry *entry,
    const uint8_t *chunk_bytes,
    size_t chunk_size,
    uint32_t *frame_offsets
)
{
    const uint8_t *cursor = chunk_bytes;
    size_t remaining = chunk_size;
    uint16_t frame_index;

    for (frame_index = 0; frame_index < entry->frame_count; ++frame_index) {
        frame_offsets[frame_index] = (uint32_t) (cursor - chunk_bytes);
        if (remaining < 1) {
            return false;
        }
        if (*cursor == 'N') {
            cursor++;
            remaining--;
        } else if (*cursor == 'I') {
            uint32_t payload_size;
            cursor++;
            remaining--;
            if (remaining < 4) {
                return false;
            }
            payload_size = read_le32(cursor);
            cursor += 4;
            remaining -= 4;
            if (payload_size > remaining) {
                return false;
            }
            cursor += payload_size;
            remaining -= payload_size;
        } else if (*cursor == 'D') {
            uint16_t row_count;
            cursor++;
            remaining--;
            if (remaining < 2) {
                return false;
            }
            row_count = read_le16(cursor);
            cursor += 2;
            remaining -= 2;
            if (!skip_diff_rows(&cursor, &remaining, row_count)) {
                return false;
            }
        } else if (*cursor == 'H') {
            uint16_t row_count;
            cursor++;
            remaining--;
            if (remaining < 4) {
                return false;
            }
            row_count = read_le16(cursor + 2);
            cursor += 4;
            remaining -= 4;
            if (!skip_diff_rows(&cursor, &remaining, row_count)) {
                return false;
            }
        } else if (*cursor == 'P') {
            uint16_t block_count;
            uint16_t block_index;
            cursor++;
            remaining--;
            if (remaining < 2) {
                return false;
            }
            block_count = read_le16(cursor);
            cursor += 2;
            remaining -= 2;
            for (block_index = 0; block_index < block_count; ++block_index) {
                uint16_t payload_size;
                if (remaining < 4) {
                    return false;
                }
                payload_size = read_le16(cursor + 2);
                cursor += 4;
                remaining -= 4;
                if (payload_size > remaining) {
                    return false;
                }
                cursor += payload_size;
                remaining -= payload_size;
            }
        } else if (*cursor == 'M') {
            uint16_t record_count;
            uint16_t record_index;
            cursor++;
            remaining--;
            if (remaining < 2) {
                return false;
            }
            record_count = read_le16(cursor);
            cursor += 2;
            remaining -= 2;
            for (record_index = 0; record_index < record_count; ++record_index) {
                uint8_t mode;
                if (remaining < 3) {
                    return false;
                }
                mode = cursor[0];
                cursor += 3;
                remaining -= 3;
                if (mode == 1) {
                    if (remaining < 2) {
                        return false;
                    }
                    cursor += 2;
                    remaining -= 2;
                } else if (mode == 2) {
                    uint8_t sub_count;
                    uint8_t sub_index;
                    if (remaining < 1) {
                        return false;
                    }
                    sub_count = cursor[0];
                    cursor++;
                    remaining--;
                    for (sub_index = 0; sub_index < sub_count; ++sub_index) {
                        uint8_t sub_mode;
                        if (remaining < 2) {
                            return false;
                        }
                        sub_mode = cursor[1];
                        cursor += 2;
                        remaining -= 2;
                        if (sub_mode == 1) {
                            if (remaining < 2) {
                                return false;
                            }
                            cursor += 2;
                            remaining -= 2;
                        } else {
                            uint16_t payload_size;
                            if (remaining < 2) {
                                return false;
                            }
                            payload_size = read_le16(cursor);
                            cursor += 2;
                            remaining -= 2;
                            if (payload_size > remaining) {
                                return false;
                            }
                            cursor += payload_size;
                            remaining -= payload_size;
                        }
                    }
                } else {
                    uint16_t payload_size;
                    if (remaining < 2) {
                        return false;
                    }
                    payload_size = read_le16(cursor);
                    cursor += 2;
                    remaining -= 2;
                    if (payload_size > remaining) {
                        return false;
                    }
                    cursor += payload_size;
                    remaining -= payload_size;
                }
            }
        } else if (*cursor == 'G') {
            uint16_t record_count;
            uint16_t record_index;
            cursor++;
            remaining--;
            if (remaining < 4) {
                return false;
            }
            record_count = read_le16(cursor + 2);
            cursor += 4;
            remaining -= 4;
            for (record_index = 0; record_index < record_count; ++record_index) {
                uint8_t mode;
                if (remaining < 3) {
                    return false;
                }
                mode = cursor[0];
                cursor += 3;
                remaining -= 3;
                if (mode == 1) {
                    if (remaining < 2) {
                        return false;
                    }
                    cursor += 2;
                    remaining -= 2;
                } else if (mode == 2) {
                    uint8_t sub_count;
                    uint8_t sub_index;
                    if (remaining < 1) {
                        return false;
                    }
                    sub_count = cursor[0];
                    cursor++;
                    remaining--;
                    for (sub_index = 0; sub_index < sub_count; ++sub_index) {
                        uint8_t sub_mode;
                        if (remaining < 2) {
                            return false;
                        }
                        sub_mode = cursor[1];
                        cursor += 2;
                        remaining -= 2;
                        if (sub_mode == 1) {
                            if (remaining < 2) {
                                return false;
                            }
                            cursor += 2;
                            remaining -= 2;
                        } else {
                            uint16_t payload_size;
                            if (remaining < 2) {
                                return false;
                            }
                            payload_size = read_le16(cursor);
                            cursor += 2;
                            remaining -= 2;
                            if (payload_size > remaining) {
                                return false;
                            }
                            cursor += payload_size;
                            remaining -= payload_size;
                        }
                    }
                } else {
                    uint16_t payload_size;
                    if (remaining < 2) {
                        return false;
                    }
                    payload_size = read_le16(cursor);
                    cursor += 2;
                    remaining -= 2;
                    if (payload_size > remaining) {
                        return false;
                    }
                    cursor += payload_size;
                    remaining -= payload_size;
                }
            }
        } else {
            return false;
        }
    }
    return true;
}

static bool load_frame_offsets(Movie *movie, const ChunkIndexEntry *entry, const uint8_t *chunk_bytes, size_t chunk_size, uint32_t *frame_offsets)
{
    if (entry->frame_count == 0) {
        return true;
    }
    if (entry->frame_table_offset != 0) {
        if (fseek(movie->file, (long) entry->frame_table_offset, SEEK_SET) != 0) {
            return false;
        }
        if (fread(frame_offsets, sizeof(uint32_t), entry->frame_count, movie->file) != entry->frame_count) {
            return false;
        }
        return true;
    }
    return build_frame_offsets(movie, entry, chunk_bytes, chunk_size, frame_offsets);
}

static bool configure_chunk_view(Movie *movie, int chunk_index)
{
    const ChunkIndexEntry *entry = movie->chunk_index + chunk_index;

    free(movie->frame_offsets);
    movie->frame_offsets = NULL;
    movie->chunk_bytes = NULL;
    movie->chunk_size = 0;

    if (movie->header.version >= 4) {
        size_t local_offset = (size_t) entry->frame_table_offset;
        size_t header_size;
        uint32_t payload_size;
        if (!movie->chunk_storage || local_offset + 4 > movie->chunk_storage_size) {
            return false;
        }
        payload_size = read_le32(movie->chunk_storage + local_offset);
        header_size = 4 + ((size_t) entry->frame_count * sizeof(uint32_t));
        if (local_offset + header_size + payload_size > movie->chunk_storage_size) {
            return false;
        }
        movie->frame_offsets = (uint32_t *) calloc((size_t) entry->frame_count, sizeof(uint32_t));
        if (!movie->frame_offsets) {
            return false;
        }
        memcpy(movie->frame_offsets, movie->chunk_storage + local_offset + 4, (size_t) entry->frame_count * sizeof(uint32_t));
        movie->chunk_bytes = movie->chunk_storage + local_offset + header_size;
        movie->chunk_size = payload_size;
        return true;
    }

    if (!movie->chunk_storage) {
        return false;
    }
    movie->frame_offsets = (uint32_t *) calloc((size_t) entry->frame_count, sizeof(uint32_t));
    if (!movie->frame_offsets) {
        return false;
    }
    if (!load_frame_offsets(movie, entry, movie->chunk_storage, movie->chunk_storage_size, movie->frame_offsets)) {
        free(movie->frame_offsets);
        movie->frame_offsets = NULL;
        return false;
    }
    movie->chunk_bytes = movie->chunk_storage;
    movie->chunk_size = movie->chunk_storage_size;
    return true;
}

static void clear_prefetched_chunk(PrefetchedChunk *chunk)
{
    if (!chunk) {
        return;
    }
    free(chunk->chunk_storage);
    chunk->chunk_storage = NULL;
    chunk->chunk_storage_size = 0;
    chunk->chunk_index = -1;
}

static PrefetchedChunk *find_prefetched_chunk(Movie *movie, int chunk_index)
{
    int index;
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        if (movie->prefetched[index].chunk_index == chunk_index) {
            return &movie->prefetched[index];
        }
    }
    return NULL;
}

static bool load_chunk(Movie *movie, int chunk_index)
{
    const ChunkIndexEntry *entry;
    uint8_t *packed_bytes = NULL;
    uLongf unpacked_size;
    PrefetchedChunk *prefetched;
    if (chunk_index < 0 || (uint32_t) chunk_index >= movie->header.chunk_count) {
        return false;
    }
    if (movie->loaded_chunk == chunk_index) {
        return true;
    }

    entry = movie->chunk_index + chunk_index;
    prefetched = find_prefetched_chunk(movie, chunk_index);
    if (prefetched) {
        free(movie->chunk_storage);
        movie->chunk_storage = prefetched->chunk_storage;
        movie->chunk_storage_size = prefetched->chunk_storage_size;
        prefetched->chunk_storage = NULL;
        prefetched->chunk_storage_size = 0;
        prefetched->chunk_index = -1;
        if (!configure_chunk_view(movie, chunk_index)) {
            return false;
        }
        movie->loaded_chunk = chunk_index;
        movie->decoded_local_frame = -1;
        return true;
    }

    free(movie->chunk_storage);
    movie->chunk_storage = NULL;
    movie->chunk_storage_size = 0;
    packed_bytes = (uint8_t *) malloc(entry->packed_size);
    if (!packed_bytes) {
        return false;
    }
    if (fseek(movie->file, (long) entry->offset, SEEK_SET) != 0) {
        free(packed_bytes);
        return false;
    }
    if (fread(packed_bytes, 1, entry->packed_size, movie->file) != entry->packed_size) {
        free(packed_bytes);
        return false;
    }
    if (entry->packed_size != entry->unpacked_size) {
        movie->chunk_storage = (uint8_t *) malloc(entry->unpacked_size);
        if (!movie->chunk_storage) {
            free(packed_bytes);
            return false;
        }
        unpacked_size = entry->unpacked_size;
        if (uncompress(movie->chunk_storage, &unpacked_size, packed_bytes, entry->packed_size) != Z_OK || unpacked_size != entry->unpacked_size) {
            free(packed_bytes);
            free(movie->chunk_storage);
            movie->chunk_storage = NULL;
            return false;
        }
        free(packed_bytes);
        movie->chunk_storage_size = entry->unpacked_size;
    } else {
        movie->chunk_storage = packed_bytes;
        movie->chunk_storage_size = entry->packed_size;
    }
    if (!configure_chunk_view(movie, chunk_index)) {
        return false;
    }
    movie->loaded_chunk = chunk_index;
    movie->decoded_local_frame = -1;
    return true;
}

static bool prefetch_chunk(Movie *movie, int chunk_index)
{
    const ChunkIndexEntry *entry;
    uint8_t *bytes;
    uint8_t *packed_bytes;
    size_t chunk_storage_size;
    uLongf unpacked_size;
    PrefetchedChunk *slot = NULL;
    int index;
    if (chunk_index < 0 || (uint32_t) chunk_index >= movie->header.chunk_count) {
        return false;
    }
    if (movie->loaded_chunk == chunk_index || find_prefetched_chunk(movie, chunk_index)) {
        return true;
    }
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        if (movie->prefetched[index].chunk_index < 0) {
            slot = &movie->prefetched[index];
            break;
        }
    }
    if (!slot) {
        slot = &movie->prefetched[0];
        for (index = 1; index < PREFETCH_CHUNK_COUNT; ++index) {
            if (movie->prefetched[index].chunk_index < slot->chunk_index) {
                slot = &movie->prefetched[index];
            }
        }
        clear_prefetched_chunk(slot);
    }
    entry = movie->chunk_index + chunk_index;
    packed_bytes = (uint8_t *) malloc(entry->packed_size);
    if (!packed_bytes) {
        return false;
    }
    if (fseek(movie->file, (long) entry->offset, SEEK_SET) != 0) {
        free(packed_bytes);
        return false;
    }
    if (fread(packed_bytes, 1, entry->packed_size, movie->file) != entry->packed_size) {
        free(packed_bytes);
        return false;
    }
    if (entry->packed_size != entry->unpacked_size) {
        bytes = (uint8_t *) malloc(entry->unpacked_size);
        if (!bytes) {
            free(packed_bytes);
            return false;
        }
        unpacked_size = entry->unpacked_size;
        if (uncompress(bytes, &unpacked_size, packed_bytes, entry->packed_size) != Z_OK || unpacked_size != entry->unpacked_size) {
            free(packed_bytes);
            free(bytes);
            return false;
        }
        free(packed_bytes);
        chunk_storage_size = entry->unpacked_size;
    } else {
        bytes = packed_bytes;
        chunk_storage_size = entry->packed_size;
    }
    slot->chunk_storage = bytes;
    slot->chunk_storage_size = chunk_storage_size;
    slot->chunk_index = chunk_index;
    return true;
}

static void prefetch_ahead(Movie *movie, int current_chunk, int max_new_chunks)
{
    int index;
    int loaded = 0;
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        int wanted_min = current_chunk + 1;
        int wanted_max = current_chunk + PREFETCH_CHUNK_COUNT;
        if (movie->prefetched[index].chunk_index >= 0 &&
            (movie->prefetched[index].chunk_index < wanted_min || movie->prefetched[index].chunk_index > wanted_max)) {
            clear_prefetched_chunk(&movie->prefetched[index]);
        }
    }
    for (index = 1; index <= PREFETCH_CHUNK_COUNT; ++index) {
        int wanted_chunk = current_chunk + index;
        if ((uint32_t) wanted_chunk < movie->header.chunk_count &&
            movie->loaded_chunk != wanted_chunk &&
            !find_prefetched_chunk(movie, wanted_chunk)) {
            if (!prefetch_chunk(movie, wanted_chunk)) {
                break;
            }
            loaded++;
            if (loaded >= max_new_chunks) {
                break;
            }
        }
    }
}

static int prefetch_budget_for_state(bool paused, uint32_t spare_ms)
{
    if (paused) {
        return PREFETCH_CHUNK_COUNT;
    }
    if (spare_ms > 10) {
        return 1;
    }
    return 0;
}
 
static int prefetch_target_chunk(const Movie *movie)
{
    if (!movie) {
        return -1;
    }
    if (movie->loaded_chunk < 0) {
        return -1;
    }
    if ((uint32_t) movie->loaded_chunk >= movie->header.chunk_count) {
        return -1;
    }
    return movie->loaded_chunk;
}

static void prefetch_tick(Movie *movie, bool paused, uint32_t spare_ms)
{
    int current_chunk = prefetch_target_chunk(movie);
    int budget = prefetch_budget_for_state(paused, spare_ms);
    if (current_chunk >= 0 && budget > 0) {
        prefetch_ahead(movie, current_chunk, budget);
    }
}
 
static void init_prefetch(Movie *movie)
{
    if (movie && movie->header.chunk_count > 1) {
        prefetch_tick(movie, true, 1000);
    }
}

static int movie_chunk_for_frame(const Movie *movie, uint32_t frame_index)
{
    uint32_t left = 0;
    uint32_t right = movie->header.chunk_count;
    while (left < right) {
        uint32_t mid = left + ((right - left) / 2);
        const ChunkIndexEntry *entry = movie->chunk_index + mid;
        if (frame_index < entry->first_frame) {
            right = mid;
        } else if (frame_index >= entry->first_frame + entry->frame_count) {
            left = mid + 1;
        } else {
            return (int) mid;
        }
    }
    return -1;
}

static bool decode_to_frame(Movie *movie, uint32_t frame_index)
{
    int chunk_index = movie_chunk_for_frame(movie, frame_index);
    const ChunkIndexEntry *entry;
    uint32_t local_index;
    uint32_t replay_index;
    bool have_contiguous_seed = false;

    if (chunk_index < 0) {
        return false;
    }
    if (!load_chunk(movie, chunk_index)) {
        return false;
    }
    entry = movie->chunk_index + chunk_index;
    local_index = frame_index - entry->first_frame;
    have_contiguous_seed = (
        chunk_index > 0 &&
        entry->first_frame > 0 &&
        frame_index == entry->first_frame &&
        movie->current_frame + 1 == entry->first_frame
    );
    if (movie->decoded_local_frame < 0 && chunk_index > 0 && entry->frame_count > 0 && movie->chunk_bytes[0] != 'I') {
        if (!have_contiguous_seed) {
            uint32_t seed_frame = entry->first_frame - 1;
            if (!decode_to_frame(movie, seed_frame)) {
                return false;
            }
            if (!load_chunk(movie, chunk_index)) {
                return false;
            }
            entry = movie->chunk_index + chunk_index;
            local_index = frame_index - entry->first_frame;
        }
    }
    if (movie->decoded_local_frame > (int) local_index) {
        movie->decoded_local_frame = -1;
    }
    for (replay_index = (uint32_t) (movie->decoded_local_frame + 1); replay_index <= local_index; ++replay_index) {
        size_t start = movie->frame_offsets[replay_index];
        size_t end = (replay_index + 1 < entry->frame_count)
            ? movie->frame_offsets[replay_index + 1]
            : movie->chunk_size;
        if (!decode_frame_record(movie, movie->chunk_bytes + start, end - start)) {
            return false;
        }
        movie->decoded_local_frame = (int) replay_index;
    }
    movie->current_frame = frame_index;
    return true;
}

static SubtitleCue *load_subtitles(FILE *file, const MovieHeader *header)
{
    SubtitleCue *cues = NULL;
    uint32_t index;
    if (!header->subtitle_count) {
        return NULL;
    }
    cues = (SubtitleCue *) calloc(header->subtitle_count, sizeof(SubtitleCue));
    if (!cues) {
        return NULL;
    }
    if (fseek(file, (long) header->subtitle_offset, SEEK_SET) != 0) {
        free(cues);
        return NULL;
    }
    for (index = 0; index < header->subtitle_count; ++index) {
        uint8_t meta[10];
        uint16_t text_len;
        if (fread(meta, 1, sizeof(meta), file) != sizeof(meta)) {
            free(cues);
            return NULL;
        }
        cues[index].start_ms = read_le32(meta);
        cues[index].end_ms = read_le32(meta + 4);
        text_len = read_le16(meta + 8);
        cues[index].text = (char *) malloc(text_len + 1);
        if (!cues[index].text) {
            free(cues);
            return NULL;
        }
        if (fread(cues[index].text, 1, text_len, file) != text_len) {
            free(cues[index].text);
            free(cues);
            return NULL;
        }
        cues[index].text[text_len] = '\0';
    }
    return cues;
}

static bool load_movie(const char *path, Movie *movie)
{
    size_t framebuffer_words;
    uint32_t chunk_index_read;
    memset(movie, 0, sizeof(*movie));
    movie->loaded_chunk = -1;
    {
        int prefetch_index;
        for (prefetch_index = 0; prefetch_index < PREFETCH_CHUNK_COUNT; ++prefetch_index) {
            movie->prefetched[prefetch_index].chunk_index = -1;
        }
    }
    movie->decoded_local_frame = -1;

    movie->file = fopen(path, "rb");
    if (!movie->file) {
        return false;
    }
    if (fread(&movie->header, 1, sizeof(movie->header), movie->file) != sizeof(movie->header)) {
        return false;
    }
    if (memcmp(movie->header.magic, "NVP1", 4) != 0) {
        return false;
    }
    if (movie->header.version < 2 || movie->header.version > 5) {
        return false;
    }
    movie->chunk_index = (ChunkIndexEntry *) calloc(movie->header.chunk_count, sizeof(ChunkIndexEntry));
    if (!movie->chunk_index) {
        return false;
    }
    if (fseek(movie->file, (long) movie->header.index_offset, SEEK_SET) != 0) {
        return false;
    }
    if (movie->header.version >= 3) {
        if (fread(movie->chunk_index, sizeof(ChunkIndexEntry), movie->header.chunk_count, movie->file) != movie->header.chunk_count) {
            return false;
        }
    } else {
        for (chunk_index_read = 0; chunk_index_read < movie->header.chunk_count; ++chunk_index_read) {
            LegacyChunkIndexEntry legacy_entry;
            if (fread(&legacy_entry, sizeof(legacy_entry), 1, movie->file) != 1) {
                return false;
            }
            movie->chunk_index[chunk_index_read].offset = legacy_entry.offset;
            movie->chunk_index[chunk_index_read].packed_size = legacy_entry.packed_size;
            movie->chunk_index[chunk_index_read].unpacked_size = legacy_entry.unpacked_size;
            movie->chunk_index[chunk_index_read].first_frame = legacy_entry.first_frame;
            movie->chunk_index[chunk_index_read].frame_count = legacy_entry.frame_count;
            movie->chunk_index[chunk_index_read].frame_table_offset = 0;
        }
    }
    movie->subtitles = load_subtitles(movie->file, &movie->header);
    if (movie->header.subtitle_count && !movie->subtitles) {
        return false;
    }
    framebuffer_words = (size_t) movie->header.video_width * movie->header.video_height;
    movie->framebuffer = (uint16_t *) calloc(framebuffer_words, sizeof(uint16_t));
    movie->previous_framebuffer = (uint16_t *) calloc(framebuffer_words, sizeof(uint16_t));
    movie->block_scratch = (uint16_t *) malloc(
        (size_t) ((movie->header.video_width > (movie->header.block_size * movie->header.block_size))
            ? movie->header.video_width
            : (movie->header.block_size * movie->header.block_size)) * sizeof(uint16_t)
    );
    if (!movie->framebuffer || !movie->previous_framebuffer || !movie->block_scratch) {
        return false;
    }
    movie->frame_surface = SDL_CreateRGBSurfaceFrom(
        movie->framebuffer,
        movie->header.video_width,
        movie->header.video_height,
        16,
        movie->header.video_width * 2,
        0xF800, 0x07E0, 0x001F, 0
    );
    if (!movie->frame_surface) {
        return false;
    }
    if (!decode_to_frame(movie, 0)) {
        return false;
    }
    init_prefetch(movie);
    return true;
}

static bool key_pressed_edge(t_key key, bool *previous_state)
{
    bool current_state = isKeyPressed(key) ? true : false;
    bool pressed = current_state && !(*previous_state);
    *previous_state = current_state;
    return pressed;
}

static int compare_movie_files(const void *lhs, const void *rhs)
{
    const MovieFile *a = (const MovieFile *) lhs;
    const MovieFile *b = (const MovieFile *) rhs;
    return strcmp(a->name, b->name);
}

static MovieFile *scan_movies(const char *directory, size_t *out_count)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    MovieFile *files;
    size_t count = 0;
    if (!dir) {
        *out_count = 0;
        return NULL;
    }
    files = (MovieFile *) calloc(PICKER_MAX_FILES, sizeof(MovieFile));
    if (!files) {
        closedir(dir);
        *out_count = 0;
        return NULL;
    }
    while ((entry = readdir(dir)) && count < PICKER_MAX_FILES) {
        char joined[MAX_PATH_LEN];
        int joined_len;
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!has_suffix(entry->d_name, ".nvp") && !has_suffix(entry->d_name, ".nvp.tns")) {
            continue;
        }
        joined_len = snprintf(joined, sizeof(joined), "%s/%s", directory, entry->d_name);
        if (joined_len < 0 || (size_t) joined_len >= sizeof(joined)) {
            continue;
        }
        files[count].name = display_name_for_movie(entry->d_name);
        files[count].path = dup_string(joined);
        if (!files[count].name || !files[count].path) {
            closedir(dir);
            free_movie_files(files, count + 1);
            *out_count = 0;
            return NULL;
        }
        count++;
    }
    closedir(dir);
    qsort(files, count, sizeof(MovieFile), compare_movie_files);
    *out_count = count;
    return files;
}

static const char *active_subtitle(const Movie *movie, uint32_t now_ms)
{
    uint32_t index;
    for (index = 0; index < movie->header.subtitle_count; ++index) {
        if (now_ms >= movie->subtitles[index].start_ms && now_ms <= movie->subtitles[index].end_ms) {
            return movie->subtitles[index].text;
        }
    }
    return NULL;
}

static void draw_outlined_text(SDL_Surface *surface, nSDL_Font *white_font, nSDL_Font *outline_font, int x, int y, const char *text)
{
    nSDL_DrawString(surface, outline_font, x - 1, y, text);
    nSDL_DrawString(surface, outline_font, x + 1, y, text);
    nSDL_DrawString(surface, outline_font, x, y - 1, text);
    nSDL_DrawString(surface, outline_font, x, y + 1, text);
    nSDL_DrawString(surface, white_font, x, y, text);
}

static int wrap_subtitle(nSDL_Font *font, const char *text, int max_width, char lines[MAX_SUBTITLE_LINES][MAX_SUBTITLE_LINE_LEN])
{
    char current[MAX_SUBTITLE_LINE_LEN];
    size_t pos = 0;
    int line_count = 0;
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

static SDL_Rect progress_bar_rect(void)
{
    SDL_Rect rect = {16, SCREEN_H - 14, SCREEN_W - 32, 6};
    return rect;
}

static void draw_cursor(SDL_Surface *screen, int x, int y)
{
    SDL_Rect black_h = {(Sint16) (x - 6), (Sint16) (y - 1), 13, 3};
    SDL_Rect black_v = {(Sint16) (x - 1), (Sint16) (y - 6), 3, 13};
    SDL_Rect white_h = {(Sint16) (x - 5), (Sint16) y, 11, 1};
    SDL_Rect white_v = {(Sint16) x, (Sint16) (y - 5), 1, 11};
    SDL_Rect center = {(Sint16) (x - 1), (Sint16) (y - 1), 3, 3};
    SDL_FillRect(screen, &black_h, SDL_MapRGB(screen->format, 0, 0, 0));
    SDL_FillRect(screen, &black_v, SDL_MapRGB(screen->format, 0, 0, 0));
    SDL_FillRect(screen, &white_h, SDL_MapRGB(screen->format, 255, 255, 255));
    SDL_FillRect(screen, &white_v, SDL_MapRGB(screen->format, 255, 255, 255));
    SDL_FillRect(screen, &center, SDL_MapRGB(screen->format, 255, 255, 255));
}

static int subtitle_scale_num(int subtitle_size)
{
    static const int numerators[4] = {1, 4, 5, 2};
    subtitle_size = clamp_int(subtitle_size, 0, 3);
    return numerators[subtitle_size];
}

static int subtitle_scale_den(int subtitle_size)
{
    static const int denominators[4] = {1, 3, 3, 1};
    subtitle_size = clamp_int(subtitle_size, 0, 3);
    return denominators[subtitle_size];
}

static void subtitle_fonts_for_size(const Fonts *fonts, int subtitle_size, nSDL_Font **white_font, nSDL_Font **outline_font)
{
    (void) subtitle_size;
    *white_font = fonts->subtitle_white;
    *outline_font = fonts->subtitle_outline;
}

static void compute_video_rects(const Movie *movie, ScaleMode scale_mode, SDL_Rect *src, SDL_Rect *dst)
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
        dst->x = (SCREEN_W - dst->w) / 2;
        dst->y = (SCREEN_H - dst->h) / 2;
        return;
    }

    if (scale_mode == SCALE_FILL) {
        dst->x = 0;
        dst->y = 0;
        dst->w = SCREEN_W;
        dst->h = SCREEN_H;
        if (video_aspect > screen_aspect) {
            src->w = (Uint16) ((double) movie->header.video_height * screen_aspect);
            src->x = (movie->header.video_width - src->w) / 2;
        } else {
            src->h = (Uint16) ((double) movie->header.video_width / screen_aspect);
            src->y = (movie->header.video_height - src->h) / 2;
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
        dst->x = 0;
        dst->y = (SCREEN_H - dst->h) / 2;
    } else {
        dst->h = SCREEN_H;
        dst->w = (Uint16) ((double) SCREEN_H * video_aspect);
        dst->x = (SCREEN_W - dst->w) / 2;
        dst->y = 0;
    }
}

static const char *scale_mode_text(ScaleMode scale_mode)
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

static void draw_mode_badge(SDL_Surface *screen, const Fonts *fonts, ScaleMode scale_mode)
{
    const char *label = scale_mode_text(scale_mode);
    int text_w = nSDL_GetStringWidth(fonts->white, label);
    SDL_Rect badge = {(Sint16) (SCREEN_W - text_w - 18), 8, (Uint16) (text_w + 10), 16};
    SDL_FillRect(screen, &badge, SDL_MapRGB(screen->format, 18, 18, 24));
    nSDL_DrawString(screen, fonts->white, badge.x + 5, badge.y + 4, label);
}

static void draw_scaled_outlined_text(
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

static void draw_subtitle(SDL_Surface *screen, const Fonts *fonts, const SDL_Rect *video_rect, const char *text, bool overlay_visible, int subtitle_size)
{
    char lines[MAX_SUBTITLE_LINES][MAX_SUBTITLE_LINE_LEN];
    int line_count;
    int line_index;
    int base_y;
    int max_width = video_rect->w - 12;
    int bottom_limit = overlay_visible ? (SCREEN_H - UI_BAR_H - 8) : (video_rect->y + video_rect->h - 8);
    nSDL_Font *white_font;
    nSDL_Font *outline_font;
    int scale_num;
    int scale_den;
    int line_height;
    if (!text || !*text) {
        return;
    }
    subtitle_size = clamp_int(subtitle_size, 0, 3);
    subtitle_fonts_for_size(fonts, subtitle_size, &white_font, &outline_font);
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
    line_count = wrap_subtitle(white_font, text, (max_width * scale_den) / scale_num, lines);
    if (line_count <= 0) {
        return;
    }
    base_y = bottom_limit - (line_count * (line_height + 2));
    if (base_y < video_rect->y + 4) {
        base_y = video_rect->y + 4;
    }
    for (line_index = 0; line_index < line_count; ++line_index) {
        int width = (nSDL_GetStringWidth(white_font, lines[line_index]) * scale_num) / scale_den;
        int x = video_rect->x + (video_rect->w - width) / 2;
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

static void draw_progress(
    SDL_Surface *screen,
    const Fonts *fonts,
    const Movie *movie,
    uint32_t current_ms,
    const PointerState *pointer
)
{
    SDL_Rect overlay = {0, SCREEN_H - UI_BAR_H, SCREEN_W, UI_BAR_H};
    SDL_Rect bar_back = progress_bar_rect();
    SDL_Rect bar_front = bar_back;
    int index;
    char current_text[24];
    char total_text[24];
    char left_text[56];
    char hint_text[80];
    uint32_t duration_ms = movie_duration_ms(movie);
    snprintf(hint_text, sizeof(hint_text), "CLICK SEEK  L/R +/-5s  TAB STEP  / MODE  +/- SUB");
    SDL_FillRect(screen, &overlay, SDL_MapRGB(screen->format, 0, 0, 0));
    SDL_FillRect(screen, &bar_back, SDL_MapRGB(screen->format, 52, 52, 68));
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        if (movie->prefetched[index].chunk_index >= 0) {
            const ChunkIndexEntry *entry = movie->chunk_index + movie->prefetched[index].chunk_index;
            SDL_Rect prefetch_rect = bar_back;
            prefetch_rect.x = bar_back.x + (int) (((uint64_t) bar_back.w * entry->first_frame) / movie->header.frame_count);
            prefetch_rect.w = (Uint16) (((uint64_t) bar_back.w * entry->frame_count) / movie->header.frame_count);
            if (prefetch_rect.w <= 0) {
                prefetch_rect.w = 1;
            }
            SDL_FillRect(screen, &prefetch_rect, SDL_MapRGB(screen->format, 104, 104, 116));
        }
    }
    if (duration_ms > 0) {
        bar_front.w = (Uint16) (((uint64_t) bar_back.w * current_ms) / duration_ms);
    }
    SDL_FillRect(screen, &bar_front, SDL_MapRGB(screen->format, 32, 182, 255));
    if (pointer && pointer->visible) {
        SDL_Rect marker = {(Sint16) clamp_int(pointer->x, bar_back.x, bar_back.x + bar_back.w - 1), (Sint16) (bar_back.y - 2), 1, 10};
        SDL_FillRect(screen, &marker, SDL_MapRGB(screen->format, 255, 255, 255));
    }
    format_clock(current_ms, current_text, sizeof(current_text));
    format_clock(duration_ms, total_text, sizeof(total_text));
    snprintf(left_text, sizeof(left_text), "%s / %s", current_text, total_text);
    nSDL_DrawString(screen, fonts->white, 12, SCREEN_H - UI_BAR_H + 6, left_text);
    nSDL_DrawString(
        screen,
        fonts->white,
        (SCREEN_W - nSDL_GetStringWidth(fonts->white, hint_text)) / 2,
        SCREEN_H - UI_BAR_H + 18,
        hint_text
    );
}

static void render_movie(SDL_Surface *screen, const Fonts *fonts, Movie *movie, bool paused, bool show_ui, ScaleMode scale_mode, int subtitle_size, const PointerState *pointer)
{
    SDL_Rect src;
    SDL_Rect dst;
    uint32_t current_ms = movie_frame_time_ms(movie, movie->current_frame);
    const char *subtitle = active_subtitle(movie, current_ms);
    compute_video_rects(movie, scale_mode, &src, &dst);
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
    if (src.w == movie->header.video_width && src.h == movie->header.video_height &&
        dst.w == movie->header.video_width && dst.h == movie->header.video_height) {
        SDL_BlitSurface(movie->frame_surface, NULL, screen, &dst);
    } else {
        SDL_SoftStretch(movie->frame_surface, &src, screen, &dst);
    }
    draw_subtitle(screen, fonts, &dst, subtitle, show_ui, subtitle_size);
    if (show_ui) {
        draw_mode_badge(screen, fonts, scale_mode);
        draw_progress(screen, fonts, movie, current_ms, pointer);
        if (pointer && pointer->visible) {
            draw_cursor(screen, pointer->x, pointer->y);
        }
    }
    SDL_Flip(screen);
}

static void render_picker(SDL_Surface *screen, const Fonts *fonts, MovieFile *files, size_t count, size_t selected)
{
    size_t start_index;
    size_t end_index;
    size_t index;
    int y = 52;
    SDL_Rect header = {0, 0, SCREEN_W, 38};
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 8, 10, 14));
    SDL_FillRect(screen, &header, SDL_MapRGB(screen->format, 12, 18, 28));
    nSDL_DrawString(screen, fonts->white, 10, 10, "ND Video Player");
    nSDL_DrawString(screen, fonts->white, 10, 24, "ENTER open   UP/DOWN choose   ESC exit");
    if (count == 0) {
        nSDL_DrawString(screen, fonts->white, 10, 54, "No .nvp or .nvp.tns files found");
        SDL_Flip(screen);
        return;
    }
    start_index = selected > (PICKER_VISIBLE_ROWS / 2) ? selected - (PICKER_VISIBLE_ROWS / 2) : 0;
    if (start_index + PICKER_VISIBLE_ROWS > count) {
        start_index = count > PICKER_VISIBLE_ROWS ? count - PICKER_VISIBLE_ROWS : 0;
    }
    end_index = start_index + PICKER_VISIBLE_ROWS;
    if (end_index > count) {
        end_index = count;
    }
    for (index = start_index; index < end_index && y < SCREEN_H - 20; ++index) {
        SDL_Rect row = {8, (Sint16) (y - 4), SCREEN_W - 16, 18};
        if (index == selected) {
            SDL_Rect accent = {8, (Sint16) (y - 4), 4, 18};
            SDL_FillRect(screen, &row, SDL_MapRGB(screen->format, 26, 118, 180));
            SDL_FillRect(screen, &accent, SDL_MapRGB(screen->format, 32, 182, 255));
            nSDL_DrawString(screen, fonts->white, 24, y, files[index].name);
        } else {
            SDL_FillRect(screen, &row, SDL_MapRGB(screen->format, 16, 20, 28));
            nSDL_DrawString(screen, fonts->white, 12, y, files[index].name);
        }
        y += 20;
    }
    {
        char footer[32];
        snprintf(footer, sizeof(footer), "%lu file(s)", (unsigned long) count);
        nSDL_DrawString(screen, fonts->white, SCREEN_W - 10 - nSDL_GetStringWidth(fonts->white, footer), SCREEN_H - 16, footer);
    }
    SDL_Flip(screen);
}

static void strip_filename(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash) {
        slash = strrchr(path, '\\');
    }
    if (slash) {
        *slash = '\0';
    } else {
        strcpy(path, ".");
    }
}

static int pick_movie(SDL_Surface *screen, const Fonts *fonts, const char *directory, char *selected_path, size_t selected_size)
{
    bool prev_up = false;
    bool prev_down = false;
    bool prev_enter = false;
    bool prev_esc = false;
    MovieFile *files;
    size_t count = 0;
    size_t selected = 0;

    files = scan_movies(directory, &count);
    while (1) {
        render_picker(screen, fonts, files, count, selected);
        if (key_pressed_edge(KEY_NSPIRE_UP, &prev_up) && selected > 0) {
            selected--;
        }
        if (key_pressed_edge(KEY_NSPIRE_DOWN, &prev_down) && selected + 1 < count) {
            selected++;
        }
        if (key_pressed_edge(KEY_NSPIRE_ENTER, &prev_enter) && count > 0) {
            strncpy(selected_path, files[selected].path, selected_size - 1);
            selected_path[selected_size - 1] = '\0';
            free_movie_files(files, count);
            return 0;
        }
        if (key_pressed_edge(KEY_NSPIRE_ESC, &prev_esc)) {
            free_movie_files(files, count);
            return -1;
        }
        msleep(16);
    }
}

static void clamp_seek(Movie *movie, int32_t delta_ms)
{
    int64_t current_ms = (int64_t) movie_frame_time_ms(movie, movie->current_frame);
    int64_t target_ms = current_ms + delta_ms;
    uint32_t duration_ms = movie_duration_ms(movie);
    uint32_t target_frame;
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
    decode_to_frame(movie, target_frame);
}

static void seek_to_ratio(Movie *movie, uint32_t numerator, uint32_t denominator)
{
    uint32_t duration_ms;
    uint32_t target_ms;
    uint32_t target_frame;
    if (denominator == 0 || movie->header.frame_count == 0) {
        return;
    }
    duration_ms = movie_duration_ms(movie);
    target_ms = (uint32_t) (((uint64_t) duration_ms * numerator) / denominator);
    target_frame = movie_frames_from_ms(movie, target_ms);
    if (target_frame >= movie->header.frame_count) {
        target_frame = movie->header.frame_count - 1;
    }
    decode_to_frame(movie, target_frame);
}

static int play_movie(SDL_Surface *screen, const Fonts *fonts, const char *path)
{
    Movie movie;
    ClockState playback_clock_state;
    bool playback_clock_active = false;
    bool prev_enter = false;
    bool prev_left = false;
    bool prev_right = false;
    bool prev_tab = false;
    bool prev_esc = false;
    bool prev_click = false;
    bool prev_divide = false;
    bool prev_plus = false;
    bool prev_minus = false;
    bool paused = false;
    uint32_t frame_ms;
    uint32_t next_frame_due;
    uint32_t playback_anchor_ms;
    uint32_t playback_anchor_frame;
    uint32_t ui_visible_until;
    int result = 0;
    ScaleMode scale_mode = SCALE_FIT;
    int subtitle_size = 2;
    PointerState pointer;

    if (!load_movie(path, &movie)) {
        if (playback_clock_active) {
            clock_state_restore(&playback_clock_state);
        }
        show_msgbox("ND Video Player", "Failed to open movie file.");
        return -1;
    }
    if (ENABLE_STARTUP_OVERCLOCK) {
        clock_state_init(&playback_clock_state);
        clock_state_apply_boost(&playback_clock_state);
        playback_clock_active = true;
    }
    pointer_init(&pointer);
    frame_ms = movie_frame_interval_ms(&movie);
    playback_anchor_ms = monotonic_clock_now_ms();
    playback_anchor_frame = movie.current_frame;
    next_frame_due = playback_anchor_ms + movie_frame_time_ms(&movie, 1);
    ui_visible_until = playback_anchor_ms + POINTER_UI_TIMEOUT_MS;
    {
        prefetch_tick(&movie, true, 1000);
    }

    while (1) {
        bool pointer_click = pointer_update(&pointer);
        uint32_t now_ms = monotonic_clock_now_ms();
        bool show_ui = paused || (now_ms <= ui_visible_until);
        bool enter_edge = key_pressed_edge(KEY_NSPIRE_ENTER, &prev_enter);
        bool click_edge = key_pressed_edge(KEY_NSPIRE_CLICK, &prev_click);

        if (pointer.moved || pointer_click) {
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            show_ui = true;
        }
        if (click_edge) {
            pointer_click = true;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            show_ui = true;
        }
        if (key_pressed_edge(KEY_NSPIRE_ESC, &prev_esc)) {
            break;
        }
        if (enter_edge) {
            if (movie.current_frame + 1 >= movie.header.frame_count) {
                decode_to_frame(&movie, 0);
                paused = false;
            } else {
                paused = !paused;
            }
            reset_playback_timeline(&movie, now_ms, &playback_anchor_ms, &playback_anchor_frame, &next_frame_due);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (key_pressed_edge(KEY_NSPIRE_LEFT, &prev_left)) {
            clamp_seek(&movie, -SEEK_STEP_MS);
            paused = true;
            reset_playback_timeline(&movie, now_ms, &playback_anchor_ms, &playback_anchor_frame, &next_frame_due);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (key_pressed_edge(KEY_NSPIRE_RIGHT, &prev_right)) {
            clamp_seek(&movie, SEEK_STEP_MS);
            paused = true;
            reset_playback_timeline(&movie, now_ms, &playback_anchor_ms, &playback_anchor_frame, &next_frame_due);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (key_pressed_edge(KEY_NSPIRE_DIVIDE, &prev_divide)) {
            scale_mode = (ScaleMode) ((scale_mode + 1) % 4);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (key_pressed_edge(KEY_NSPIRE_PLUS, &prev_plus) && subtitle_size < 3) {
            subtitle_size++;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (key_pressed_edge(KEY_NSPIRE_MINUS, &prev_minus) && subtitle_size > 0) {
            subtitle_size--;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (paused && key_pressed_edge(KEY_NSPIRE_TAB, &prev_tab) && movie.current_frame + 1 < movie.header.frame_count) {
            if (!decode_to_frame(&movie, movie.current_frame + 1)) {
                show_msgbox("ND Video Player", "Movie decode failed.");
                result = -1;
                break;
            }
            reset_playback_timeline(&movie, now_ms, &playback_anchor_ms, &playback_anchor_frame, &next_frame_due);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (pointer_click) {
            SDL_Rect bar = progress_bar_rect();
            if (show_ui && pointer.x >= bar.x && pointer.x < bar.x + bar.w && pointer.y >= bar.y - 3 && pointer.y <= bar.y + (int) bar.h + 3) {
                seek_to_ratio(&movie, (uint32_t) (pointer.x - bar.x), (uint32_t) bar.w);
                reset_playback_timeline(&movie, now_ms, &playback_anchor_ms, &playback_anchor_frame, &next_frame_due);
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            } else {
                if (movie.current_frame + 1 >= movie.header.frame_count) {
                    decode_to_frame(&movie, 0);
                    paused = false;
                } else {
                    paused = !paused;
                }
                reset_playback_timeline(&movie, now_ms, &playback_anchor_ms, &playback_anchor_frame, &next_frame_due);
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            }
        }
        if (!paused && frame_ms > 0) {
            if ((int32_t) (now_ms - next_frame_due) >= 0) {
                uint32_t elapsed_ms = now_ms - playback_anchor_ms;
                uint32_t frames_to_advance = movie_frames_from_ms(&movie, elapsed_ms);
                uint32_t target_frame = playback_anchor_frame + frames_to_advance;
                if (target_frame >= movie.header.frame_count) {
                    if (movie.current_frame + 1 < movie.header.frame_count) {
                        if (!decode_to_frame(&movie, movie.header.frame_count - 1)) {
                            show_msgbox("ND Video Player", "Movie decode failed.");
                            result = -1;
                            break;
                        }
                    }
                    paused = true;
                    ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
                } else {
                    if (target_frame > movie.current_frame && !decode_to_frame(&movie, target_frame)) {
                        show_msgbox("ND Video Player", "Movie decode failed.");
                        result = -1;
                        break;
                    }
                    next_frame_due = playback_anchor_ms + movie_frame_time_ms(&movie, (target_frame - playback_anchor_frame) + 1);
                }
            }
        }
        show_ui = paused || (monotonic_clock_now_ms() <= ui_visible_until);
        render_movie(screen, fonts, &movie, paused, show_ui, scale_mode, subtitle_size, &pointer);
        if (paused || frame_ms == 0) {
            prefetch_tick(&movie, true, 1000);
            msleep(16);
        } else {
            uint32_t after_render_ms = monotonic_clock_now_ms();
            uint32_t spare_ms = next_frame_due > after_render_ms ? (next_frame_due - after_render_ms) : 0;
            prefetch_tick(&movie, false, spare_ms);
            uint32_t wait_ms = next_frame_due > after_render_ms ? (next_frame_due - after_render_ms) : 1;
            if (wait_ms > 8) {
                wait_ms = 8;
            }
            msleep(wait_ms);
        }
    }

    destroy_movie(&movie);
    if (playback_clock_active) {
        clock_state_restore(&playback_clock_state);
    }
    return result;
}

int main(int argc, char **argv)
{
    SDL_Surface *screen;
    Fonts fonts;
    char movie_path[MAX_PATH_LEN];
    char directory[MAX_PATH_LEN];
    int result = 0;

    if (argc < 1) {
        show_msgbox("ND Video Player", "Ndless did not provide argv[0].");
        return 1;
    }

    enable_relative_paths(argv);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        show_msgbox("ND Video Player", "Failed to initialize SDL.");
        return 1;
    }
    monotonic_clock_init();
    screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, has_colors ? 16 : 8, SDL_SWSURFACE);
    if (!screen) {
        show_msgbox("ND Video Player", "Failed to create the screen surface.");
        SDL_Quit();
        monotonic_clock_shutdown();
        return 1;
    }
    if (!init_fonts(&fonts)) {
        show_msgbox("ND Video Player", "Failed to load fonts.");
        SDL_Quit();
        monotonic_clock_shutdown();
        return 1;
    }

    strncpy(directory, argv[0], sizeof(directory) - 1);
    directory[sizeof(directory) - 1] = '\0';
    strip_filename(directory);

    while (1) {
        if (argc > 1) {
            strncpy(movie_path, argv[1], sizeof(movie_path) - 1);
            movie_path[sizeof(movie_path) - 1] = '\0';
        } else {
            if (pick_movie(screen, &fonts, directory, movie_path, sizeof(movie_path)) != 0) {
                break;
            }
        }
        result = play_movie(screen, &fonts, movie_path);
        argc = 1;
        if (result != 0) {
            break;
        }
    }

    free_fonts(&fonts);
    SDL_Quit();
    monotonic_clock_shutdown();
    return result == 0 ? 0 : 1;
}
