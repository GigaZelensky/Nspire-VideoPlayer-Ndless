#include <dirent.h>
#include <libndls.h>
#include <os.h>
#include <SDL/SDL.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <zlib.h>

#include "h264bsd_decoder.h"
#include "h264bsd_util.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define UI_BAR_H 28
#define SEEK_STEP_MS 5000
#define PICKER_MAX_FILES 128
#define PICKER_VISIBLE_ROWS 9
#define MAX_PATH_LEN 512
#define MAX_SUBTITLE_LINES 3
#define MAX_SUBTITLE_LINE_LEN 96
#define APP_RAM_TARGET_BYTES (32U * 1024U * 1024U)
#define APP_RAM_HEADROOM_BYTES (2U * 1024U * 1024U)
#define PREFETCH_CHUNK_COUNT 5
#define TIMER_TICKS_PER_SEC 32768U
#define PREFETCH_MAX_TOTAL_BYTES (12U * 1024U * 1024U)
#define POINTER_FIXED_SHIFT 6
#define POINTER_UI_TIMEOUT_MS 1800U
#define POINTER_GAIN_NUM 5
#define POINTER_GAIN_DEN 4
#define PREFETCH_FILE_BLOCK_SIZE 32768U
#define PREFETCH_ACTIVE_FILE_BLOCK_SIZE 8192U
#define PREFETCH_INFLATE_OUTPUT_SLICE 2048U
#define PREFETCH_PAUSED_SLICE_MS 12U
#define PREFETCH_ACTIVE_H264_MIN_SPARE_MS 12U
#define PREFETCH_ACTIVE_H264_SLICE_MS 2U
#define H264_FRAME_RING_MAX_COUNT 160
#define H264_FRAME_RING_ALIGNMENT 32U
#define H264_FRAME_RING_BUDGET_BYTES (12U * 1024U * 1024U)
#define H264_PREFETCH_ACTIVE_MIN_SPARE_MS 14U
#define H264_PREFETCH_ACTIVE_GUARD_MS 2U
#define H264_PREFETCH_MIN_READY_FRAMES 10U
#define H264_PREFETCH_LOOKAHEAD_FRAMES 128U
#define H264_PREFETCH_ACTIVE_SCAN_MIN_FRAMES 40U
#define H264_PREFETCH_ACTIVE_SCAN_MID_FRAMES 56U
#define H264_PREFETCH_ACTIVE_SCAN_MAX_FRAMES 72U
#define H264_PREFETCH_HEAVY_FRAME_BYTES 8192U
#define H264_PREFETCH_VERY_HEAVY_FRAME_BYTES 12288U
#define H264_PREFETCH_AVG_MIN_FRAME_BYTES 2048U
#define H264_PREFETCH_ABOVE_AVG_NUMERATOR 9U
#define H264_PREFETCH_ABOVE_AVG_DENOMINATOR 8U
#define H264_PREFETCH_NEAR_WINDOW_FRAMES 24U
#define H264_PREFETCH_ACTIVE_READY_BASE 14U
#define H264_PREFETCH_ACTIVE_READY_MID 18U
#define H264_PREFETCH_ACTIVE_READY_HIGH 22U
#define H264_PREFETCH_ACTIVE_READY_MAX 28U
#define H264_FRAME_RING_USEFUL_MAX (H264_PREFETCH_ACTIVE_READY_MAX + H264_PREFETCH_NEXT_CHUNK_BRIDGE_FRAMES)
#define H264_PREFETCH_MAX_PROXIMITY_BONUS 4U
#define H264_PREFETCH_ACTIVE_LOW_WATERMARK 6U
#define H264_PREFETCH_ACTIVE_MAX_DECODES_PER_TICK 1U
#define H264_PREFETCH_ACTIVE_MAX_DECODES_RECOVERY 2U
#define H264_PREFETCH_NEXT_CHUNK_GUARD_FRAMES 12U
#define H264_PREFETCH_NEXT_CHUNK_BRIDGE_FRAMES 6U
#define H264_BOUNDARY_MISS_WINDOW_FRAMES 4U
#define H264_PREFETCH_IO_PRIORITY_SLICE_MS 4U
#define H264_PREFETCH_DECODE_GUARD_DEFAULT_MS 24U
#define H264_PREFETCH_DECODE_GUARD_MIN_MS 18U
#define H264_PREFETCH_DECODE_GUARD_MAX_MS 40U
#define H264_FOREGROUND_DECODE_SOFT_MS 18U
#define H264_FOREGROUND_DECODE_HARD_MS 26U
#define H264_ACTIVE_PREFETCH_BACKOFF_LIGHT 2U
#define H264_ACTIVE_PREFETCH_BACKOFF_HEAVY 4U
#define H264_SAME_CHUNK_RESCUE_MIN_SPARE_MS 10U
#define FRAME_PACING_SPIN_MS 2U
#define MONOTONIC_TIMER_VALUE_ADDR 0x900C0004U
#define MONOTONIC_TIMER_CONTROL_ADDR 0x900C0008U
#define MONOTONIC_TIMER_CLOCK_SOURCE_ADDR 0x900C0080U
#define MONOTONIC_TIMER_CLOCK_SOURCE_32768HZ 0x0AU
#define MONOTONIC_TIMER_CONTROL_ENABLE_32BIT 0x82U
#define MONOTONIC_TIMER_MAX_DELTA_TICKS (TIMER_TICKS_PER_SEC * 10U)
#define DEBUG_RING_SIZE 2048
#define DEBUG_LINE_LEN 160
#define DEBUG_SNAPSHOT_INTERVAL_MS 1000U
#define DEBUG_TRACE_FOREGROUND_MS 12U
#define DEBUG_TRACE_PREFETCH_MS 10U
#define HISTORY_FILE_NAME "ndhistory.ts.tns"
#define HISTORY_MAX_ENTRIES 5
#define RESUME_MIN_MS 5000U
#define RESUME_CLEAR_TAIL_MS 3000U
#define STATUS_OVERLAY_MS 1200U
#define MOVIE_VERSION_H264 9
#define H264_CLIP_OFFSET 384
#define H264_CLIP_TABLE_SIZE 1024

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
    uint32_t cue_start;
    uint32_t cue_count;
} SubtitleTrack;

typedef struct {
    char *name;
    char *path;
    uint32_t resume_frame;
    bool has_resume;
} MovieFile;

typedef struct {
    char *path;
    uint32_t frame;
} HistoryEntry;

typedef struct {
    HistoryEntry entries[HISTORY_MAX_ENTRIES];
    size_t count;
} HistoryStore;

typedef struct {
    nSDL_Font *white;
    nSDL_Font *outline;
    nSDL_Font *subtitle_white[NSP_NUMFONTS];
    nSDL_Font *subtitle_outline[NSP_NUMFONTS];
} Fonts;

typedef enum {
    SCALE_FIT = 0,
    SCALE_FILL = 1,
    SCALE_STRETCH = 2,
    SCALE_NATIVE = 3,
} ScaleMode;

typedef enum {
    SUBTITLE_POS_BAR_BOTTOM = 0,
    SUBTITLE_POS_VIDEO_BOTTOM,
    SUBTITLE_POS_VIDEO_TOP,
    SUBTITLE_POS_BAR_TOP,
    SUBTITLE_POS_COUNT,
} SubtitlePlacement;

typedef struct {
    uint8_t numerator;
    uint8_t denominator;
    const char *label;
} PlaybackRate;

typedef enum {
    MEMORY_OVERLAY_OFF = 0,
    MEMORY_OVERLAY_ALWAYS,
} MemoryOverlayMode;

typedef struct {
    bool valid;
    size_t used_bytes;
    size_t prefetched_bytes;
    size_t free_bytes;
    size_t total_bytes;
    unsigned percent_used;
} MemoryStats;

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

typedef enum {
    PREFETCH_IDLE = 0,
    PREFETCH_READING,
    PREFETCH_INFLATING,
    PREFETCH_READY,
} PrefetchState;

typedef struct {
    uint8_t *chunk_storage;
    size_t chunk_storage_size;
    int chunk_index;
    PrefetchState state;
    size_t read_offset;
    size_t inflate_write_offset;
    bool inflate_initialized;
    z_stream stream;
    uint8_t input_buffer[PREFETCH_FILE_BLOCK_SIZE];
} PrefetchedChunk;

typedef struct {
    uint8_t *data;
    uint8_t *allocation;
    uint32_t frame_index;
    bool valid;
} H264FrameSlot;

typedef struct {
    FILE *file;
    MovieHeader header;
    ChunkIndexEntry *chunk_index;
    SubtitleCue *subtitles;
    SubtitleTrack *subtitle_tracks;
    uint16_t subtitle_track_count;
    uint16_t selected_subtitle_track;
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
    storage_t *h264_decoder;
    uint32_t h264_full_width;
    uint32_t h264_full_height;
    uint32_t h264_crop_left;
    uint32_t h264_crop_top;
    uint32_t h264_crop_width;
    uint32_t h264_crop_height;
    bool h264_headers_ready;
    bool h264_decoder_initialized;
    bool h264_chunk_dirty;
    uint16_t h264_prefetch_decode_avg_ms;
    uint16_t h264_prefetch_decode_peak_ms;
    uint16_t h264_foreground_decode_avg_ms;
    uint16_t h264_foreground_decode_peak_ms;
    uint8_t h264_active_prefetch_backoff;
    uint32_t diag_last_snapshot_ms;
    uint32_t diag_prefetch_tick_count;
    uint32_t diag_active_prefetch_tick_count;
    uint32_t diag_prefetch_suppressed_count;
    uint32_t diag_prefetch_backoff_skip_count;
    uint32_t diag_io_priority_count;
    uint32_t diag_prefetch_frame_decode_count;
    uint32_t diag_prefetch_frame_decode_fail_count;
    uint32_t diag_foreground_decode_count;
    uint32_t diag_foreground_ring_hit_count;
    uint32_t diag_foreground_direct_decode_count;
    uint32_t diag_lag_event_count;
    uint32_t diag_lag_frame_total;
    uint32_t diag_max_lag_frames;
    uint32_t diag_max_late_ms;
    uint32_t diag_max_spare_ms;
    uint32_t diag_chunk_load_sync_count;
    uint32_t diag_chunk_load_prefetched_count;
    uint32_t diag_prefetch_read_ops;
    uint32_t diag_prefetch_inflate_ops;
    uint32_t diag_prefetch_read_bytes;
    uint32_t diag_prefetch_inflate_bytes;
    uint32_t diag_h264_replay_count;
    uint32_t diag_h264_replay_frames_total;
    uint32_t diag_h264_replay_max_distance;
    uint32_t diag_bg_same_chunk_count;
    uint32_t diag_bg_next_chunk_count;
    uint16_t diag_bg_same_chunk_avg_ms;
    uint16_t diag_bg_next_chunk_avg_ms;
    uint32_t diag_ring_miss_not_cached_count;
    uint32_t diag_ring_miss_not_contiguous_count;
    uint32_t diag_ring_miss_chunk_not_ready_count;
    uint32_t diag_ring_miss_slot_unavailable_count;
    uint32_t diag_chunk_boundary_miss_count;
    uint32_t diag_bg_chunk_cross_blocked_count;
    uint32_t diag_last_spare_ms;
    size_t h264_frame_bytes;
    size_t h264_frame_ring_capacity;
    H264FrameSlot h264_frame_ring[H264_FRAME_RING_MAX_COUNT];
} Movie;

typedef struct {
    bool initialized;
    int32_t y_base[256];
    int32_t u_to_blue[256];
    int32_t u_to_green[256];
    int32_t v_to_red[256];
    int32_t v_to_green[256];
    uint8_t clip[H264_CLIP_TABLE_SIZE];
    uint16_t red565[256];
    uint16_t green565[256];
    uint16_t blue565[256];
} H264ColorTables;

typedef struct {
    bool initialized;
    bool using_hw_timer;
    uint32_t ticks_per_second;
    volatile unsigned *load_reg;
    volatile unsigned *value_reg;
    volatile unsigned *control_reg;
    volatile unsigned *int_clear_reg;
    volatile unsigned *bgload_reg;
    uint32_t last_value;
    uint64_t elapsed_ticks;
    unsigned original_load;
    unsigned original_control;
    unsigned original_bgload;
    volatile unsigned *speed_reg;
    unsigned original_speed;
} MonotonicClock;

static MonotonicClock g_clock;
static char g_debug_ring[DEBUG_RING_SIZE][DEBUG_LINE_LEN];
static size_t g_debug_ring_count = 0;
static size_t g_debug_ring_next = 0;
static char g_last_error_message[DEBUG_LINE_LEN];
static H264ColorTables g_h264_color_tables;
static const PlaybackRate g_playback_rates[] = {
    {1, 4, "0.25x"},
    {1, 2, "0.5x"},
    {3, 4, "0.75x"},
    {1, 1, "1.0x"},
    {5, 4, "1.25x"},
    {3, 2, "1.5x"},
    {7, 4, "1.75x"},
    {2, 1, "2.0x"},
};
static const int g_subtitle_font_choices[] = {
    NSDL_FONT_TINYTYPE,
    NSDL_FONT_VGA,
    NSDL_FONT_THIN,
    NSDL_FONT_SPACE,
    NSDL_FONT_FANTASY,
};
static const char *g_subtitle_font_names[] = {
    "Tinytype",
    "VGA",
    "Thin",
    "Space",
    "Fantasy",
};

#define PLAYBACK_RATE_DEFAULT_INDEX 3U
#define PLAYBACK_RATE_COUNT ((size_t) (sizeof(g_playback_rates) / sizeof(g_playback_rates[0])))
#define SUBTITLE_FONT_DEFAULT_INDEX 2U
#define SUBTITLE_FONT_CHOICE_COUNT ((size_t) (sizeof(g_subtitle_font_choices) / sizeof(g_subtitle_font_choices[0])))
#define SUBTITLE_FONT_OVERLAY_MS 1200U

static bool decode_to_frame(Movie *movie, uint32_t frame_index);
static bool decode_h264_frame(Movie *movie, uint32_t frame_index, bool blit_output, bool store_prefetched);
static bool load_chunk(Movie *movie, int chunk_index);
static bool prefetch_chunk(Movie *movie, int chunk_index);
static void prefetch_ahead(Movie *movie, int current_chunk, int max_new_chunks);
static void clear_prefetched_chunk(PrefetchedChunk *chunk);
static bool prefetch_finish_chunk(Movie *movie, PrefetchedChunk *chunk);
static void prefetch_do_work(Movie *movie, int current_chunk, uint32_t deadline_ms);
static void prefetch_h264_frames(Movie *movie, bool paused, uint32_t spare_ms, uint32_t deadline_ms);
static bool prefetch_one_h264_same_chunk_frame(Movie *movie, uint32_t spare_ms, uint32_t deadline_ms) __attribute__((unused));
static int movie_chunk_for_frame(const Movie *movie, uint32_t frame_index);
static int64_t h264_decoded_global_frame(const Movie *movie);
static bool next_chunk_needs_prefetch(const Movie *movie, int current_chunk);
static uint32_t next_chunk_prefetch_guard_frames(const Movie *movie, int current_chunk);
static size_t active_h264_frame_ring_capacity(const Movie *movie);
static size_t h264_prefetch_target_ready_count(const Movie *movie, uint32_t spare_ms);
static void discard_h264_frame_ring_before(Movie *movie, uint32_t first_frame_to_keep);
static void free_fonts(Fonts *fonts);
static uint32_t monotonic_clock_now_ms(void);
static void debug_dump_session(const char *path, const Movie *movie, const char *reason);
static void history_path_for_directory(const char *directory, char *history_path, size_t history_path_size);
static bool load_history_store_from_path(const char *history_path, HistoryStore *history);
static int history_find_entry_index(const HistoryStore *history, const char *movie_path);

static void debug_tracef(const char *fmt, ...)
{
    char line[DEBUG_LINE_LEN];
    size_t slot_index;
    va_list args;
    uint32_t now_ms = g_clock.initialized ? monotonic_clock_now_ms() : 0;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

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

static void debug_clear_last_error(void)
{
    g_last_error_message[0] = '\0';
}

static const char *debug_last_error(void)
{
    return g_last_error_message[0] != '\0' ? g_last_error_message : "unknown";
}

static void debug_failf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(g_last_error_message, sizeof(g_last_error_message), fmt, args);
    va_end(args);
    debug_tracef("%s", g_last_error_message);
}

static size_t total_prefetched_chunk_bytes(const Movie *movie)
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

static void clear_all_prefetched_chunks(Movie *movie)
{
    int index;

    if (!movie) {
        return;
    }
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        clear_prefetched_chunk(&movie->prefetched[index]);
    }
}

static PrefetchedChunk *find_farthest_prefetched_chunk(Movie *movie)
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

static bool ensure_prefetch_budget(Movie *movie, int requested_chunk, size_t required_bytes)
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

static void debug_log_path_for_movie(const char *movie_path, char *log_path, size_t log_path_size)
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

static void report_movie_decode_failure(const Movie *movie, const char *movie_path, const char *reason)
{
    char log_path[MAX_PATH_LEN];
    char message[192];

    if (movie) {
        debug_tracef(
            "decode failed reason=%s frame=%lu loaded_chunk=%d decoded_local=%d prefetched=%lu",
            reason ? reason : "unknown",
            (unsigned long) movie->current_frame,
            movie->loaded_chunk,
            movie->decoded_local_frame,
            (unsigned long) total_prefetched_chunk_bytes(movie)
        );
    } else {
        debug_tracef("decode failed reason=%s", reason ? reason : "unknown");
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

static void report_movie_open_failure(const char *movie_path)
{
    char log_path[MAX_PATH_LEN];
    char message[192];

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

static bool monotonic_clock_try_init_hw_timer(void)
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

static void monotonic_clock_init(void)
{
    memset(&g_clock, 0, sizeof(g_clock));
    g_clock.initialized = true;
    g_clock.ticks_per_second = TIMER_TICKS_PER_SEC;
    monotonic_clock_try_init_hw_timer();
}

static uint32_t monotonic_clock_ticks_per_second(void)
{
    if (g_clock.ticks_per_second) {
        return g_clock.ticks_per_second;
    }
    return TIMER_TICKS_PER_SEC;
}

static uint64_t monotonic_clock_now_ticks(void)
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

static uint32_t monotonic_clock_ticks_to_ms(uint64_t ticks)
{
    return (uint32_t) ((ticks * 1000ULL) / monotonic_clock_ticks_per_second());
}

static uint32_t monotonic_clock_now_ms(void)
{
    return monotonic_clock_ticks_to_ms(monotonic_clock_now_ticks());
}

static void monotonic_clock_shutdown(void)
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

static MemoryStats query_memory_stats(const Movie *movie)
{
    MemoryStats stats;
    size_t framebuffer_words;
    size_t block_scratch_words;
    size_t chunk_prefetch_bytes = 0;
    size_t h264_ring_bytes = 0;
    int index;

    memset(&stats, 0, sizeof(stats));
    if (!movie) {
        return stats;
    }

    if (movie->chunk_index) {
        stats.used_bytes += (size_t) movie->header.chunk_count * sizeof(ChunkIndexEntry);
    }
    if (movie->subtitles) {
        stats.used_bytes += (size_t) movie->header.subtitle_count * sizeof(SubtitleCue);
    }

    framebuffer_words = (size_t) movie->header.video_width * movie->header.video_height;
    if (movie->framebuffer) {
        stats.used_bytes += framebuffer_words * sizeof(uint16_t);
    }
    if (movie->previous_framebuffer) {
        stats.used_bytes += framebuffer_words * sizeof(uint16_t);
    }
    if (movie->h264_frame_bytes > 0) {
        int h264_index;
        for (h264_index = 0; h264_index < (int) active_h264_frame_ring_capacity(movie); ++h264_index) {
            if (movie->h264_frame_ring[h264_index].data) {
                stats.used_bytes += movie->h264_frame_bytes;
                h264_ring_bytes += movie->h264_frame_bytes;
            }
        }
    }

    block_scratch_words = (size_t) ((movie->header.video_width > (movie->header.block_size * movie->header.block_size))
        ? movie->header.video_width
        : (movie->header.block_size * movie->header.block_size));
    if (movie->block_scratch) {
        stats.used_bytes += block_scratch_words * sizeof(uint16_t);
    }
    if (movie->frame_offsets && movie->loaded_chunk >= 0 && (uint32_t) movie->loaded_chunk < movie->header.chunk_count) {
        stats.used_bytes += (size_t) movie->chunk_index[movie->loaded_chunk].frame_count * sizeof(uint32_t);
    }
    if (movie->chunk_storage) {
        stats.used_bytes += movie->chunk_storage_size;
    }

    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        if (movie->prefetched[index].chunk_index >= 0 && movie->prefetched[index].chunk_storage) {
            chunk_prefetch_bytes += movie->prefetched[index].chunk_storage_size;
        }
    }

    stats.prefetched_bytes = chunk_prefetch_bytes + h264_ring_bytes;
    stats.used_bytes += chunk_prefetch_bytes;
    stats.total_bytes = APP_RAM_TARGET_BYTES;
    stats.free_bytes = stats.total_bytes > stats.used_bytes
        ? (stats.total_bytes - stats.used_bytes)
        : 0;
    if (stats.total_bytes > 0) {
        stats.percent_used = (unsigned) (((stats.used_bytes * 100U) + (stats.total_bytes / 2U)) / stats.total_bytes);
        if (stats.percent_used > 100U) {
            stats.percent_used = 100U;
        }
    }
    stats.valid = true;
    return stats;
}

static void format_memory_compact(size_t bytes, char *buffer, size_t buffer_size)
{
    const size_t mib = 1024U * 1024U;
    size_t whole = bytes / mib;
    size_t tenth = ((bytes % mib) * 10U + (mib / 2U)) / mib;

    if (tenth >= 10U) {
        whole++;
        tenth = 0;
    }
    snprintf(buffer, buffer_size, "%lu.%luM", (unsigned long) whole, (unsigned long) tenth);
}

static uint64_t movie_frame_interval_ticks(const Movie *movie)
{
    if (!movie->header.fps_num) {
        return 0;
    }
    return (((uint64_t) monotonic_clock_ticks_per_second()) * movie->header.fps_den) / movie->header.fps_num;
}

static const PlaybackRate *playback_rate_for_index(size_t rate_index)
{
    if (rate_index >= PLAYBACK_RATE_COUNT) {
        return &g_playback_rates[PLAYBACK_RATE_DEFAULT_INDEX];
    }
    return &g_playback_rates[rate_index];
}

static uint32_t movie_frame_time_ms(const Movie *movie, uint32_t frame_index)
{
    if (!movie->header.fps_num) {
        return 0;
    }
    return (uint32_t) (((uint64_t) frame_index * 1000ULL * movie->header.fps_den) / movie->header.fps_num);
}

static uint64_t movie_frame_time_scaled_ticks(const Movie *movie, uint32_t frame_index, const PlaybackRate *rate)
{
    if (!movie->header.fps_num || !rate || !rate->numerator) {
        return 0;
    }
    return (((uint64_t) frame_index) * monotonic_clock_ticks_per_second() * movie->header.fps_den * rate->denominator)
        / (((uint64_t) movie->header.fps_num) * rate->numerator);
}

static uint32_t movie_frames_from_ms(const Movie *movie, uint32_t total_ms)
{
    if (!movie->header.fps_num || !movie->header.fps_den) {
        return 0;
    }
    return (uint32_t) (((uint64_t) total_ms * movie->header.fps_num) / (1000ULL * movie->header.fps_den));
}

static uint32_t movie_frames_from_scaled_ticks(const Movie *movie, uint64_t total_ticks, const PlaybackRate *rate)
{
    if (!movie->header.fps_num || !movie->header.fps_den || !rate || !rate->denominator) {
        return 0;
    }
    return (uint32_t) ((total_ticks * movie->header.fps_num * rate->numerator)
        / (((uint64_t) monotonic_clock_ticks_per_second()) * movie->header.fps_den * rate->denominator));
}

static uint32_t movie_duration_ms(const Movie *movie)
{
    return movie_frame_time_ms(movie, movie->header.frame_count);
}

static void reset_playback_timeline(const Movie *movie, const PlaybackRate *playback_rate, uint64_t *anchor_ticks, uint32_t *anchor_frame, uint64_t *next_frame_due_ticks)
{
    uint64_t now_ticks = monotonic_clock_now_ticks();
    *anchor_ticks = now_ticks;
    *anchor_frame = movie->current_frame;
    *next_frame_due_ticks = now_ticks + movie_frame_time_scaled_ticks(movie, 1, playback_rate);
}

static uint16_t rolling_u16_average(uint16_t current, uint32_t sample_ms)
{
    if (sample_ms == 0U) {
        sample_ms = 1U;
    }
    if (current == 0U) {
        return (uint16_t) sample_ms;
    }
    return (uint16_t) (((current * 7U) + sample_ms + 4U) / 8U);
}

static void record_h264_prefetch_decode_time(Movie *movie, uint32_t elapsed_ms)
{
    uint32_t average_ms;
    uint32_t peak_ms;

    if (!movie) {
        return;
    }
    if (elapsed_ms == 0) {
        elapsed_ms = 1;
    }

    average_ms = rolling_u16_average(movie->h264_prefetch_decode_avg_ms, elapsed_ms);

    peak_ms = movie->h264_prefetch_decode_peak_ms;
    if (elapsed_ms >= peak_ms) {
        peak_ms = elapsed_ms;
    } else if (peak_ms > average_ms) {
        peak_ms--;
    } else {
        peak_ms = average_ms;
    }

    if (average_ms > H264_PREFETCH_DECODE_GUARD_MAX_MS) {
        average_ms = H264_PREFETCH_DECODE_GUARD_MAX_MS;
    }
    if (peak_ms > H264_PREFETCH_DECODE_GUARD_MAX_MS) {
        peak_ms = H264_PREFETCH_DECODE_GUARD_MAX_MS;
    }

    movie->h264_prefetch_decode_avg_ms = (uint16_t) average_ms;
    movie->h264_prefetch_decode_peak_ms = (uint16_t) peak_ms;
}

static void record_h264_background_decode_kind(Movie *movie, bool same_chunk, uint32_t elapsed_ms)
{
    if (!movie) {
        return;
    }

    if (same_chunk) {
        movie->diag_bg_same_chunk_count++;
        movie->diag_bg_same_chunk_avg_ms = rolling_u16_average(movie->diag_bg_same_chunk_avg_ms, elapsed_ms);
    } else {
        movie->diag_bg_next_chunk_count++;
        movie->diag_bg_next_chunk_avg_ms = rolling_u16_average(movie->diag_bg_next_chunk_avg_ms, elapsed_ms);
    }
}

static void record_h264_foreground_decode_time(Movie *movie, uint32_t elapsed_ms)
{
    uint32_t average_ms;
    uint32_t peak_ms;

    if (!movie) {
        return;
    }

    average_ms = rolling_u16_average(movie->h264_foreground_decode_avg_ms, elapsed_ms);
    if (average_ms > 1000U) {
        average_ms = 1000U;
    }

    peak_ms = movie->h264_foreground_decode_peak_ms;
    if (elapsed_ms > peak_ms) {
        peak_ms = elapsed_ms;
    } else if (peak_ms > elapsed_ms) {
        peak_ms -= (peak_ms - elapsed_ms + 3U) / 4U;
    }
    if (peak_ms > 1000U) {
        peak_ms = 1000U;
    }

    movie->h264_foreground_decode_avg_ms = (uint16_t) average_ms;
    movie->h264_foreground_decode_peak_ms = (uint16_t) peak_ms;
}

static uint32_t h264_prefetch_decode_guard_ms(const Movie *movie)
{
    uint32_t average_ms = movie && movie->h264_prefetch_decode_avg_ms
        ? movie->h264_prefetch_decode_avg_ms
        : H264_PREFETCH_DECODE_GUARD_DEFAULT_MS;
    uint32_t peak_ms = movie && movie->h264_prefetch_decode_peak_ms
        ? movie->h264_prefetch_decode_peak_ms
        : average_ms;
    uint32_t guard_ms = average_ms + 4U;

    if (peak_ms > guard_ms) {
        guard_ms = peak_ms;
    }
    if (guard_ms < H264_PREFETCH_DECODE_GUARD_MIN_MS) {
        guard_ms = H264_PREFETCH_DECODE_GUARD_MIN_MS;
    }
    if (guard_ms > H264_PREFETCH_DECODE_GUARD_MAX_MS) {
        guard_ms = H264_PREFETCH_DECODE_GUARD_MAX_MS;
    }
    return guard_ms;
}

static void wait_until_ticks_precise(uint64_t target_ticks)
{
    while ((int64_t) (target_ticks - monotonic_clock_now_ticks()) > 0) {
    }
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

static void free_history_store(HistoryStore *history)
{
    size_t index;
    if (!history) {
        return;
    }
    for (index = 0; index < history->count && index < HISTORY_MAX_ENTRIES; ++index) {
        free(history->entries[index].path);
        history->entries[index].path = NULL;
    }
    history->count = 0;
}

static void destroy_movie(Movie *movie)
{
    uint32_t index;
    int prefetch_index;
    int h264_ring_index;
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
    if (movie->subtitle_tracks) {
        for (index = 0; index < movie->subtitle_track_count; ++index) {
            free(movie->subtitle_tracks[index].name);
        }
    }
    free(movie->subtitles);
    free(movie->subtitle_tracks);
    free(movie->chunk_index);
    free(movie->framebuffer);
    free(movie->previous_framebuffer);
    free(movie->block_scratch);
    free(movie->chunk_storage);
    free(movie->frame_offsets);
    if (movie->h264_decoder) {
        if (movie->h264_decoder_initialized) {
            h264bsdShutdown(movie->h264_decoder);
        }
        h264bsdFree(movie->h264_decoder);
    }
    for (h264_ring_index = 0; h264_ring_index < H264_FRAME_RING_MAX_COUNT; ++h264_ring_index) {
        free(movie->h264_frame_ring[h264_ring_index].allocation);
    }
    for (prefetch_index = 0; prefetch_index < PREFETCH_CHUNK_COUNT; ++prefetch_index) {
        clear_prefetched_chunk(&movie->prefetched[prefetch_index]);
    }
    memset(movie, 0, sizeof(*movie));
    movie->loaded_chunk = -1;
    for (prefetch_index = 0; prefetch_index < PREFETCH_CHUNK_COUNT; ++prefetch_index) {
        movie->prefetched[prefetch_index].chunk_index = -1;
    }
    for (h264_ring_index = 0; h264_ring_index < H264_FRAME_RING_MAX_COUNT; ++h264_ring_index) {
        movie->h264_frame_ring[h264_ring_index].frame_index = UINT32_MAX;
    }
    movie->decoded_local_frame = -1;
}

static bool init_fonts(Fonts *fonts)
{
    size_t index;

    memset(fonts, 0, sizeof(*fonts));
    fonts->white = nSDL_LoadFont(NSDL_FONT_TINYTYPE, 255, 255, 255);
    fonts->outline = nSDL_LoadFont(NSDL_FONT_TINYTYPE, 0, 0, 0);
    for (index = 0; index < SUBTITLE_FONT_CHOICE_COUNT; ++index) {
        int font_id = g_subtitle_font_choices[index];
        fonts->subtitle_white[font_id] = nSDL_LoadFont(font_id, 255, 255, 255);
        fonts->subtitle_outline[font_id] = nSDL_LoadFont(font_id, 0, 0, 0);
        if (fonts->subtitle_white[font_id]) {
            nSDL_SetFontSpacing(fonts->subtitle_white[font_id], 0, 0);
        }
        if (fonts->subtitle_outline[font_id]) {
            nSDL_SetFontSpacing(fonts->subtitle_outline[font_id], 0, 0);
        }
    }
    if (!fonts->white || !fonts->outline) {
        free_fonts(fonts);
        return false;
    }
    for (index = 0; index < SUBTITLE_FONT_CHOICE_COUNT; ++index) {
        int font_id = g_subtitle_font_choices[index];
        if (!fonts->subtitle_white[font_id] || !fonts->subtitle_outline[font_id]) {
            free_fonts(fonts);
            return false;
        }
    }
    if (!fonts->subtitle_white[NSDL_FONT_TINYTYPE] || !fonts->subtitle_outline[NSDL_FONT_TINYTYPE]) {
        free_fonts(fonts);
        return false;
    }
    return true;
}

static void free_fonts(Fonts *fonts)
{
    int font_id;

    if (fonts->white) {
        nSDL_FreeFont(fonts->white);
    }
    if (fonts->outline) {
        nSDL_FreeFont(fonts->outline);
    }
    for (font_id = 0; font_id < NSP_NUMFONTS; ++font_id) {
        if (fonts->subtitle_white[font_id]) {
            nSDL_FreeFont(fonts->subtitle_white[font_id]);
        }
        if (fonts->subtitle_outline[font_id]) {
            nSDL_FreeFont(fonts->subtitle_outline[font_id]);
        }
    }
    memset(fonts, 0, sizeof(*fonts));
}

static bool movie_uses_h264(const Movie *movie)
{
    return movie && movie->header.version >= MOVIE_VERSION_H264;
}

static void init_h264_color_tables(void)
{
    int index;
    if (g_h264_color_tables.initialized) {
        return;
    }

    for (index = 0; index < 256; ++index) {
        int y = index - 16;
        int chroma = index - 128;
        if (y < 0) {
            y = 0;
        }
        g_h264_color_tables.y_base[index] = (298 * y) + 128;
        g_h264_color_tables.u_to_blue[index] = 516 * chroma;
        g_h264_color_tables.u_to_green[index] = -100 * chroma;
        g_h264_color_tables.v_to_red[index] = 409 * chroma;
        g_h264_color_tables.v_to_green[index] = -208 * chroma;
        g_h264_color_tables.red565[index] = (uint16_t) ((index & 0xF8) << 8);
        g_h264_color_tables.green565[index] = (uint16_t) ((index & 0xFC) << 3);
        g_h264_color_tables.blue565[index] = (uint16_t) (index >> 3);
    }

    for (index = 0; index < H264_CLIP_TABLE_SIZE; ++index) {
        int value = index - H264_CLIP_OFFSET;
        if (value < 0) {
            value = 0;
        } else if (value > 255) {
            value = 255;
        }
        g_h264_color_tables.clip[index] = (uint8_t) value;
    }

    g_h264_color_tables.initialized = true;
}

static inline uint8_t h264_clip_byte(int32_t value)
{
    /* The YUV->RGB fixed-point path keeps this in [-258, 534]. */
    return g_h264_color_tables.clip[value + H264_CLIP_OFFSET];
}

static uint8_t *alloc_aligned_bytes(size_t size, size_t alignment, uint8_t **allocation)
{
    uintptr_t raw_address;
    uintptr_t aligned_address;
    uint8_t *raw;

    if (allocation) {
        *allocation = NULL;
    }
    if (size == 0 || alignment < sizeof(void *) || (alignment & (alignment - 1U)) != 0U) {
        return NULL;
    }

    raw = (uint8_t *) malloc(size + alignment - 1U);
    if (!raw) {
        return NULL;
    }

    raw_address = (uintptr_t) raw;
    aligned_address = (raw_address + alignment - 1U) & ~((uintptr_t) alignment - 1U);
    if (allocation) {
        *allocation = raw;
    }
    return (uint8_t *) aligned_address;
}

#if defined(__arm__) && !defined(__thumb__)
static inline int32_t armv5te_smulbb(int32_t lhs, int32_t rhs)
{
    int32_t result;
    __asm__ volatile ("smulbb %0, %1, %2" : "=r" (result) : "r" (lhs), "r" (rhs));
    return result;
}

static inline int32_t armv5te_smlabb(int32_t acc, int32_t lhs, int32_t rhs)
{
    int32_t result;
    __asm__ volatile ("smlabb %0, %1, %2, %3" : "=r" (result) : "r" (lhs), "r" (rhs), "r" (acc));
    return result;
}

static inline void h264_compute_chroma_terms(uint8_t u_sample, uint8_t v_sample, int32_t *red, int32_t *green, int32_t *blue)
{
    const int32_t u = (int32_t) u_sample - 128;
    const int32_t v = (int32_t) v_sample - 128;

    *red = armv5te_smulbb(v, 409);
    *green = armv5te_smlabb(armv5te_smulbb(u, -100), v, -208);
    *blue = armv5te_smulbb(u, 516);
}
#else
static inline void h264_compute_chroma_terms(uint8_t u_sample, uint8_t v_sample, int32_t *red, int32_t *green, int32_t *blue)
{
    *red = g_h264_color_tables.v_to_red[v_sample];
    *green = g_h264_color_tables.u_to_green[u_sample] + g_h264_color_tables.v_to_green[v_sample];
    *blue = g_h264_color_tables.u_to_blue[u_sample];
}
#endif

static size_t h264_cropped_frame_bytes(const Movie *movie)
{
    size_t luma_size;
    size_t chroma_size;

    if (!movie) {
        return 0;
    }

    luma_size = (size_t) movie->header.video_width * movie->header.video_height;
    chroma_size = ((size_t) movie->header.video_width / 2U) * ((size_t) movie->header.video_height / 2U);
    return luma_size + (chroma_size * 2U);
}

static size_t active_h264_frame_ring_capacity(const Movie *movie)
{
    if (!movie || movie->h264_frame_ring_capacity > H264_FRAME_RING_MAX_COUNT) {
        return 0;
    }
    return movie->h264_frame_ring_capacity;
}

static size_t target_h264_frame_ring_capacity(const Movie *movie)
{
    size_t target;

    if (!movie || movie->h264_frame_bytes == 0) {
        return 0;
    }

    target = H264_FRAME_RING_BUDGET_BYTES / movie->h264_frame_bytes;
    if (target == 0) {
        target = 1;
    }
    if (target > H264_FRAME_RING_USEFUL_MAX) {
        target = H264_FRAME_RING_USEFUL_MAX;
    }
    if (target > H264_FRAME_RING_MAX_COUNT) {
        target = H264_FRAME_RING_MAX_COUNT;
    }
    return target;
}

static void release_h264_frame_ring_slot(Movie *movie, size_t slot_index)
{
    size_t last_index;
    H264FrameSlot temp;

    if (!movie || active_h264_frame_ring_capacity(movie) == 0 || slot_index >= active_h264_frame_ring_capacity(movie)) {
        return;
    }

    last_index = movie->h264_frame_ring_capacity - 1U;
    if (slot_index != last_index) {
        temp = movie->h264_frame_ring[slot_index];
        movie->h264_frame_ring[slot_index] = movie->h264_frame_ring[last_index];
        movie->h264_frame_ring[last_index] = temp;
    }

    free(movie->h264_frame_ring[last_index].allocation);
    memset(&movie->h264_frame_ring[last_index], 0, sizeof(H264FrameSlot));
    movie->h264_frame_ring[last_index].frame_index = UINT32_MAX;
    movie->h264_frame_ring[last_index].valid = false;
    movie->h264_frame_ring_capacity--;
}

static int find_h264_frame_ring_victim(const Movie *movie, size_t min_ready_to_keep)
{
    size_t ready_count = 0;
    size_t index;
    int invalid_index = -1;
    int farthest_index = -1;
    uint32_t farthest_frame = 0;

    if (!movie) {
        return -1;
    }

    for (index = 0; index < active_h264_frame_ring_capacity(movie); ++index) {
        const H264FrameSlot *slot = &movie->h264_frame_ring[index];
        if (!slot->data || !slot->valid || slot->frame_index <= movie->current_frame) {
            invalid_index = (int) index;
            continue;
        }
        ready_count++;
        if (slot->frame_index >= farthest_frame) {
            farthest_frame = slot->frame_index;
            farthest_index = (int) index;
        }
    }

    if (invalid_index >= 0) {
        return invalid_index;
    }
    if (ready_count > min_ready_to_keep) {
        return farthest_index;
    }
    return -1;
}

static bool ensure_app_memory_headroom(Movie *movie, size_t bytes_needed, size_t min_ready_to_keep)
{
    if (!movie) {
        return false;
    }

    discard_h264_frame_ring_before(movie, movie->current_frame + 1U);
    while (1) {
        MemoryStats stats = query_memory_stats(movie);
        size_t required_bytes = stats.used_bytes + bytes_needed + APP_RAM_HEADROOM_BYTES;
        int victim_index;

        if (required_bytes <= APP_RAM_TARGET_BYTES) {
            return true;
        }
        victim_index = find_h264_frame_ring_victim(movie, min_ready_to_keep);
        if (victim_index < 0) {
            break;
        }
        release_h264_frame_ring_slot(movie, (size_t) victim_index);
    }

    return query_memory_stats(movie).used_bytes + bytes_needed + APP_RAM_HEADROOM_BYTES <= APP_RAM_TARGET_BYTES;
}

static size_t trim_h264_frame_ring(Movie *movie, size_t min_ready_to_keep, size_t target_capacity)
{
    size_t start_capacity;

    if (!movie) {
        return 0;
    }

    discard_h264_frame_ring_before(movie, movie->current_frame + 1U);
    start_capacity = active_h264_frame_ring_capacity(movie);
    while (active_h264_frame_ring_capacity(movie) > target_capacity) {
        int victim_index = find_h264_frame_ring_victim(movie, min_ready_to_keep);
        if (victim_index < 0) {
            break;
        }
        release_h264_frame_ring_slot(movie, (size_t) victim_index);
    }
    return start_capacity - active_h264_frame_ring_capacity(movie);
}

static void try_grow_h264_frame_ring(Movie *movie, size_t max_new_slots)
{
    size_t target;
    size_t start_capacity;

    if (!movie || max_new_slots == 0) {
        return;
    }

    target = target_h264_frame_ring_capacity(movie);
    start_capacity = active_h264_frame_ring_capacity(movie);
    while (movie->h264_frame_ring_capacity < target && max_new_slots > 0) {
        size_t slot_index = movie->h264_frame_ring_capacity;
        H264FrameSlot *slot = &movie->h264_frame_ring[slot_index];
        if (!ensure_app_memory_headroom(movie, movie->h264_frame_bytes, H264_PREFETCH_MIN_READY_FRAMES)) {
            break;
        }

        slot->data = alloc_aligned_bytes(
            movie->h264_frame_bytes,
            H264_FRAME_RING_ALIGNMENT,
            &slot->allocation
        );
        if (!slot->data) {
            break;
        }
        slot->frame_index = UINT32_MAX;
        slot->valid = false;
        movie->h264_frame_ring_capacity++;
        max_new_slots--;
    }

    if (movie->h264_frame_ring_capacity != start_capacity) {
        debug_tracef(
            "h264 ring grow slots=%lu/%lu frame_bytes=%lu budget=%lu",
            (unsigned long) movie->h264_frame_ring_capacity,
            (unsigned long) target,
            (unsigned long) movie->h264_frame_bytes,
            (unsigned long) H264_FRAME_RING_BUDGET_BYTES
        );
    } else if (start_capacity == 0 && target > 0) {
        debug_tracef(
            "h264 ring unavailable frame_bytes=%lu budget=%lu",
            (unsigned long) movie->h264_frame_bytes,
            (unsigned long) H264_FRAME_RING_BUDGET_BYTES
        );
    }
}

static void clear_h264_frame_ring(Movie *movie)
{
    size_t index;

    if (!movie) {
        return;
    }

    for (index = 0; index < active_h264_frame_ring_capacity(movie); ++index) {
        movie->h264_frame_ring[index].frame_index = UINT32_MAX;
        movie->h264_frame_ring[index].valid = false;
    }
}

static void discard_h264_frame_ring_before(Movie *movie, uint32_t first_frame_to_keep)
{
    size_t index;

    if (!movie) {
        return;
    }

    for (index = 0; index < active_h264_frame_ring_capacity(movie); ++index) {
        if (movie->h264_frame_ring[index].valid &&
            movie->h264_frame_ring[index].frame_index < first_frame_to_keep) {
            movie->h264_frame_ring[index].frame_index = UINT32_MAX;
            movie->h264_frame_ring[index].valid = false;
        }
    }
}

static size_t h264_frame_ring_contiguous_ready_count(const Movie *movie)
{
    size_t count = 0;
    uint32_t frame_index;

    if (!movie) {
        return 0;
    }

    for (frame_index = movie->current_frame + 1U; frame_index < movie->header.frame_count; ++frame_index) {
        size_t index;
        bool found = false;
        for (index = 0; index < active_h264_frame_ring_capacity(movie); ++index) {
            if (movie->h264_frame_ring[index].valid &&
                movie->h264_frame_ring[index].frame_index == frame_index) {
                found = true;
                break;
            }
        }
        if (!found) {
            break;
        }
        count++;
    }

    return count;
}

static size_t h264_frame_ring_valid_count(const Movie *movie)
{
    size_t count = 0;
    size_t index;

    if (!movie) {
        return 0;
    }

    for (index = 0; index < active_h264_frame_ring_capacity(movie); ++index) {
        if (movie->h264_frame_ring[index].valid) {
            count++;
        }
    }

    return count;
}

static void debug_trace_runtime_snapshot(
    Movie *movie,
    bool paused,
    uint32_t spare_ms,
    const PlaybackRate *playback_rate,
    const char *tag
)
{
    MemoryStats stats;
    size_t ring_valid;
    size_t ring_contig;
    size_t ready_target;

    if (!movie) {
        return;
    }

    stats = query_memory_stats(movie);
    ring_valid = h264_frame_ring_valid_count(movie);
    ring_contig = h264_frame_ring_contiguous_ready_count(movie);
    ready_target = movie_uses_h264(movie)
        ? h264_prefetch_target_ready_count(movie, spare_ms)
        : 0U;
    debug_tracef(
        "snap %s pause=%u rate=%s frame=%lu chunk=%d contig=%lu/%lu ring=%lu/%lu backoff=%u spare=%lu mem=%u fg=%u/%u bg=%u/%u replay=%lu miss=%lu/%lu/%lu/%lu chunkpref=%lu",
        tag ? tag : "-",
        paused ? 1U : 0U,
        playback_rate ? playback_rate->label : "-",
        (unsigned long) movie->current_frame,
        movie->loaded_chunk,
        (unsigned long) ring_contig,
        (unsigned long) ready_target,
        (unsigned long) ring_valid,
        (unsigned long) active_h264_frame_ring_capacity(movie),
        (unsigned) movie->h264_active_prefetch_backoff,
        (unsigned long) spare_ms,
        stats.percent_used,
        (unsigned) movie->h264_foreground_decode_avg_ms,
        (unsigned) movie->h264_foreground_decode_peak_ms,
        (unsigned) movie->h264_prefetch_decode_avg_ms,
        (unsigned) movie->h264_prefetch_decode_peak_ms,
        (unsigned long) movie->diag_h264_replay_count,
        (unsigned long) movie->diag_ring_miss_not_cached_count,
        (unsigned long) movie->diag_ring_miss_not_contiguous_count,
        (unsigned long) movie->diag_ring_miss_chunk_not_ready_count,
        (unsigned long) movie->diag_ring_miss_slot_unavailable_count,
        (unsigned long) total_prefetched_chunk_bytes(movie)
    );
}

static void debug_dump_session(const char *path, const Movie *movie, const char *reason)
{
    FILE *log_file;
    size_t index;

    if (!path) {
        return;
    }

    log_file = fopen(path, "wb");
    if (!log_file) {
        return;
    }

    fputs("ND Video Player diagnostic log\n", log_file);
    fprintf(log_file, "reason=%s\n", reason ? reason : "unknown");
    fprintf(log_file, "last_error=%s\n", debug_last_error());

    if (movie) {
        MemoryStats stats = query_memory_stats(movie);
        fprintf(
            log_file,
            "frame=%lu/%lu loaded_chunk=%d decoded_local=%d mem_used=%lu mem_prefetched=%lu mem_free=%lu mem_pct=%u\n",
            (unsigned long) movie->current_frame,
            (unsigned long) movie->header.frame_count,
            movie->loaded_chunk,
            movie->decoded_local_frame,
            (unsigned long) stats.used_bytes,
            (unsigned long) stats.prefetched_bytes,
            (unsigned long) stats.free_bytes,
            stats.percent_used
        );
        fprintf(
            log_file,
            "ring_valid=%lu ring_contig=%lu ring_cap=%lu ready_target=%lu chunk_prefetched=%lu active_backoff=%u\n",
            (unsigned long) h264_frame_ring_valid_count(movie),
            (unsigned long) h264_frame_ring_contiguous_ready_count(movie),
            (unsigned long) active_h264_frame_ring_capacity(movie),
            (unsigned long) h264_prefetch_target_ready_count(movie, 0U),
            (unsigned long) total_prefetched_chunk_bytes(movie),
            (unsigned) movie->h264_active_prefetch_backoff
        );
        fprintf(
            log_file,
            "fg_decode count=%lu ring_hits=%lu direct=%lu avg_ms=%u peak_ms=%u lag_events=%lu lag_frames_total=%lu max_lag_frames=%lu max_late_ms=%lu\n",
            (unsigned long) movie->diag_foreground_decode_count,
            (unsigned long) movie->diag_foreground_ring_hit_count,
            (unsigned long) movie->diag_foreground_direct_decode_count,
            (unsigned) movie->h264_foreground_decode_avg_ms,
            (unsigned) movie->h264_foreground_decode_peak_ms,
            (unsigned long) movie->diag_lag_event_count,
            (unsigned long) movie->diag_lag_frame_total,
            (unsigned long) movie->diag_max_lag_frames,
            (unsigned long) movie->diag_max_late_ms
        );
        fprintf(
            log_file,
            "prefetch ticks=%lu active_ticks=%lu suppressed=%lu backoff_skips=%lu io_priority=%lu frame_decodes=%lu frame_decode_fail=%lu avg_ms=%u peak_ms=%u bg_same=%lu/%u bg_next=%lu/%u bg_cross_blocked=%lu\n",
            (unsigned long) movie->diag_prefetch_tick_count,
            (unsigned long) movie->diag_active_prefetch_tick_count,
            (unsigned long) movie->diag_prefetch_suppressed_count,
            (unsigned long) movie->diag_prefetch_backoff_skip_count,
            (unsigned long) movie->diag_io_priority_count,
            (unsigned long) movie->diag_prefetch_frame_decode_count,
            (unsigned long) movie->diag_prefetch_frame_decode_fail_count,
            (unsigned) movie->h264_prefetch_decode_avg_ms,
            (unsigned) movie->h264_prefetch_decode_peak_ms,
            (unsigned long) movie->diag_bg_same_chunk_count,
            (unsigned) movie->diag_bg_same_chunk_avg_ms,
            (unsigned long) movie->diag_bg_next_chunk_count,
            (unsigned) movie->diag_bg_next_chunk_avg_ms,
            (unsigned long) movie->diag_bg_chunk_cross_blocked_count
        );
        fprintf(
            log_file,
            "chunk loads_sync=%lu loads_prefetched=%lu read_ops=%lu read_bytes=%lu inflate_ops=%lu inflate_bytes=%lu max_spare_ms=%lu\n",
            (unsigned long) movie->diag_chunk_load_sync_count,
            (unsigned long) movie->diag_chunk_load_prefetched_count,
            (unsigned long) movie->diag_prefetch_read_ops,
            (unsigned long) movie->diag_prefetch_read_bytes,
            (unsigned long) movie->diag_prefetch_inflate_ops,
            (unsigned long) movie->diag_prefetch_inflate_bytes,
            (unsigned long) movie->diag_max_spare_ms
        );
        fprintf(
            log_file,
            "replay count=%lu frames=%lu max_distance=%lu miss_not_cached=%lu miss_not_contig=%lu miss_chunk_not_ready=%lu miss_slot_unavailable=%lu boundary_miss=%lu\n",
            (unsigned long) movie->diag_h264_replay_count,
            (unsigned long) movie->diag_h264_replay_frames_total,
            (unsigned long) movie->diag_h264_replay_max_distance,
            (unsigned long) movie->diag_ring_miss_not_cached_count,
            (unsigned long) movie->diag_ring_miss_not_contiguous_count,
            (unsigned long) movie->diag_ring_miss_chunk_not_ready_count,
            (unsigned long) movie->diag_ring_miss_slot_unavailable_count,
            (unsigned long) movie->diag_chunk_boundary_miss_count
        );
    }

    fputs("recent_events:\n", log_file);
    for (index = 0; index < g_debug_ring_count; ++index) {
        size_t ring_index = (g_debug_ring_next + DEBUG_RING_SIZE - g_debug_ring_count + index) % DEBUG_RING_SIZE;
        fputs(g_debug_ring[ring_index], log_file);
        fputc('\n', log_file);
    }

    fclose(log_file);
}

static H264FrameSlot *find_h264_frame_slot(Movie *movie, uint32_t frame_index)
{
    size_t index;

    if (!movie) {
        return NULL;
    }

    for (index = 0; index < active_h264_frame_ring_capacity(movie); ++index) {
        if (movie->h264_frame_ring[index].valid &&
            movie->h264_frame_ring[index].frame_index == frame_index) {
            return &movie->h264_frame_ring[index];
        }
    }
    return NULL;
}

static H264FrameSlot *reserve_h264_frame_slot(Movie *movie, uint32_t frame_index)
{
    size_t index;

    if (!movie) {
        return NULL;
    }

    for (index = 0; index < active_h264_frame_ring_capacity(movie); ++index) {
        if (movie->h264_frame_ring[index].valid &&
            movie->h264_frame_ring[index].frame_index == frame_index) {
            return &movie->h264_frame_ring[index];
        }
    }

    for (index = 0; index < active_h264_frame_ring_capacity(movie); ++index) {
        if (movie->h264_frame_ring[index].data && !movie->h264_frame_ring[index].valid) {
            movie->h264_frame_ring[index].frame_index = frame_index;
            return &movie->h264_frame_ring[index];
        }
    }

    return NULL;
}

static const PrefetchedChunk *find_prefetched_chunk_const(const Movie *movie, int chunk_index)
{
    int index;

    if (!movie) {
        return NULL;
    }

    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        if (movie->prefetched[index].chunk_index == chunk_index) {
            return &movie->prefetched[index];
        }
    }
    return NULL;
}

static uint32_t h264_frame_size_from_offsets(const uint32_t *frame_offsets, uint32_t frame_count, size_t chunk_size, uint32_t local_index)
{
    size_t start;
    size_t end;

    if (!frame_offsets || local_index >= frame_count) {
        return 0;
    }

    start = frame_offsets[local_index];
    end = (local_index + 1U < frame_count) ? frame_offsets[local_index + 1U] : chunk_size;
    if (end < start || end > chunk_size) {
        return 0;
    }
    return (uint32_t) (end - start);
}

static uint32_t h264_frame_size_from_chunk_storage(const Movie *movie, const ChunkIndexEntry *entry, const uint8_t *chunk_storage, size_t chunk_storage_size, uint32_t local_index)
{
    size_t local_offset;
    size_t table_size;
    size_t header_size;
    uint32_t payload_size;
    uint32_t start;
    uint32_t end;

    if (!movie || !entry || !chunk_storage || local_index >= entry->frame_count) {
        return 0;
    }

    if (movie->header.version < 4) {
        return 0;
    }

    local_offset = (size_t) entry->frame_table_offset;
    table_size = (size_t) entry->frame_count * sizeof(uint32_t);
    header_size = 4U + table_size;
    if (local_offset + header_size > chunk_storage_size) {
        return 0;
    }

    payload_size = read_le32(chunk_storage + local_offset);
    if (local_offset + header_size + payload_size > chunk_storage_size) {
        return 0;
    }

    start = read_le32(chunk_storage + local_offset + 4U + ((size_t) local_index * sizeof(uint32_t)));
    end = (local_index + 1U < entry->frame_count)
        ? read_le32(chunk_storage + local_offset + 4U + ((size_t) (local_index + 1U) * sizeof(uint32_t)))
        : payload_size;
    if (end < start || end > payload_size) {
        return 0;
    }
    return end - start;
}

static uint32_t estimate_h264_chunk_average_frame_bytes(const Movie *movie, const ChunkIndexEntry *entry)
{
    size_t payload_bytes;

    if (!movie || !entry || entry->frame_count == 0) {
        return 0;
    }

    payload_bytes = entry->unpacked_size;
    if (movie->header.version >= 4) {
        size_t header_bytes = 4U + ((size_t) entry->frame_count * sizeof(uint32_t));
        if (payload_bytes > header_bytes) {
            payload_bytes -= header_bytes;
        } else {
            payload_bytes = 0;
        }
    }

    if (payload_bytes == 0) {
        return 0;
    }

    return (uint32_t) ((payload_bytes + entry->frame_count - 1U) / entry->frame_count);
}

static uint32_t estimate_h264_frame_bytes(const Movie *movie, uint32_t frame_index)
{
    int chunk_index;
    const ChunkIndexEntry *entry;
    uint32_t local_index;
    const PrefetchedChunk *prefetched;

    if (!movie || !movie_uses_h264(movie) || frame_index >= movie->header.frame_count) {
        return 0;
    }

    chunk_index = movie_chunk_for_frame(movie, frame_index);
    if (chunk_index < 0) {
        return 0;
    }

    entry = movie->chunk_index + chunk_index;
    local_index = frame_index - entry->first_frame;

    if (movie->loaded_chunk == chunk_index && movie->frame_offsets && movie->chunk_bytes) {
        return h264_frame_size_from_offsets(movie->frame_offsets, entry->frame_count, movie->chunk_size, local_index);
    }

    prefetched = find_prefetched_chunk_const(movie, chunk_index);
    if (prefetched && prefetched->state == PREFETCH_READY && prefetched->chunk_storage) {
        return h264_frame_size_from_chunk_storage(movie, entry, prefetched->chunk_storage, prefetched->chunk_storage_size, local_index);
    }

    return estimate_h264_chunk_average_frame_bytes(movie, entry);
}

static uint32_t h264_prefetch_active_scan_frames(uint32_t spare_ms)
{
    if (spare_ms >= 28U) {
        return H264_PREFETCH_ACTIVE_SCAN_MAX_FRAMES;
    }
    if (spare_ms >= 20U) {
        return H264_PREFETCH_ACTIVE_SCAN_MID_FRAMES;
    }
    return H264_PREFETCH_ACTIVE_SCAN_MIN_FRAMES;
}

static size_t h264_prefetch_active_ready_cap(const Movie *movie, uint32_t spare_ms)
{
    size_t cap = H264_PREFETCH_ACTIVE_READY_BASE;

    if (!movie) {
        return 0;
    }

    if (spare_ms >= 18U) {
        cap = H264_PREFETCH_ACTIVE_READY_MID;
    }
    if (spare_ms >= 24U) {
        cap = H264_PREFETCH_ACTIVE_READY_HIGH;
    }
    if (spare_ms >= 32U) {
        cap = H264_PREFETCH_ACTIVE_READY_MAX;
    }
    if (cap > active_h264_frame_ring_capacity(movie)) {
        cap = active_h264_frame_ring_capacity(movie);
    }
    return cap;
}

static uint32_t h264_prefetch_active_decode_limit(size_t contiguous_ready, size_t target_ready_count, uint32_t spare_ms)
{
    if (contiguous_ready >= target_ready_count) {
        return 0U;
    }
    if (contiguous_ready < H264_PREFETCH_ACTIVE_LOW_WATERMARK && spare_ms >= 20U) {
        return H264_PREFETCH_ACTIVE_MAX_DECODES_RECOVERY;
    }
    return H264_PREFETCH_ACTIVE_MAX_DECODES_PER_TICK;
}

static bool h264_should_allow_active_prefetch(const Movie *movie, uint32_t spare_ms) __attribute__((unused));

static bool h264_should_allow_active_prefetch(const Movie *movie, uint32_t spare_ms)
{
    if (!movie) {
        return false;
    }

    if (movie->h264_active_prefetch_backoff > 0) {
        return false;
    }
    if (movie->h264_foreground_decode_peak_ms >= H264_FOREGROUND_DECODE_HARD_MS && spare_ms < 28U) {
        return false;
    }
    if (movie->h264_foreground_decode_avg_ms >= H264_FOREGROUND_DECODE_SOFT_MS && spare_ms < 22U) {
        return false;
    }
    return true;
}

static size_t h264_prefetch_target_ready_count(const Movie *movie, uint32_t spare_ms)
{
    size_t capacity;
    size_t target;
    size_t heavy_target = 0;
    size_t proximity_bonus = 0;
    size_t contiguous_ready;
    size_t active_cap;
    uint64_t total_frame_bytes = 0;
    uint32_t sampled_frames = 0;
    uint32_t average_frame_bytes;
    uint32_t above_average_threshold;
    uint32_t heavy_hits = 0;
    uint32_t frame_index;
    uint32_t limit;
    uint32_t scan_frames;

    if (!movie || movie->current_frame + 1U >= movie->header.frame_count) {
        return 0;
    }

    capacity = active_h264_frame_ring_capacity(movie);
    if (capacity == 0) {
        return 0;
    }

    target = H264_PREFETCH_MIN_READY_FRAMES;
    if (target > capacity) {
        target = capacity;
    }

    contiguous_ready = h264_frame_ring_contiguous_ready_count(movie);
    active_cap = h264_prefetch_active_ready_cap(movie, spare_ms);
    if (active_cap < target) {
        active_cap = target;
    }

    scan_frames = h264_prefetch_active_scan_frames(spare_ms);
    if (contiguous_ready + 8U < target && scan_frames < H264_PREFETCH_ACTIVE_SCAN_MID_FRAMES) {
        scan_frames = H264_PREFETCH_ACTIVE_SCAN_MID_FRAMES;
    }
    if (scan_frames > H264_PREFETCH_LOOKAHEAD_FRAMES) {
        scan_frames = H264_PREFETCH_LOOKAHEAD_FRAMES;
    }

    limit = movie->current_frame + scan_frames + 1U;
    if (limit > movie->header.frame_count) {
        limit = movie->header.frame_count;
    }

    for (frame_index = movie->current_frame + 1U; frame_index < limit; ++frame_index) {
        uint32_t frame_bytes = estimate_h264_frame_bytes(movie, frame_index);
        if (frame_bytes > 0) {
            total_frame_bytes += frame_bytes;
            sampled_frames++;
        }
    }

    if (sampled_frames > 0) {
        average_frame_bytes = (uint32_t) ((total_frame_bytes + (sampled_frames / 2U)) / sampled_frames);
    } else {
        average_frame_bytes = H264_PREFETCH_HEAVY_FRAME_BYTES;
    }
    if (average_frame_bytes < H264_PREFETCH_AVG_MIN_FRAME_BYTES) {
        average_frame_bytes = H264_PREFETCH_AVG_MIN_FRAME_BYTES;
    }

    above_average_threshold =
        (average_frame_bytes * H264_PREFETCH_ABOVE_AVG_NUMERATOR) / H264_PREFETCH_ABOVE_AVG_DENOMINATOR;
    if (above_average_threshold < average_frame_bytes + 256U) {
        above_average_threshold = average_frame_bytes + 256U;
    }

    for (frame_index = movie->current_frame + 1U; frame_index < limit; ++frame_index) {
        size_t distance = (size_t) (frame_index - movie->current_frame);
        uint32_t frame_bytes = estimate_h264_frame_bytes(movie, frame_index);
        if (frame_bytes >= above_average_threshold) {
            heavy_target = distance;
            heavy_hits++;
            if (distance <= H264_PREFETCH_NEAR_WINDOW_FRAMES) {
                proximity_bonus++;
            }
            if (frame_bytes >= H264_PREFETCH_VERY_HEAVY_FRAME_BYTES || heavy_hits >= 3U) {
                break;
            }
        }
    }

    /* Next-chunk boundary runway is intentionally ignored here because the
     * background decoder no longer crosses chunk boundaries. */

    if (spare_ms >= 18U) {
        target += 2U;
    }
    if (spare_ms >= 24U) {
        target += 4U;
    }
    if (spare_ms >= 32U) {
        target += 8U;
    }
    if (proximity_bonus > H264_PREFETCH_MAX_PROXIMITY_BONUS) {
        proximity_bonus = H264_PREFETCH_MAX_PROXIMITY_BONUS;
    }
    target += proximity_bonus;
    if (heavy_target > target) {
        target = heavy_target;
    }
    if (target > active_cap) {
        target = active_cap;
    }
    if (target > capacity) {
        target = capacity;
    }
    return target;
}

static bool prefetch_one_h264_same_chunk_frame(Movie *movie, uint32_t spare_ms, uint32_t deadline_ms)
{
    int64_t decoded_frame;
    uint32_t next_frame;
    int current_chunk;
    int next_chunk;
    uint32_t decode_guard_ms;
    uint32_t decode_start_ms;
    uint32_t decode_elapsed_ms;

    if (!movie || !movie_uses_h264(movie) || spare_ms < H264_SAME_CHUNK_RESCUE_MIN_SPARE_MS) {
        return false;
    }

    current_chunk = movie_chunk_for_frame(movie, movie->current_frame);
    if (current_chunk < 0) {
        return false;
    }
    if (h264_frame_ring_contiguous_ready_count(movie) >= 2U) {
        return false;
    }

    decoded_frame = h264_decoded_global_frame(movie);
    next_frame = (decoded_frame >= (int64_t) movie->current_frame)
        ? ((uint32_t) decoded_frame + 1U)
        : (movie->current_frame + 1U);
    if (next_frame >= movie->header.frame_count) {
        return false;
    }

    next_chunk = movie_chunk_for_frame(movie, next_frame);
    if (next_chunk != current_chunk || movie->loaded_chunk != current_chunk) {
        return false;
    }
    if (find_h264_frame_slot(movie, next_frame)) {
        return false;
    }

    decode_guard_ms = h264_prefetch_decode_guard_ms(movie);
    if (deadline_ms != 0) {
        uint32_t now_ms = monotonic_clock_now_ms();
        if ((int32_t) ((now_ms + decode_guard_ms) - deadline_ms) >= 0) {
            return false;
        }
    }

    decode_start_ms = monotonic_clock_now_ms();
    if (!decode_h264_frame(movie, next_frame, false, true)) {
        movie->diag_prefetch_frame_decode_fail_count++;
        debug_tracef("prefetch rescue fail frame=%lu chunk=%d", (unsigned long) next_frame, current_chunk);
        return false;
    }
    decode_elapsed_ms = monotonic_clock_now_ms() - decode_start_ms;
    movie->diag_prefetch_frame_decode_count++;
    record_h264_prefetch_decode_time(movie, decode_elapsed_ms);
    record_h264_background_decode_kind(movie, true, decode_elapsed_ms);
    if (decode_elapsed_ms >= DEBUG_TRACE_PREFETCH_MS) {
        debug_tracef(
            "prefetch rescue frame=%lu ms=%lu contig=%lu chunk=%d",
            (unsigned long) next_frame,
            (unsigned long) decode_elapsed_ms,
            (unsigned long) h264_frame_ring_contiguous_ready_count(movie),
            current_chunk
        );
    }
    return true;
}

static bool h264_prefetch_deadline_reached(uint32_t deadline_ms)
{
    if (deadline_ms == 0) {
        return false;
    }
    return (int32_t) ((monotonic_clock_now_ms() + H264_PREFETCH_ACTIVE_GUARD_MS) - deadline_ms) >= 0;
}

static int64_t h264_decoded_global_frame(const Movie *movie)
{
    if (!movie || !movie->chunk_index || movie->loaded_chunk < 0 ||
        (uint32_t) movie->loaded_chunk >= movie->header.chunk_count ||
        movie->decoded_local_frame < 0) {
        return -1;
    }
    return (int64_t) movie->chunk_index[movie->loaded_chunk].first_frame + movie->decoded_local_frame;
}

static bool reset_h264_decoder(Movie *movie)
{
    if (!movie || !movie->h264_decoder) {
        debug_failf("h264 decoder reset failed: decoder missing");
        return false;
    }
    if (movie->h264_decoder_initialized) {
        h264bsdShutdown(movie->h264_decoder);
    }
    if (h264bsdInit(movie->h264_decoder, HANTRO_TRUE) != HANTRO_OK) {
        movie->h264_decoder_initialized = false;
        debug_failf("h264 decoder init failed");
        return false;
    }
    movie->h264_headers_ready = false;
    movie->h264_decoder_initialized = true;
    movie->h264_full_width = 0;
    movie->h264_full_height = 0;
    movie->h264_crop_left = 0;
    movie->h264_crop_top = 0;
    movie->h264_crop_width = 0;
    movie->h264_crop_height = 0;
    movie->h264_chunk_dirty = false;
    return true;
}

static bool refresh_h264_picture_params(Movie *movie)
{
    u32 cropping_flag = 0;
    u32 crop_left = 0;
    u32 crop_width = 0;
    u32 crop_top = 0;
    u32 crop_height = 0;

    if (!movie || !movie->h264_decoder) {
        debug_failf("h264 picture params failed: decoder missing");
        return false;
    }

    movie->h264_full_width = h264bsdPicWidth(movie->h264_decoder) * 16U;
    movie->h264_full_height = h264bsdPicHeight(movie->h264_decoder) * 16U;
    h264bsdCroppingParams(movie->h264_decoder, &cropping_flag, &crop_left, &crop_width, &crop_top, &crop_height);
    if (!cropping_flag) {
        crop_left = 0;
        crop_top = 0;
        crop_width = movie->h264_full_width;
        crop_height = movie->h264_full_height;
    }
    if (crop_width != movie->header.video_width || crop_height != movie->header.video_height) {
        debug_failf(
            "h264 size mismatch crop=%lux%lu header=%ux%u",
            (unsigned long) crop_width,
            (unsigned long) crop_height,
            (unsigned) movie->header.video_width,
            (unsigned) movie->header.video_height
        );
        return false;
    }
    movie->h264_crop_left = crop_left;
    movie->h264_crop_top = crop_top;
    movie->h264_crop_width = crop_width;
    movie->h264_crop_height = crop_height;
    movie->h264_headers_ready = true;
    return true;
}

static inline uint32_t h264_pack_rgb565_pair(uint16_t left_pixel, uint16_t right_pixel)
{
    return ((uint32_t) right_pixel << 16) | left_pixel;
}

static bool blit_h264_planes_to_framebuffer(
    Movie *movie,
    const uint8_t *restrict y_plane,
    const uint8_t *restrict u_plane,
    const uint8_t *restrict v_plane,
    size_t luma_stride,
    size_t chroma_stride
)
{
    const int32_t *restrict y_base = g_h264_color_tables.y_base;
    const uint16_t *restrict red565 = g_h264_color_tables.red565;
    const uint16_t *restrict green565 = g_h264_color_tables.green565;
    const uint16_t *restrict blue565 = g_h264_color_tables.blue565;
    size_t y;

    if (!movie || !y_plane || !u_plane || !v_plane || !movie->framebuffer || !movie->h264_headers_ready) {
        return false;
    }

    for (y = 0; y < movie->h264_crop_height; y += 2U) {
        const uint8_t *restrict y_row0 = y_plane + ((size_t) y * luma_stride);
        const uint8_t *restrict y_row1 = y_row0 + luma_stride;
        const uint8_t *restrict u_row = u_plane + ((size_t) (y / 2U) * chroma_stride);
        const uint8_t *restrict v_row = v_plane + ((size_t) (y / 2U) * chroma_stride);
        uint32_t *restrict dst_row0 = (uint32_t *) (void *) (movie->framebuffer + ((size_t) y * movie->header.video_width));
        uint32_t *restrict dst_row1 = (uint32_t *) (void *) (movie->framebuffer + ((size_t) (y + 1U) * movie->header.video_width));
        const size_t main_width = movie->h264_crop_width & ~(size_t) 3U;
        size_t x;

        for (x = 0; x < main_width; x += 4U) {
            const size_t chroma_index = x / 2U;
            const size_t dst_index = x / 2U;
            int32_t r0;
            int32_t g0;
            int32_t b0;
            int32_t r1;
            int32_t g1;
            int32_t b1;
            uint16_t p0;
            uint16_t p1;
            uint16_t p2;
            uint16_t p3;
            int32_t luma0;
            int32_t luma1;

            h264_compute_chroma_terms(
                u_row[chroma_index],
                v_row[chroma_index],
                &r0,
                &g0,
                &b0
            );
            h264_compute_chroma_terms(
                u_row[chroma_index + 1U],
                v_row[chroma_index + 1U],
                &r1,
                &g1,
                &b1
            );

            luma0 = y_base[y_row0[x]];
            luma1 = y_base[y_row0[x + 1U]];
            p0 = (uint16_t) (
                red565[h264_clip_byte((luma0 + r0) >> 8)] |
                green565[h264_clip_byte((luma0 + g0) >> 8)] |
                blue565[h264_clip_byte((luma0 + b0) >> 8)]
            );
            p1 = (uint16_t) (
                red565[h264_clip_byte((luma1 + r0) >> 8)] |
                green565[h264_clip_byte((luma1 + g0) >> 8)] |
                blue565[h264_clip_byte((luma1 + b0) >> 8)]
            );
            luma0 = y_base[y_row0[x + 2U]];
            luma1 = y_base[y_row0[x + 3U]];
            p2 = (uint16_t) (
                red565[h264_clip_byte((luma0 + r1) >> 8)] |
                green565[h264_clip_byte((luma0 + g1) >> 8)] |
                blue565[h264_clip_byte((luma0 + b1) >> 8)]
            );
            p3 = (uint16_t) (
                red565[h264_clip_byte((luma1 + r1) >> 8)] |
                green565[h264_clip_byte((luma1 + g1) >> 8)] |
                blue565[h264_clip_byte((luma1 + b1) >> 8)]
            );
            dst_row0[dst_index] = h264_pack_rgb565_pair(p0, p1);
            dst_row0[dst_index + 1U] = h264_pack_rgb565_pair(p2, p3);

            luma0 = y_base[y_row1[x]];
            luma1 = y_base[y_row1[x + 1U]];
            p0 = (uint16_t) (
                red565[h264_clip_byte((luma0 + r0) >> 8)] |
                green565[h264_clip_byte((luma0 + g0) >> 8)] |
                blue565[h264_clip_byte((luma0 + b0) >> 8)]
            );
            p1 = (uint16_t) (
                red565[h264_clip_byte((luma1 + r0) >> 8)] |
                green565[h264_clip_byte((luma1 + g0) >> 8)] |
                blue565[h264_clip_byte((luma1 + b0) >> 8)]
            );
            luma0 = y_base[y_row1[x + 2U]];
            luma1 = y_base[y_row1[x + 3U]];
            p2 = (uint16_t) (
                red565[h264_clip_byte((luma0 + r1) >> 8)] |
                green565[h264_clip_byte((luma0 + g1) >> 8)] |
                blue565[h264_clip_byte((luma0 + b1) >> 8)]
            );
            p3 = (uint16_t) (
                red565[h264_clip_byte((luma1 + r1) >> 8)] |
                green565[h264_clip_byte((luma1 + g1) >> 8)] |
                blue565[h264_clip_byte((luma1 + b1) >> 8)]
            );
            dst_row1[dst_index] = h264_pack_rgb565_pair(p0, p1);
            dst_row1[dst_index + 1U] = h264_pack_rgb565_pair(p2, p3);
        }

        for (; x < movie->h264_crop_width; x += 2U) {
            const size_t chroma_index = x / 2U;
            const size_t dst_index = x / 2U;
            int32_t chroma_red;
            int32_t chroma_green;
            int32_t chroma_blue;
            uint16_t p0;
            uint16_t p1;
            int32_t luma0;
            int32_t luma1;

            h264_compute_chroma_terms(
                u_row[chroma_index],
                v_row[chroma_index],
                &chroma_red,
                &chroma_green,
                &chroma_blue
            );

            luma0 = y_base[y_row0[x]];
            luma1 = y_base[y_row0[x + 1U]];
            p0 = (uint16_t) (
                red565[h264_clip_byte((luma0 + chroma_red) >> 8)] |
                green565[h264_clip_byte((luma0 + chroma_green) >> 8)] |
                blue565[h264_clip_byte((luma0 + chroma_blue) >> 8)]
            );
            p1 = (uint16_t) (
                red565[h264_clip_byte((luma1 + chroma_red) >> 8)] |
                green565[h264_clip_byte((luma1 + chroma_green) >> 8)] |
                blue565[h264_clip_byte((luma1 + chroma_blue) >> 8)]
            );
            dst_row0[dst_index] = h264_pack_rgb565_pair(p0, p1);

            luma0 = y_base[y_row1[x]];
            luma1 = y_base[y_row1[x + 1U]];
            p0 = (uint16_t) (
                red565[h264_clip_byte((luma0 + chroma_red) >> 8)] |
                green565[h264_clip_byte((luma0 + chroma_green) >> 8)] |
                blue565[h264_clip_byte((luma0 + chroma_blue) >> 8)]
            );
            p1 = (uint16_t) (
                red565[h264_clip_byte((luma1 + chroma_red) >> 8)] |
                green565[h264_clip_byte((luma1 + chroma_green) >> 8)] |
                blue565[h264_clip_byte((luma1 + chroma_blue) >> 8)]
            );
            dst_row1[dst_index] = h264_pack_rgb565_pair(p0, p1);
        }
    }

    return true;
}

static bool copy_h264_picture_to_slot(Movie *movie, H264FrameSlot *slot, const uint8_t *picture)
{
    const size_t luma_stride = movie->h264_full_width;
    const size_t chroma_stride = luma_stride / 2U;
    const size_t luma_plane_size = luma_stride * movie->h264_full_height;
    const size_t chroma_plane_size = chroma_stride * (movie->h264_full_height / 2U);
    const uint8_t *src_y;
    const uint8_t *src_u;
    const uint8_t *src_v;
    uint8_t *dst_y;
    uint8_t *dst_u;
    uint8_t *dst_v;
    size_t row;
    size_t chroma_height;
    size_t dst_luma_stride;
    size_t dst_chroma_stride;

    if (!movie || !slot || !slot->data || !picture || !movie->h264_headers_ready) {
        return false;
    }

    dst_luma_stride = movie->header.video_width;
    dst_chroma_stride = dst_luma_stride / 2U;
    chroma_height = movie->header.video_height / 2U;
    dst_y = slot->data;
    dst_u = dst_y + ((size_t) movie->header.video_width * movie->header.video_height);
    dst_v = dst_u + (dst_chroma_stride * chroma_height);

    src_y = picture + ((size_t) movie->h264_crop_top * luma_stride) + movie->h264_crop_left;
    src_u = picture + luma_plane_size
        + ((size_t) (movie->h264_crop_top / 2U) * chroma_stride)
        + (movie->h264_crop_left / 2U);
    src_v = picture + luma_plane_size + chroma_plane_size
        + ((size_t) (movie->h264_crop_top / 2U) * chroma_stride)
        + (movie->h264_crop_left / 2U);

    for (row = 0; row < movie->header.video_height; ++row) {
        memcpy(
            dst_y + (row * dst_luma_stride),
            src_y + (row * luma_stride),
            dst_luma_stride
        );
    }
    for (row = 0; row < chroma_height; ++row) {
        memcpy(
            dst_u + (row * dst_chroma_stride),
            src_u + (row * chroma_stride),
            dst_chroma_stride
        );
        memcpy(
            dst_v + (row * dst_chroma_stride),
            src_v + (row * chroma_stride),
            dst_chroma_stride
        );
    }

    slot->valid = true;
    return true;
}

static bool blit_h264_picture(Movie *movie, const uint8_t *picture)
{
    const size_t luma_stride = movie->h264_full_width;
    const size_t chroma_stride = luma_stride / 2U;
    const size_t luma_plane_size = luma_stride * movie->h264_full_height;
    const size_t chroma_plane_size = chroma_stride * (movie->h264_full_height / 2U);
    const uint8_t *y_plane;
    const uint8_t *u_plane;
    const uint8_t *v_plane;

    if (!movie || !picture || !movie->framebuffer || !movie->h264_headers_ready) {
        return false;
    }

    y_plane = picture + ((size_t) movie->h264_crop_top * luma_stride) + movie->h264_crop_left;
    u_plane = picture + luma_plane_size
        + ((size_t) (movie->h264_crop_top / 2U) * chroma_stride)
        + (movie->h264_crop_left / 2U);
    v_plane = picture + luma_plane_size + chroma_plane_size
        + ((size_t) (movie->h264_crop_top / 2U) * chroma_stride)
        + (movie->h264_crop_left / 2U);

    return blit_h264_planes_to_framebuffer(movie, y_plane, u_plane, v_plane, luma_stride, chroma_stride);
}

static bool blit_h264_frame_slot(Movie *movie, const H264FrameSlot *slot)
{
    const size_t luma_plane_size = (size_t) movie->header.video_width * movie->header.video_height;
    const size_t chroma_plane_size = ((size_t) movie->header.video_width / 2U) * ((size_t) movie->header.video_height / 2U);

    if (!movie || !slot || !slot->valid || !slot->data) {
        return false;
    }

    return blit_h264_planes_to_framebuffer(
        movie,
        slot->data,
        slot->data + luma_plane_size,
        slot->data + luma_plane_size + chroma_plane_size,
        movie->header.video_width,
        movie->header.video_width / 2U
    );
}

static bool decode_h264_access_unit(
    Movie *movie,
    uint8_t *frame_data,
    size_t frame_size,
    uint32_t frame_index,
    bool blit_output,
    bool store_prefetched
)
{
    size_t consumed = 0;
    bool picture_ready = false;
    unsigned zero_advance_retries = 0;

    if (!movie || !movie->h264_decoder || !frame_data || frame_size == 0) {
        debug_failf("h264 access-unit decode invalid input size=%lu", (unsigned long) frame_size);
        return false;
    }

    while (consumed < frame_size) {
        u32 read_bytes = 0;
        u32 result = h264bsdDecode(movie->h264_decoder, frame_data + consumed, (u32) (frame_size - consumed), 0, &read_bytes);

        if (result == H264BSD_HDRS_RDY) {
            if (!refresh_h264_picture_params(movie)) {
                return false;
            }
        } else if (result == H264BSD_PIC_RDY) {
            u32 pic_id = 0;
            u32 is_idr_pic = 0;
            u32 num_err_mbs = 0;
            uint8_t *picture = h264bsdNextOutputPicture(movie->h264_decoder, &pic_id, &is_idr_pic, &num_err_mbs);
            if (!picture) {
                debug_failf("h264 picture ready but no output picture");
                return false;
            }
            if (!movie->h264_headers_ready && !refresh_h264_picture_params(movie)) {
                return false;
            }
            if (store_prefetched) {
                H264FrameSlot *slot = reserve_h264_frame_slot(movie, frame_index);
                if (!slot) {
                    debug_tracef("h264 frame ring full frame=%lu", (unsigned long) frame_index);
                    return false;
                }
                if (!copy_h264_picture_to_slot(movie, slot, picture)) {
                    debug_failf("h264 frame ring copy failed");
                    return false;
                }
            }
            if (blit_output && !blit_h264_picture(movie, picture)) {
                debug_failf("h264 blit failed");
                return false;
            }
            picture_ready = true;
        } else if (result != H264BSD_RDY) {
            debug_failf("h264 decode error result=%lu read=%lu", (unsigned long) result, (unsigned long) read_bytes);
            return false;
        }

        if (read_bytes == 0) {
            if (result == H264BSD_HDRS_RDY && zero_advance_retries < 8U) {
                zero_advance_retries++;
                debug_tracef(
                    "h264 hdrs ready retry=%u consumed=%lu size=%lu",
                    zero_advance_retries,
                    (unsigned long) consumed,
                    (unsigned long) frame_size
                );
                continue;
            }
            if (!picture_ready) {
                debug_failf(
                    "h264 decode stalled with zero-byte advance result=%lu retries=%u consumed=%lu size=%lu",
                    (unsigned long) result,
                    zero_advance_retries,
                    (unsigned long) consumed,
                    (unsigned long) frame_size
                );
            }
            return picture_ready;
        }
        zero_advance_retries = 0;
        consumed += read_bytes;
    }

    return picture_ready;
}

static bool legacy_rle16_decode(const uint8_t *src, size_t src_size, uint16_t *dst, size_t dst_words)
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
    if (!legacy_rle16_decode(payload, payload_size, movie->block_scratch, (size_t) block_w * block_h)) {
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
    if (!legacy_rle16_decode(payload, payload_size, movie->block_scratch, span_length)) {
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
static bool legacy_decode_motion_records(
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

static bool legacy_decode_frame_record(Movie *movie, const uint8_t *frame_data, size_t frame_size)
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
        return legacy_rle16_decode(
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
        if (!legacy_decode_motion_records(movie, &cursor, &remaining, record_count, movie->previous_framebuffer)) {
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
        if (!legacy_decode_motion_records(movie, &cursor, &remaining, record_count, movie->previous_framebuffer)) {
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
            debug_failf(
                "chunk view invalid table chunk=%d table=%lu storage=%lu",
                chunk_index,
                (unsigned long) entry->frame_table_offset,
                (unsigned long) movie->chunk_storage_size
            );
            return false;
        }
        payload_size = read_le32(movie->chunk_storage + local_offset);
        header_size = 4 + ((size_t) entry->frame_count * sizeof(uint32_t));
        if (local_offset + header_size + payload_size > movie->chunk_storage_size) {
            debug_failf(
                "chunk view payload overflow chunk=%d payload=%lu table=%lu storage=%lu",
                chunk_index,
                (unsigned long) payload_size,
                (unsigned long) entry->frame_table_offset,
                (unsigned long) movie->chunk_storage_size
            );
            return false;
        }
        movie->frame_offsets = (uint32_t *) calloc((size_t) entry->frame_count, sizeof(uint32_t));
        if (!movie->frame_offsets) {
            debug_failf("chunk view frame offset alloc failed chunk=%d count=%u", chunk_index, (unsigned) entry->frame_count);
            return false;
        }
        memcpy(movie->frame_offsets, movie->chunk_storage + local_offset + 4, (size_t) entry->frame_count * sizeof(uint32_t));
        movie->chunk_bytes = movie->chunk_storage + local_offset + header_size;
        movie->chunk_size = payload_size;
        return true;
    }

    if (!movie->chunk_storage) {
        debug_failf("chunk view missing chunk storage chunk=%d", chunk_index);
        return false;
    }
    movie->frame_offsets = (uint32_t *) calloc((size_t) entry->frame_count, sizeof(uint32_t));
    if (!movie->frame_offsets) {
        debug_failf("chunk view frame offset alloc failed chunk=%d count=%u", chunk_index, (unsigned) entry->frame_count);
        return false;
    }
    if (!load_frame_offsets(movie, entry, movie->chunk_storage, movie->chunk_storage_size, movie->frame_offsets)) {
        debug_failf("chunk view frame offsets load failed chunk=%d", chunk_index);
        free(movie->frame_offsets);
        movie->frame_offsets = NULL;
        return false;
    }
    movie->chunk_bytes = movie->chunk_storage;
    movie->chunk_size = movie->chunk_storage_size;
    return true;
}

static bool prefetch_deadline_reached(uint32_t deadline_ms)
{
    return (int32_t) (monotonic_clock_now_ms() - deadline_ms) >= 0;
}

static void reset_prefetched_chunk(PrefetchedChunk *chunk)
{
    if (!chunk) {
        return;
    }
    chunk->chunk_storage = NULL;
    chunk->chunk_storage_size = 0;
    chunk->chunk_index = -1;
    chunk->state = PREFETCH_IDLE;
    chunk->read_offset = 0;
    chunk->inflate_write_offset = 0;
    chunk->inflate_initialized = false;
    memset(&chunk->stream, 0, sizeof(chunk->stream));
}

static void clear_prefetched_chunk(PrefetchedChunk *chunk)
{
    if (!chunk) {
        return;
    }
    if (chunk->inflate_initialized) {
        inflateEnd(&chunk->stream);
    }
    free(chunk->chunk_storage);
    reset_prefetched_chunk(chunk);
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

static PrefetchedChunk *find_prefetch_work_chunk(Movie *movie, int current_chunk)
{
    PrefetchedChunk *best = NULL;
    int index;
    int wanted_min = current_chunk + 1;
    int wanted_max = current_chunk + PREFETCH_CHUNK_COUNT;

    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        PrefetchedChunk *candidate = &movie->prefetched[index];
        if (candidate->chunk_index < wanted_min || candidate->chunk_index > wanted_max) {
            continue;
        }
        if (candidate->state == PREFETCH_IDLE || candidate->state == PREFETCH_READY) {
            continue;
        }
        if (!best || candidate->chunk_index < best->chunk_index) {
            best = candidate;
        }
    }
    return best;
}

static bool prefetch_read_step(Movie *movie, PrefetchedChunk *chunk, bool respect_deadline)
{
    const ChunkIndexEntry *entry;
    size_t remaining;
    size_t read_size;
    size_t block_size;
    uint8_t *read_target;

    if (!movie || !chunk || chunk->chunk_index < 0) {
        return false;
    }
    entry = movie->chunk_index + chunk->chunk_index;
    if (chunk->read_offset > entry->packed_size) {
        return false;
    }
    remaining = (size_t) entry->packed_size - chunk->read_offset;
    if (remaining == 0) {
        if (entry->packed_size == entry->unpacked_size) {
            chunk->state = PREFETCH_READY;
            return true;
        }
        if (chunk->stream.avail_in == 0) {
            return false;
        }
        chunk->state = PREFETCH_INFLATING;
        return true;
    }

    block_size = respect_deadline ? PREFETCH_ACTIVE_FILE_BLOCK_SIZE : PREFETCH_FILE_BLOCK_SIZE;
    read_size = remaining > block_size ? block_size : remaining;
    read_target = entry->packed_size == entry->unpacked_size
        ? (chunk->chunk_storage + chunk->read_offset)
        : chunk->input_buffer;

    if (fseek(movie->file, (long) (entry->offset + chunk->read_offset), SEEK_SET) != 0) {
        return false;
    }
    if (fread(read_target, 1, read_size, movie->file) != read_size) {
        return false;
    }

    movie->diag_prefetch_read_ops++;
    movie->diag_prefetch_read_bytes += (uint32_t) read_size;
    chunk->read_offset += read_size;
    if (entry->packed_size == entry->unpacked_size) {
        if (chunk->read_offset == entry->packed_size) {
            chunk->state = PREFETCH_READY;
        }
        return true;
    }

    chunk->stream.next_in = chunk->input_buffer;
    chunk->stream.avail_in = (uInt) read_size;
    chunk->state = PREFETCH_INFLATING;
    return true;
}

static bool prefetch_inflate_step(Movie *movie, PrefetchedChunk *chunk)
{
    const ChunkIndexEntry *entry;
    size_t remaining_out;
    uInt input_before;
    uInt output_size;
    size_t produced;
    uInt consumed;
    int zresult;

    if (!movie || !chunk || chunk->chunk_index < 0 || !chunk->inflate_initialized) {
        return false;
    }
    entry = movie->chunk_index + chunk->chunk_index;
    if (chunk->stream.avail_in == 0) {
        if (chunk->read_offset < entry->packed_size) {
            chunk->state = PREFETCH_READING;
            return true;
        }
        return false;
    }
    if (chunk->inflate_write_offset > entry->unpacked_size) {
        return false;
    }

    remaining_out = (size_t) entry->unpacked_size - chunk->inflate_write_offset;
    if (remaining_out == 0) {
        return false;
    }

    input_before = chunk->stream.avail_in;
    output_size = (uInt) (remaining_out > PREFETCH_INFLATE_OUTPUT_SLICE ? PREFETCH_INFLATE_OUTPUT_SLICE : remaining_out);
    chunk->stream.next_out = chunk->chunk_storage + chunk->inflate_write_offset;
    chunk->stream.avail_out = output_size;
    zresult = inflate(&chunk->stream, Z_NO_FLUSH);
    produced = (size_t) (output_size - chunk->stream.avail_out);
    consumed = input_before - chunk->stream.avail_in;
    chunk->inflate_write_offset += produced;
    movie->diag_prefetch_inflate_ops++;
    movie->diag_prefetch_inflate_bytes += (uint32_t) produced;

    if (zresult == Z_STREAM_END) {
        if (chunk->inflate_write_offset != entry->unpacked_size ||
            chunk->read_offset != entry->packed_size ||
            chunk->stream.avail_in != 0) {
            return false;
        }
        inflateEnd(&chunk->stream);
        chunk->inflate_initialized = false;
        memset(&chunk->stream, 0, sizeof(chunk->stream));
        chunk->state = PREFETCH_READY;
        return true;
    }
    if (zresult != Z_OK) {
        return false;
    }
    if (produced == 0 && consumed == 0) {
        return false;
    }
    if (chunk->stream.avail_in == 0) {
        if (chunk->read_offset < entry->packed_size) {
            chunk->state = PREFETCH_READING;
        } else {
            return false;
        }
    } else {
        chunk->state = PREFETCH_INFLATING;
    }
    return true;
}

static bool prefetch_process_chunk(Movie *movie, PrefetchedChunk *chunk, uint32_t deadline_ms, bool respect_deadline)
{
    while (chunk && chunk->state != PREFETCH_READY) {
        if (chunk->state == PREFETCH_READING) {
            if (!prefetch_read_step(movie, chunk, respect_deadline)) {
                return false;
            }
        } else if (chunk->state == PREFETCH_INFLATING) {
            if (!prefetch_inflate_step(movie, chunk)) {
                return false;
            }
        } else {
            return false;
        }

        if (respect_deadline && prefetch_deadline_reached(deadline_ms)) {
            break;
        }
    }
    return true;
}

static bool prefetch_finish_chunk(Movie *movie, PrefetchedChunk *chunk)
{
    if (!chunk) {
        return false;
    }
    if (chunk->state == PREFETCH_READY) {
        return true;
    }
    if (!prefetch_process_chunk(movie, chunk, 0, false)) {
        return false;
    }
    return chunk->state == PREFETCH_READY;
}

static bool load_chunk_from_file(Movie *movie, int chunk_index, bool allow_prefetch_retry)
{
    const ChunkIndexEntry *entry = movie->chunk_index + chunk_index;
    uint8_t *packed_bytes = NULL;
    uLongf unpacked_size;

retry:
    free(movie->chunk_storage);
    movie->chunk_storage = NULL;
    movie->chunk_storage_size = 0;

    ensure_app_memory_headroom(movie, entry->packed_size, 2U);
    packed_bytes = (uint8_t *) malloc(entry->packed_size);
    if (!packed_bytes) {
        if (allow_prefetch_retry) {
            size_t released_slots;
            debug_failf(
                "load chunk=%d retry after packed alloc fail packed=%lu prefetched=%lu",
                chunk_index,
                (unsigned long) entry->packed_size,
                (unsigned long) total_prefetched_chunk_bytes(movie)
            );
            clear_all_prefetched_chunks(movie);
            released_slots = trim_h264_frame_ring(movie, 0U, 0U);
            if (released_slots > 0) {
                debug_tracef(
                    "load chunk=%d packed alloc trim released=%lu cap=%lu",
                    chunk_index,
                    (unsigned long) released_slots,
                    (unsigned long) active_h264_frame_ring_capacity(movie)
                );
            }
            ensure_app_memory_headroom(movie, entry->packed_size, 0U);
            allow_prefetch_retry = false;
            goto retry;
        }
        debug_failf("load chunk=%d packed alloc fail packed=%lu", chunk_index, (unsigned long) entry->packed_size);
        return false;
    }
    if (fseek(movie->file, (long) entry->offset, SEEK_SET) != 0) {
        debug_failf("load chunk=%d fseek fail offset=%lu", chunk_index, (unsigned long) entry->offset);
        free(packed_bytes);
        return false;
    }
    if (fread(packed_bytes, 1, entry->packed_size, movie->file) != entry->packed_size) {
        debug_failf("load chunk=%d fread fail packed=%lu", chunk_index, (unsigned long) entry->packed_size);
        free(packed_bytes);
        return false;
    }
    if (entry->packed_size != entry->unpacked_size) {
        ensure_app_memory_headroom(movie, entry->unpacked_size, 2U);
        movie->chunk_storage = (uint8_t *) malloc(entry->unpacked_size);
        if (!movie->chunk_storage) {
            free(packed_bytes);
            if (allow_prefetch_retry) {
                size_t released_slots;
                debug_failf(
                    "load chunk=%d retry after unpacked alloc fail unpacked=%lu prefetched=%lu",
                    chunk_index,
                    (unsigned long) entry->unpacked_size,
                    (unsigned long) total_prefetched_chunk_bytes(movie)
                );
                clear_all_prefetched_chunks(movie);
                released_slots = trim_h264_frame_ring(movie, 0U, 0U);
                if (released_slots > 0) {
                    debug_tracef(
                        "load chunk=%d unpacked alloc trim released=%lu cap=%lu",
                        chunk_index,
                        (unsigned long) released_slots,
                        (unsigned long) active_h264_frame_ring_capacity(movie)
                    );
                }
                ensure_app_memory_headroom(movie, entry->unpacked_size, 0U);
                allow_prefetch_retry = false;
                goto retry;
            }
            debug_failf("load chunk=%d unpacked alloc fail unpacked=%lu", chunk_index, (unsigned long) entry->unpacked_size);
            return false;
        }
        unpacked_size = entry->unpacked_size;
        if (uncompress(movie->chunk_storage, &unpacked_size, packed_bytes, entry->packed_size) != Z_OK || unpacked_size != entry->unpacked_size) {
            debug_failf(
                "load chunk=%d uncompress fail packed=%lu unpacked=%lu actual=%lu",
                chunk_index,
                (unsigned long) entry->packed_size,
                (unsigned long) entry->unpacked_size,
                (unsigned long) unpacked_size
            );
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
        debug_tracef(
            "load chunk=%d configure fail storage=%lu frame_count=%u table=%lu",
            chunk_index,
            (unsigned long) movie->chunk_storage_size,
            (unsigned) entry->frame_count,
            (unsigned long) entry->frame_table_offset
        );
        free(movie->chunk_storage);
        movie->chunk_storage = NULL;
        movie->chunk_storage_size = 0;
        return false;
    }
    movie->loaded_chunk = chunk_index;
    movie->decoded_local_frame = -1;
    if (movie_uses_h264(movie) && !reset_h264_decoder(movie)) {
        return false;
    }
    movie->h264_chunk_dirty = false;
    movie->diag_chunk_load_sync_count++;
    debug_tracef(
        "load chunk=%d sync packed=%lu unpacked=%lu prefetched=%lu",
        chunk_index,
        (unsigned long) entry->packed_size,
        (unsigned long) entry->unpacked_size,
        (unsigned long) total_prefetched_chunk_bytes(movie)
    );
    return true;
}

static bool load_chunk(Movie *movie, int chunk_index)
{
    const ChunkIndexEntry *entry;
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
        if (prefetched->state != PREFETCH_READY) {
            if (!prefetch_finish_chunk(movie, prefetched)) {
                debug_tracef(
                    "prefetch finish fail chunk=%d state=%d read=%lu write=%lu avail_in=%u",
                    chunk_index,
                    (int) prefetched->state,
                    (unsigned long) prefetched->read_offset,
                    (unsigned long) prefetched->inflate_write_offset,
                    prefetched->stream.avail_in
                );
                clear_prefetched_chunk(prefetched);
                return load_chunk_from_file(movie, chunk_index, true);
            }
        }
        free(movie->chunk_storage);
        movie->chunk_storage = prefetched->chunk_storage;
        movie->chunk_storage_size = prefetched->chunk_storage_size;
        prefetched->chunk_storage = NULL;
        reset_prefetched_chunk(prefetched);
        if (!configure_chunk_view(movie, chunk_index)) {
            debug_tracef(
                "prefetched configure fail chunk=%d storage=%lu frame_count=%u table=%lu",
                chunk_index,
                (unsigned long) movie->chunk_storage_size,
                (unsigned) entry->frame_count,
                (unsigned long) entry->frame_table_offset
            );
            free(movie->chunk_storage);
            movie->chunk_storage = NULL;
            movie->chunk_storage_size = 0;
            return load_chunk_from_file(movie, chunk_index, false);
        }
        movie->loaded_chunk = chunk_index;
        movie->decoded_local_frame = -1;
        if (movie_uses_h264(movie) && !reset_h264_decoder(movie)) {
            return false;
        }
        movie->h264_chunk_dirty = false;
        movie->diag_chunk_load_prefetched_count++;
        debug_tracef(
            "load chunk=%d prefetched packed=%lu unpacked=%lu prefetched=%lu",
            chunk_index,
            (unsigned long) entry->packed_size,
            (unsigned long) entry->unpacked_size,
            (unsigned long) total_prefetched_chunk_bytes(movie)
        );
        return true;
    }

    return load_chunk_from_file(movie, chunk_index, true);
}

static bool prefetch_chunk(Movie *movie, int chunk_index)
{
    const ChunkIndexEntry *entry;
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
    if (!ensure_prefetch_budget(movie, chunk_index, entry->unpacked_size)) {
        return false;
    }
    if (!ensure_app_memory_headroom(movie, entry->unpacked_size, H264_PREFETCH_MIN_READY_FRAMES)) {
        return false;
    }
    clear_prefetched_chunk(slot);
    slot->chunk_storage = (uint8_t *) malloc(entry->unpacked_size);
    if (!slot->chunk_storage) {
        size_t released_slots = trim_h264_frame_ring(movie, 2U, H264_PREFETCH_MIN_READY_FRAMES);
        if (released_slots > 0) {
            debug_tracef(
                "prefetch alloc trim chunk=%d released=%lu cap=%lu",
                chunk_index,
                (unsigned long) released_slots,
                (unsigned long) active_h264_frame_ring_capacity(movie)
            );
        }
        if (ensure_app_memory_headroom(movie, entry->unpacked_size, 2U)) {
            slot->chunk_storage = (uint8_t *) malloc(entry->unpacked_size);
        }
    }
    if (!slot->chunk_storage) {
        debug_tracef(
            "prefetch alloc fail chunk=%d unpacked=%lu total=%lu",
            chunk_index,
            (unsigned long) entry->unpacked_size,
            (unsigned long) total_prefetched_chunk_bytes(movie)
        );
        return false;
    }
    slot->chunk_storage_size = entry->unpacked_size;
    slot->chunk_index = chunk_index;
    slot->state = PREFETCH_READING;
    slot->read_offset = 0;
    slot->inflate_write_offset = 0;

    if (entry->packed_size != entry->unpacked_size) {
        memset(&slot->stream, 0, sizeof(slot->stream));
        if (inflateInit(&slot->stream) != Z_OK) {
            debug_tracef("prefetch inflateInit fail chunk=%d", chunk_index);
            clear_prefetched_chunk(slot);
            return false;
        }
        slot->inflate_initialized = true;
    }
    debug_tracef(
        "prefetch start chunk=%d packed=%lu unpacked=%lu total=%lu",
        chunk_index,
        (unsigned long) entry->packed_size,
        (unsigned long) entry->unpacked_size,
        (unsigned long) total_prefetched_chunk_bytes(movie)
    );
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

static void prefetch_do_work(Movie *movie, int current_chunk, uint32_t deadline_ms)
{
    while (!prefetch_deadline_reached(deadline_ms)) {
        PrefetchedChunk *slot = find_prefetch_work_chunk(movie, current_chunk);
        if (!slot) {
            break;
        }
        if (!prefetch_process_chunk(movie, slot, deadline_ms, true)) {
            debug_tracef(
                "prefetch work fail chunk=%d state=%d read=%lu write=%lu avail_in=%u",
                slot->chunk_index,
                (int) slot->state,
                (unsigned long) slot->read_offset,
                (unsigned long) slot->inflate_write_offset,
                slot->stream.avail_in
            );
            clear_prefetched_chunk(slot);
            break;
        }
    }
}

static int prefetch_budget_for_state(const Movie *movie, bool paused, uint32_t spare_ms)
{
    if (paused) {
        return PREFETCH_CHUNK_COUNT;
    }
    if (movie_uses_h264(movie) && spare_ms < PREFETCH_ACTIVE_H264_MIN_SPARE_MS) {
        return 0;
    }
    if (spare_ms >= 24U) {
        return 2;
    }
    if (spare_ms > 0) {
        return 1;
    }
    return 0;
}
 
static int prefetch_target_chunk(const Movie *movie)
{
    int current_chunk;

    if (!movie) {
        return -1;
    }

    current_chunk = movie_chunk_for_frame(movie, movie->current_frame);
    if (current_chunk >= 0) {
        return current_chunk;
    }

    if (movie->loaded_chunk < 0) {
        return -1;
    }
    if ((uint32_t) movie->loaded_chunk >= movie->header.chunk_count) {
        return -1;
    }
    return movie->loaded_chunk;
}

static bool next_chunk_needs_prefetch(const Movie *movie, int current_chunk)
{
    int index;
    int next_chunk = current_chunk + 1;

    if (!movie || current_chunk < 0 || (uint32_t) next_chunk >= movie->header.chunk_count) {
        return false;
    }
    if (movie->loaded_chunk == next_chunk) {
        return false;
    }
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        const PrefetchedChunk *prefetched = &movie->prefetched[index];
        if (prefetched->chunk_index == next_chunk) {
            return prefetched->state != PREFETCH_READY;
        }
    }
    return true;
}

static uint32_t next_chunk_prefetch_guard_frames(const Movie *movie, int current_chunk)
{
    const ChunkIndexEntry *entry;
    uint32_t guard_frames = H264_PREFETCH_NEXT_CHUNK_GUARD_FRAMES;
    int next_chunk = current_chunk + 1;

    if (!movie || current_chunk < 0 || (uint32_t) next_chunk >= movie->header.chunk_count) {
        return guard_frames;
    }

    entry = movie->chunk_index + next_chunk;
    if (entry->unpacked_size >= 768U * 1024U) {
        guard_frames += 16U;
    } else if (entry->unpacked_size >= 512U * 1024U) {
        guard_frames += 8U;
    } else if (entry->unpacked_size >= 256U * 1024U) {
        guard_frames += 4U;
    }
    return guard_frames;
}

static bool should_prioritize_next_chunk_io(const Movie *movie, int current_chunk)
{
    const ChunkIndexEntry *entry;
    uint32_t frames_remaining;
    uint32_t guard_frames;

    if (!movie || current_chunk < 0 || (uint32_t) current_chunk >= movie->header.chunk_count) {
        return false;
    }
    if (!next_chunk_needs_prefetch(movie, current_chunk)) {
        return false;
    }

    entry = movie->chunk_index + current_chunk;
    guard_frames = next_chunk_prefetch_guard_frames(movie, current_chunk);
    if (movie->current_frame < entry->first_frame) {
        return true;
    }

    frames_remaining = (entry->first_frame + entry->frame_count) - movie->current_frame;
    return frames_remaining <= guard_frames;
}

static void prefetch_h264_frames(Movie *movie, bool paused, uint32_t spare_ms, uint32_t deadline_ms)
{
    size_t target_ready_count;
    size_t contiguous_ready_count;
    uint32_t decode_guard_ms;
    uint32_t max_decodes_this_tick;
    uint32_t decoded_this_tick = 0;
    int playback_chunk = -1;
    int current_chunk = -1;

    if (!movie || !movie_uses_h264(movie) || movie->header.frame_count == 0 || active_h264_frame_ring_capacity(movie) == 0) {
        return;
    }

    discard_h264_frame_ring_before(movie, movie->current_frame + 1U);
    target_ready_count = paused ? active_h264_frame_ring_capacity(movie) : h264_prefetch_target_ready_count(movie, spare_ms);
    contiguous_ready_count = h264_frame_ring_contiguous_ready_count(movie);
    if (!paused && spare_ms < H264_PREFETCH_ACTIVE_MIN_SPARE_MS) {
        return;
    }
    if (!paused) {
        playback_chunk = movie_chunk_for_frame(movie, movie->current_frame);
    }
    current_chunk = movie_chunk_for_frame(movie, movie->current_frame);
    max_decodes_this_tick = paused
        ? UINT32_MAX
        : h264_prefetch_active_decode_limit(contiguous_ready_count, target_ready_count, spare_ms);
    decode_guard_ms = h264_prefetch_decode_guard_ms(movie);

    while (contiguous_ready_count < target_ready_count && decoded_this_tick < max_decodes_this_tick) {
        int64_t decoded_frame = h264_decoded_global_frame(movie);
        uint32_t next_frame = (decoded_frame >= (int64_t) movie->current_frame)
            ? ((uint32_t) decoded_frame + 1U)
            : (movie->current_frame + 1U);
        int next_chunk;
        bool same_chunk;
        PrefetchedChunk *prefetched;
        uint32_t decode_start_ms;
        uint32_t decode_elapsed_ms;

        if (next_frame >= movie->header.frame_count) {
            break;
        }
        next_chunk = movie_chunk_for_frame(movie, next_frame);
        if (next_chunk < 0) {
            break;
        }
        same_chunk = (current_chunk >= 0 && next_chunk == current_chunk);
        if (!paused && playback_chunk >= 0 && next_chunk != playback_chunk) {
            /* Do not let the background decoder cross chunk boundaries.
             * It mutates the shared playback chunk/decoder state and can
             * force the foreground back into synchronous re-reads of the
             * previous chunk on the next ring miss. */
            movie->diag_bg_chunk_cross_blocked_count++;
            break;
        }
        if (!paused && deadline_ms != 0) {
            uint32_t now_ms = monotonic_clock_now_ms();
            if ((int32_t) ((now_ms + decode_guard_ms) - deadline_ms) >= 0) {
                break;
            }
        }
        if (!paused && h264_prefetch_deadline_reached(deadline_ms)) {
            break;
        }
        if (!paused && movie->loaded_chunk != next_chunk) {
            prefetched = find_prefetched_chunk(movie, next_chunk);
            if (!prefetched || prefetched->state != PREFETCH_READY) {
                break;
            }
        }
        decode_start_ms = monotonic_clock_now_ms();
        if (!decode_h264_frame(movie, next_frame, false, true)) {
            movie->diag_prefetch_frame_decode_fail_count++;
            debug_tracef("h264 predecode fail frame=%lu", (unsigned long) next_frame);
            break;
        }
        decode_elapsed_ms = monotonic_clock_now_ms() - decode_start_ms;
        movie->diag_prefetch_frame_decode_count++;
        record_h264_prefetch_decode_time(movie, decode_elapsed_ms);
        record_h264_background_decode_kind(movie, same_chunk, decode_elapsed_ms);
        if (decode_elapsed_ms >= DEBUG_TRACE_PREFETCH_MS) {
            debug_tracef(
                "prefetch frame=%lu ms=%lu contig=%lu target=%lu chunk=%d",
                (unsigned long) next_frame,
                (unsigned long) decode_elapsed_ms,
                (unsigned long) contiguous_ready_count,
                (unsigned long) target_ready_count,
                next_chunk
            );
        }
        decoded_this_tick++;
        contiguous_ready_count = h264_frame_ring_contiguous_ready_count(movie);
        if (!paused && h264_prefetch_deadline_reached(deadline_ms)) {
            break;
        }
    }
}

static void prefetch_tick(Movie *movie, bool paused, uint32_t spare_ms)
{
    uint32_t time_slice_ms = paused && spare_ms > PREFETCH_PAUSED_SLICE_MS ? PREFETCH_PAUSED_SLICE_MS : spare_ms;
    uint32_t deadline_ms = spare_ms > 0 ? (monotonic_clock_now_ms() + spare_ms) : 0;
    int current_chunk = prefetch_target_chunk(movie);
    int budget = prefetch_budget_for_state(movie, paused, spare_ms);
    bool prioritize_io = false;
    bool allow_active_h264_prefetch = false;
    size_t ring_growth = paused ? 8U : 0U;

    if (movie_uses_h264(movie)) {
        /* Keep decoded-frame H.264 prefetch as paused-only warmup.
         * During active playback this work runs on the same cooperative loop
         * as presentation and steals deadline-critical time. */
        movie->diag_prefetch_tick_count++;
        if (!paused) {
            movie->diag_active_prefetch_tick_count++;
            if (movie->h264_active_prefetch_backoff > 0) {
                movie->diag_prefetch_backoff_skip_count++;
                movie->h264_active_prefetch_backoff--;
            }
            movie->diag_prefetch_suppressed_count++;
        }
        allow_active_h264_prefetch = paused;
    }

    if (!paused && movie_uses_h264(movie) && current_chunk >= 0) {
        prioritize_io = should_prioritize_next_chunk_io(movie, current_chunk);
        if (prioritize_io) {
            movie->diag_io_priority_count++;
            if (budget < 1) {
                budget = 1;
            }
            if (time_slice_ms < H264_PREFETCH_IO_PRIORITY_SLICE_MS && spare_ms > 0) {
                time_slice_ms = spare_ms < H264_PREFETCH_IO_PRIORITY_SLICE_MS
                    ? spare_ms
                    : H264_PREFETCH_IO_PRIORITY_SLICE_MS;
            }
        }
    }

    if (paused && movie_uses_h264(movie)) {
        try_grow_h264_frame_ring(movie, ring_growth);
    }
    if (!paused && movie_uses_h264(movie) && !prioritize_io && time_slice_ms > PREFETCH_ACTIVE_H264_SLICE_MS) {
        time_slice_ms = PREFETCH_ACTIVE_H264_SLICE_MS;
    }
    if (paused && movie_uses_h264(movie) && allow_active_h264_prefetch && !prioritize_io) {
        prefetch_h264_frames(movie, paused, spare_ms, deadline_ms);
    }
    if (current_chunk >= 0 && budget > 0 && time_slice_ms > 0) {
        uint32_t io_deadline_ms = monotonic_clock_now_ms() + time_slice_ms;
        if (!io_deadline_ms || !prefetch_deadline_reached(io_deadline_ms)) {
            prefetch_ahead(movie, current_chunk, budget);
            prefetch_do_work(movie, current_chunk, io_deadline_ms);
        }
    }
    if (paused &&
        movie_uses_h264(movie) &&
        allow_active_h264_prefetch &&
        (!deadline_ms || !h264_prefetch_deadline_reached(deadline_ms))) {
        prefetch_h264_frames(movie, paused, spare_ms, deadline_ms);
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

static bool decode_h264_frame(Movie *movie, uint32_t frame_index, bool blit_output, bool store_prefetched)
{
    int chunk_index = movie_chunk_for_frame(movie, frame_index);
    const ChunkIndexEntry *entry;
    uint32_t local_index;
    uint32_t replay_index;

    if (chunk_index < 0) {
        debug_failf("decode frame=%lu invalid h264 chunk", (unsigned long) frame_index);
        return false;
    }
    if (!load_chunk(movie, chunk_index)) {
        debug_tracef("decode frame=%lu load h264 chunk=%d fail", (unsigned long) frame_index, chunk_index);
        return false;
    }

    entry = movie->chunk_index + chunk_index;
    local_index = frame_index - entry->first_frame;
    if (movie->decoded_local_frame > (int) local_index) {
        uint32_t replay_distance = (uint32_t) (movie->decoded_local_frame - (int) local_index);
        movie->diag_h264_replay_count++;
        movie->diag_h264_replay_frames_total += replay_distance;
        if (replay_distance > movie->diag_h264_replay_max_distance) {
            movie->diag_h264_replay_max_distance = replay_distance;
        }
        debug_tracef(
            "h264 replay frame=%lu chunk=%d local=%lu decoded_local=%d dirty=%u",
            (unsigned long) frame_index,
            chunk_index,
            (unsigned long) local_index,
            movie->decoded_local_frame,
            movie->h264_chunk_dirty ? 1U : 0U
        );
        if (movie->h264_chunk_dirty) {
            if (!load_chunk_from_file(movie, chunk_index, true)) {
                debug_failf("h264 chunk reload failed chunk=%d for replay", chunk_index);
                return false;
            }
            entry = movie->chunk_index + chunk_index;
            local_index = frame_index - entry->first_frame;
        } else if (!reset_h264_decoder(movie)) {
            return false;
        }
        movie->decoded_local_frame = -1;
    }

    for (replay_index = (uint32_t) (movie->decoded_local_frame + 1); replay_index <= local_index; ++replay_index) {
        size_t start = movie->frame_offsets[replay_index];
        size_t end = (replay_index + 1 < entry->frame_count)
            ? movie->frame_offsets[replay_index + 1]
            : movie->chunk_size;
        uint32_t decoded_frame_index = entry->first_frame + replay_index;

        movie->h264_chunk_dirty = true;
        if (!decode_h264_access_unit(
                movie,
                movie->chunk_bytes + start,
                end - start,
                decoded_frame_index,
                blit_output && decoded_frame_index == frame_index,
                store_prefetched && decoded_frame_index == frame_index)) {
            debug_tracef(
                "h264 frame decode fail frame=%lu chunk=%d local=%lu start=%lu end=%lu size=%lu",
                (unsigned long) decoded_frame_index,
                chunk_index,
                (unsigned long) replay_index,
                (unsigned long) start,
                (unsigned long) end,
                (unsigned long) (end - start)
            );
            return false;
        }
        movie->decoded_local_frame = (int) replay_index;
    }

    return true;
}

static bool decode_to_frame(Movie *movie, uint32_t frame_index)
{
    int chunk_index = movie_chunk_for_frame(movie, frame_index);
    const ChunkIndexEntry *entry;
    H264FrameSlot *prefetched_frame = NULL;
    uint32_t local_index;
    uint32_t replay_index;
    bool have_contiguous_seed = false;

    if (chunk_index < 0) {
        debug_failf("decode frame=%lu invalid chunk", (unsigned long) frame_index);
        return false;
    }
    if (movie_uses_h264(movie) && frame_index > movie->current_frame) {
        int current_chunk = movie_chunk_for_frame(movie, movie->current_frame);
        prefetched_frame = find_h264_frame_slot(movie, frame_index);
        if (prefetched_frame) {
            movie->diag_foreground_ring_hit_count++;
            if (!blit_h264_frame_slot(movie, prefetched_frame)) {
                debug_failf("h264 frame ring blit failed frame=%lu", (unsigned long) frame_index);
                return false;
            }
            movie->current_frame = frame_index;
            discard_h264_frame_ring_before(movie, frame_index + 1U);
            return true;
        }
        movie->diag_ring_miss_not_cached_count++;
        if (h264_frame_ring_valid_count(movie) > 0U && h264_frame_ring_contiguous_ready_count(movie) == 0U) {
            movie->diag_ring_miss_not_contiguous_count++;
        }
        if (current_chunk >= 0 && chunk_index == current_chunk + 1 && next_chunk_needs_prefetch(movie, current_chunk)) {
            movie->diag_ring_miss_chunk_not_ready_count++;
        }
        if (active_h264_frame_ring_capacity(movie) > 0U &&
            h264_frame_ring_valid_count(movie) >= active_h264_frame_ring_capacity(movie)) {
            movie->diag_ring_miss_slot_unavailable_count++;
        }
        {
            const ChunkIndexEntry *miss_entry = movie->chunk_index + chunk_index;
            uint32_t miss_local_index = frame_index - miss_entry->first_frame;
            if (miss_local_index < H264_BOUNDARY_MISS_WINDOW_FRAMES) {
                movie->diag_chunk_boundary_miss_count++;
            }
        }
    }
    if (movie_uses_h264(movie) && frame_index < movie->current_frame) {
        clear_h264_frame_ring(movie);
    }
    if (!load_chunk(movie, chunk_index)) {
        debug_tracef("decode frame=%lu load chunk=%d fail", (unsigned long) frame_index, chunk_index);
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
    if (!movie_uses_h264(movie) &&
        movie->decoded_local_frame < 0 &&
        chunk_index > 0 &&
        entry->frame_count > 0 &&
        movie->chunk_bytes[0] != 'I') {
        if (!have_contiguous_seed) {
            uint32_t seed_frame = entry->first_frame - 1;
            if (!decode_to_frame(movie, seed_frame)) {
                debug_tracef(
                    "decode frame=%lu seed frame=%lu fail chunk=%d",
                    (unsigned long) frame_index,
                    (unsigned long) seed_frame,
                    chunk_index
                );
                return false;
            }
            if (!load_chunk(movie, chunk_index)) {
                debug_tracef("decode frame=%lu reload chunk=%d after seed fail", (unsigned long) frame_index, chunk_index);
                return false;
            }
            entry = movie->chunk_index + chunk_index;
            local_index = frame_index - entry->first_frame;
        }
    }
    if (movie_uses_h264(movie)) {
        movie->diag_foreground_direct_decode_count++;
        if (!decode_h264_frame(movie, frame_index, true, false)) {
            return false;
        }
        movie->current_frame = frame_index;
        discard_h264_frame_ring_before(movie, frame_index + 1U);
        return true;
    }
    if (movie->decoded_local_frame > (int) local_index) {
        movie->decoded_local_frame = -1;
    }
    for (replay_index = (uint32_t) (movie->decoded_local_frame + 1); replay_index <= local_index; ++replay_index) {
        size_t start = movie->frame_offsets[replay_index];
        size_t end = (replay_index + 1 < entry->frame_count)
            ? movie->frame_offsets[replay_index + 1]
            : movie->chunk_size;
        if (!legacy_decode_frame_record(movie, movie->chunk_bytes + start, end - start)) {
            debug_failf(
                "frame decode fail frame=%lu chunk=%d local=%lu tag=%c start=%lu end=%lu size=%lu",
                (unsigned long) (entry->first_frame + replay_index),
                chunk_index,
                (unsigned long) replay_index,
                (end > start && movie->chunk_bytes) ? movie->chunk_bytes[start] : '?',
                (unsigned long) start,
                (unsigned long) end,
                (unsigned long) (end - start)
            );
            return false;
        }
        movie->decoded_local_frame = (int) replay_index;
    }
    movie->current_frame = frame_index;
    return true;
}

static bool load_subtitles(
    FILE *file,
    const MovieHeader *header,
    SubtitleCue **out_cues,
    SubtitleTrack **out_tracks,
    uint16_t *out_track_count
)
{
    SubtitleCue *cues = NULL;
    SubtitleTrack *tracks = NULL;
    uint32_t cue_index = 0;
    uint32_t track_index = 0;

    *out_cues = NULL;
    *out_tracks = NULL;
    *out_track_count = 0;
    if (!header->subtitle_count) {
        debug_tracef("open subtitles none");
        return true;
    }

    cues = (SubtitleCue *) calloc(header->subtitle_count, sizeof(SubtitleCue));
    if (!cues) {
        debug_failf("subtitle cue alloc failed count=%lu", (unsigned long) header->subtitle_count);
        return false;
    }
    if (fseek(file, (long) header->subtitle_offset, SEEK_SET) != 0) {
        debug_failf("subtitle seek failed offset=%lu", (unsigned long) header->subtitle_offset);
        free(cues);
        return false;
    }

    if (header->version >= 8) {
        uint8_t track_count_bytes[2];
        uint32_t cue_cursor = 0;
        if (fread(track_count_bytes, 1, sizeof(track_count_bytes), file) != sizeof(track_count_bytes)) {
            debug_failf("subtitle track count read failed");
            free(cues);
            return false;
        }
        *out_track_count = read_le16(track_count_bytes);
        if (*out_track_count == 0) {
            debug_tracef("open subtitles zero tracks");
            free(cues);
            return true;
        }
        tracks = (SubtitleTrack *) calloc(*out_track_count, sizeof(SubtitleTrack));
        if (!tracks) {
            debug_failf("subtitle track alloc failed count=%u", (unsigned) *out_track_count);
            free(cues);
            return false;
        }
        for (track_index = 0; track_index < *out_track_count; ++track_index) {
            uint8_t meta[6];
            uint16_t name_len;
            if (fread(meta, 1, sizeof(meta), file) != sizeof(meta)) {
                debug_failf("subtitle track meta read failed track=%lu", (unsigned long) track_index);
                free(cues);
                free(tracks);
                return false;
            }
            name_len = read_le16(meta);
            tracks[track_index].cue_start = cue_cursor;
            tracks[track_index].cue_count = read_le32(meta + 2);
            if (cue_cursor + tracks[track_index].cue_count > header->subtitle_count) {
                debug_failf(
                    "subtitle cue range overflow track=%lu start=%lu count=%lu total=%lu",
                    (unsigned long) track_index,
                    (unsigned long) cue_cursor,
                    (unsigned long) tracks[track_index].cue_count,
                    (unsigned long) header->subtitle_count
                );
                free(cues);
                free(tracks);
                return false;
            }
            tracks[track_index].name = (char *) malloc(name_len + 1);
            if (!tracks[track_index].name) {
                debug_failf("subtitle track name alloc failed track=%lu len=%u", (unsigned long) track_index, (unsigned) name_len);
                free(cues);
                free(tracks);
                return false;
            }
            if (fread(tracks[track_index].name, 1, name_len, file) != name_len) {
                debug_failf("subtitle track name read failed track=%lu len=%u", (unsigned long) track_index, (unsigned) name_len);
                free(cues);
                free(tracks[track_index].name);
                free(tracks);
                return false;
            }
            tracks[track_index].name[name_len] = '\0';
            cue_cursor += tracks[track_index].cue_count;
        }
        if (cue_cursor != header->subtitle_count) {
            debug_failf(
                "subtitle cue count mismatch tracks=%lu cues=%lu expected=%lu",
                (unsigned long) *out_track_count,
                (unsigned long) cue_cursor,
                (unsigned long) header->subtitle_count
            );
            free(cues);
            for (track_index = 0; track_index < *out_track_count; ++track_index) {
                free(tracks[track_index].name);
            }
            free(tracks);
            return false;
        }
    } else {
        tracks = (SubtitleTrack *) calloc(1, sizeof(SubtitleTrack));
        if (!tracks) {
            debug_failf("subtitle legacy track alloc failed");
            free(cues);
            return false;
        }
        tracks[0].name = dup_string("Subtitles");
        if (!tracks[0].name) {
            debug_failf("subtitle legacy track name alloc failed");
            free(cues);
            free(tracks);
            return false;
        }
        tracks[0].cue_start = 0;
        tracks[0].cue_count = header->subtitle_count;
        *out_track_count = 1;
    }

    for (cue_index = 0; cue_index < header->subtitle_count; ++cue_index) {
        uint8_t meta[10];
        uint16_t text_len;
        if (fread(meta, 1, sizeof(meta), file) != sizeof(meta)) {
            debug_failf("subtitle cue meta read failed cue=%lu", (unsigned long) cue_index);
            goto fail;
        }
        cues[cue_index].start_ms = read_le32(meta);
        cues[cue_index].end_ms = read_le32(meta + 4);
        text_len = read_le16(meta + 8);
        cues[cue_index].text = (char *) malloc(text_len + 1);
        if (!cues[cue_index].text) {
            debug_failf("subtitle text alloc failed cue=%lu len=%u", (unsigned long) cue_index, (unsigned) text_len);
            goto fail;
        }
        if (fread(cues[cue_index].text, 1, text_len, file) != text_len) {
            debug_failf("subtitle text read failed cue=%lu len=%u", (unsigned long) cue_index, (unsigned) text_len);
            goto fail;
        }
        cues[cue_index].text[text_len] = '\0';
    }

    debug_tracef(
        "open subtitles loaded tracks=%u cues=%lu",
        (unsigned) *out_track_count,
        (unsigned long) header->subtitle_count
    );
    *out_cues = cues;
    *out_tracks = tracks;
    return true;

fail:
    if (cues) {
        for (cue_index = 0; cue_index < header->subtitle_count; ++cue_index) {
            free(cues[cue_index].text);
        }
    }
    if (tracks) {
        for (track_index = 0; track_index < *out_track_count; ++track_index) {
            free(tracks[track_index].name);
        }
    }
    free(cues);
    free(tracks);
    *out_track_count = 0;
    return false;
}

static bool load_movie(const char *path, Movie *movie)
{
    size_t framebuffer_words;
    uint32_t chunk_index_read;
    memset(movie, 0, sizeof(*movie));
    movie->loaded_chunk = -1;
    {
        int h264_ring_index;
        int prefetch_index;
        for (prefetch_index = 0; prefetch_index < PREFETCH_CHUNK_COUNT; ++prefetch_index) {
            movie->prefetched[prefetch_index].chunk_index = -1;
        }
        for (h264_ring_index = 0; h264_ring_index < H264_FRAME_RING_MAX_COUNT; ++h264_ring_index) {
            movie->h264_frame_ring[h264_ring_index].frame_index = UINT32_MAX;
        }
    }
    movie->decoded_local_frame = -1;
    debug_tracef("open start path=%s", path ? path : "(null)");

    movie->file = fopen(path, "rb");
    if (!movie->file) {
        debug_failf("open failed: fopen");
        return false;
    }
    if (fread(&movie->header, 1, sizeof(movie->header), movie->file) != sizeof(movie->header)) {
        debug_failf("open failed: header read");
        return false;
    }
    if (memcmp(movie->header.magic, "NVP1", 4) != 0) {
        debug_failf("open failed: bad magic");
        return false;
    }
    if (movie->header.version < 2 || movie->header.version > 9) {
        debug_failf("open failed: unsupported version=%u", (unsigned) movie->header.version);
        return false;
    }
    debug_tracef(
        "open header version=%u video=%ux%u frames=%lu chunks=%lu subtitles=%lu",
        (unsigned) movie->header.version,
        (unsigned) movie->header.video_width,
        (unsigned) movie->header.video_height,
        (unsigned long) movie->header.frame_count,
        (unsigned long) movie->header.chunk_count,
        (unsigned long) movie->header.subtitle_count
    );
    movie->chunk_index = (ChunkIndexEntry *) calloc(movie->header.chunk_count, sizeof(ChunkIndexEntry));
    if (!movie->chunk_index) {
        debug_failf("open failed: chunk index alloc count=%lu", (unsigned long) movie->header.chunk_count);
        return false;
    }
    if (fseek(movie->file, (long) movie->header.index_offset, SEEK_SET) != 0) {
        debug_failf("open failed: index seek offset=%lu", (unsigned long) movie->header.index_offset);
        return false;
    }
    if (movie->header.version >= 3) {
        if (fread(movie->chunk_index, sizeof(ChunkIndexEntry), movie->header.chunk_count, movie->file) != movie->header.chunk_count) {
            debug_failf("open failed: chunk index read count=%lu", (unsigned long) movie->header.chunk_count);
            return false;
        }
    } else {
        for (chunk_index_read = 0; chunk_index_read < movie->header.chunk_count; ++chunk_index_read) {
            LegacyChunkIndexEntry legacy_entry;
            if (fread(&legacy_entry, sizeof(legacy_entry), 1, movie->file) != 1) {
                debug_failf("open failed: legacy chunk index read chunk=%lu", (unsigned long) chunk_index_read);
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
    debug_tracef("open index loaded chunks=%lu", (unsigned long) movie->header.chunk_count);
    framebuffer_words = (size_t) movie->header.video_width * movie->header.video_height;
    movie->framebuffer = (uint16_t *) calloc(framebuffer_words, sizeof(uint16_t));
    if (movie_uses_h264(movie)) {
        init_h264_color_tables();
        movie->h264_decoder = h264bsdAlloc();
        if (movie->h264_decoder) {
            memset(movie->h264_decoder, 0, sizeof(*movie->h264_decoder));
        }
        movie->h264_frame_bytes = h264_cropped_frame_bytes(movie);
        movie->h264_frame_ring_capacity = 0;
        clear_h264_frame_ring(movie);
        if (!movie->framebuffer || !movie->h264_decoder || !reset_h264_decoder(movie)) {
            if (!movie->framebuffer) {
                debug_failf("open failed: framebuffer alloc words=%lu", (unsigned long) framebuffer_words);
            } else if (!movie->h264_decoder) {
                debug_failf("open failed: h264 decoder alloc");
            }
            return false;
        }
    } else {
        movie->previous_framebuffer = (uint16_t *) calloc(framebuffer_words, sizeof(uint16_t));
        movie->block_scratch = (uint16_t *) malloc(
            (size_t) ((movie->header.video_width > (movie->header.block_size * movie->header.block_size))
                ? movie->header.video_width
                : (movie->header.block_size * movie->header.block_size)) * sizeof(uint16_t)
        );
        if (!movie->framebuffer || !movie->previous_framebuffer || !movie->block_scratch) {
            debug_failf("open failed: legacy buffers alloc");
            return false;
        }
    }
    if (!movie->framebuffer) {
        debug_failf("open failed: framebuffer missing");
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
        debug_failf("open failed: SDL surface create");
        return false;
    }
    if (!decode_to_frame(movie, 0)) {
        debug_tracef("open failed during initial frame decode");
        return false;
    }
    debug_tracef("open first frame ok");
    if (!load_subtitles(movie->file, &movie->header, &movie->subtitles, &movie->subtitle_tracks, &movie->subtitle_track_count)) {
        debug_tracef("open subtitles disabled after alloc/read failure");
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
    char history_path[MAX_PATH_LEN];
    HistoryStore history;
    bool have_history = false;
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
    history_path_for_directory(directory, history_path, sizeof(history_path));
    have_history = load_history_store_from_path(history_path, &history);
    while ((entry = readdir(dir)) && count < PICKER_MAX_FILES) {
        char joined[MAX_PATH_LEN];
        int joined_len;
        size_t history_index;
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
            if (have_history) {
                free_history_store(&history);
            }
            free_movie_files(files, count + 1);
            *out_count = 0;
            return NULL;
        }
        if (have_history) {
            for (history_index = 0; history_index < history.count; ++history_index) {
                if (strcmp(history.entries[history_index].path, files[count].path) == 0) {
                    files[count].has_resume = true;
                    files[count].resume_frame = history.entries[history_index].frame;
                    break;
                }
            }
        }
        count++;
    }
    closedir(dir);
    if (have_history) {
        free_history_store(&history);
    }
    qsort(files, count, sizeof(MovieFile), compare_movie_files);
    *out_count = count;
    return files;
}

static const char *active_subtitle(const Movie *movie, uint32_t now_ms)
{
    uint32_t index;
    uint32_t start_index = 0;
    uint32_t end_index = movie->header.subtitle_count;

    if (movie->subtitle_track_count > 0 && movie->selected_subtitle_track < movie->subtitle_track_count) {
        start_index = movie->subtitle_tracks[movie->selected_subtitle_track].cue_start;
        end_index = start_index + movie->subtitle_tracks[movie->selected_subtitle_track].cue_count;
    }
    for (index = start_index; index < end_index; ++index) {
        if (now_ms >= movie->subtitles[index].start_ms && now_ms <= movie->subtitles[index].end_ms) {
            return movie->subtitles[index].text;
        }
    }
    return NULL;
}

static const char *active_subtitle_track_name(const Movie *movie)
{
    if (!movie || movie->subtitle_track_count == 0 || movie->selected_subtitle_track >= movie->subtitle_track_count) {
        return "No subtitles";
    }
    if (!movie->subtitle_tracks[movie->selected_subtitle_track].name || movie->subtitle_tracks[movie->selected_subtitle_track].name[0] == '\0') {
        return "Subtitles";
    }
    return movie->subtitle_tracks[movie->selected_subtitle_track].name;
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

static int subtitle_font_id_for_index(size_t subtitle_font_index)
{
    if (subtitle_font_index >= SUBTITLE_FONT_CHOICE_COUNT) {
        return g_subtitle_font_choices[SUBTITLE_FONT_DEFAULT_INDEX];
    }
    return g_subtitle_font_choices[subtitle_font_index];
}

static const char *subtitle_font_name_for_index(size_t subtitle_font_index)
{
    if (subtitle_font_index >= SUBTITLE_FONT_CHOICE_COUNT) {
        return g_subtitle_font_names[SUBTITLE_FONT_DEFAULT_INDEX];
    }
    return g_subtitle_font_names[subtitle_font_index];
}

static SubtitlePlacement subtitle_opposite_placement(SubtitlePlacement placement)
{
    switch (placement) {
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

static void subtitle_fonts_for_style(const Fonts *fonts, size_t subtitle_font_index, nSDL_Font **white_font, nSDL_Font **outline_font)
{
    int font_id = subtitle_font_id_for_index(subtitle_font_index);

    *white_font = fonts->subtitle_white[font_id];
    *outline_font = fonts->subtitle_outline[font_id];
    if (!*white_font || !*outline_font) {
        *white_font = fonts->subtitle_white[NSDL_FONT_TINYTYPE];
        *outline_font = fonts->subtitle_outline[NSDL_FONT_TINYTYPE];
    }
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

static void draw_subtitle(
    SDL_Surface *screen,
    const Fonts *fonts,
    const SDL_Rect *video_rect,
    const char *text,
    bool overlay_visible,
    size_t subtitle_font_index,
    int subtitle_size,
    SubtitlePlacement placement
)
{
    char lines[MAX_SUBTITLE_LINES][MAX_SUBTITLE_LINE_LEN];
    int line_count;
    int line_index;
    int base_x;
    int base_y;
    int area_width;
    int max_width;
    nSDL_Font *white_font;
    nSDL_Font *outline_font;
    int scale_num;
    int scale_den;
    int line_height;
    int total_height;
    int area_top;
    int area_bottom;
    int area_height;
    if (!text || !*text) {
        return;
    }
    if (subtitle_size < 0) {
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
    if (placement == SUBTITLE_POS_BAR_BOTTOM || placement == SUBTITLE_POS_BAR_TOP) {
        base_x = 0;
        area_width = SCREEN_W;
    } else {
        base_x = video_rect->x;
        area_width = video_rect->w;
    }
    max_width = area_width - 12;
    if (max_width <= 0) {
        return;
    }
    line_count = wrap_subtitle(white_font, text, (max_width * scale_den) / scale_num, lines);
    if (line_count <= 0) {
        return;
    }
    total_height = line_count * line_height;
    if (line_count > 1) {
        total_height += (line_count - 1) * 2;
    }
    switch (placement) {
        case SUBTITLE_POS_BAR_BOTTOM:
            area_top = video_rect->y + video_rect->h + 2;
            area_bottom = (overlay_visible ? (SCREEN_H - UI_BAR_H) : SCREEN_H) - 2;
            area_height = area_bottom - area_top;
            if (area_height >= total_height) {
                base_y = area_top + (area_height - total_height) / 2;
            } else {
                base_y = area_bottom - total_height;
                if (base_y < 2) {
                    base_y = 2;
                }
            }
            break;
        case SUBTITLE_POS_VIDEO_TOP:
            base_y = video_rect->y + 4;
            break;
        case SUBTITLE_POS_BAR_TOP:
            area_top = 2;
            area_bottom = video_rect->y - 2;
            area_height = area_bottom - area_top;
            if (area_height >= total_height) {
                base_y = area_top + (area_height - total_height) / 2;
            } else {
                base_y = 4;
            }
            break;
        case SUBTITLE_POS_VIDEO_BOTTOM:
        default:
            base_y = (video_rect->y + video_rect->h - 8) - total_height;
            if (base_y < video_rect->y + 4) {
                base_y = video_rect->y + 4;
            }
            break;
    }
    for (line_index = 0; line_index < line_count; ++line_index) {
        int width = (nSDL_GetStringWidth(white_font, lines[line_index]) * scale_num) / scale_den;
        int x = base_x + (area_width - width) / 2;
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

static SDL_Rect progress_bar_rect(void)
{
    SDL_Rect rect = {14, SCREEN_H - 11, SCREEN_W - 28, 5};
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

static int top_overlay_y_for_rect(const SDL_Rect *video_rect, int overlay_h)
{
    if (video_rect && video_rect->y > overlay_h) {
        return (video_rect->y - overlay_h) / 2;
    }
    return 8;
}

static int draw_text_badge(SDL_Surface *screen, const Fonts *fonts, int right_x, int y, const char *label)
{
    int text_w = nSDL_GetStringWidth(fonts->white, label);
    SDL_Rect badge = {(Sint16) (right_x - text_w - 10), (Sint16) y, (Uint16) (text_w + 10), 16};
    SDL_FillRect(screen, &badge, SDL_MapRGB(screen->format, 18, 18, 24));
    nSDL_DrawString(screen, fonts->white, badge.x + 5, badge.y + 4, "%s", label);
    return badge.x - 6;
}

static int draw_left_text_badge(SDL_Surface *screen, const Fonts *fonts, int left_x, int y, const char *label)
{
    int text_w = nSDL_GetStringWidth(fonts->white, label);
    SDL_Rect badge = {(Sint16) left_x, (Sint16) y, (Uint16) (text_w + 10), 16};
    SDL_FillRect(screen, &badge, SDL_MapRGB(screen->format, 18, 18, 24));
    nSDL_DrawString(screen, fonts->white, badge.x + 5, badge.y + 4, "%s", label);
    return badge.x + badge.w + 6;
}

static int draw_status_badges(SDL_Surface *screen, const Fonts *fonts, const SDL_Rect *video_rect, ScaleMode scale_mode, const PlaybackRate *playback_rate)
{
    int right_x = SCREEN_W - 8;
    int y = top_overlay_y_for_rect(video_rect, 16);

    right_x = draw_text_badge(screen, fonts, right_x, y, playback_rate ? playback_rate->label : "1.0x");
    return draw_text_badge(screen, fonts, right_x, y, scale_mode_text(scale_mode));
}

static void draw_playback_badge(SDL_Surface *screen, const SDL_Rect *video_rect, bool paused)
{
    Uint32 background = SDL_MapRGB(screen->format, 18, 18, 24);
    Uint32 inner = SDL_MapRGB(screen->format, 8, 10, 14);
    Uint32 foreground = SDL_MapRGB(screen->format, 255, 255, 255);
    int y = top_overlay_y_for_rect(video_rect, 22);
    SDL_Rect outer = {8, (Sint16) y, 22, 22};
    SDL_Rect fill = {9, (Sint16) (y + 1), 20, 20};

    SDL_FillRect(screen, &outer, background);
    SDL_FillRect(screen, &fill, inner);
    if (paused) {
        SDL_Rect left_bar = {14, (Sint16) (y + 5), 3, 10};
        SDL_Rect right_bar = {21, (Sint16) (y + 5), 3, 10};
        SDL_FillRect(screen, &left_bar, foreground);
        SDL_FillRect(screen, &right_bar, foreground);
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
            SDL_FillRect(screen, &slice, foreground);
        }
    }
}

static void draw_memory_badge(
    SDL_Surface *screen,
    const Fonts *fonts,
    const Movie *movie,
    const SDL_Rect *video_rect,
    int right_limit,
    bool playback_badge_visible
)
{
    MemoryStats stats = query_memory_stats(movie);
    size_t ring_contig;
    size_t ring_cap;
    size_t ready_target;
    char app_text[16];
    char prefetched_text[16];
    char total_text[16];
    char free_text[16];
    char label_full[80];
    char label_medium[64];
    char label_short[48];
    char perf_full[96];
    char perf_medium[80];
    char perf_short[48];
    const char *label = NULL;
    const char *perf_label = NULL;
    int left_x;
    int y;

    if (!stats.valid) {
        return;
    }

    format_memory_compact(stats.used_bytes, app_text, sizeof(app_text));
    format_memory_compact(stats.prefetched_bytes, prefetched_text, sizeof(prefetched_text));
    format_memory_compact(stats.total_bytes, total_text, sizeof(total_text));
    format_memory_compact(stats.free_bytes, free_text, sizeof(free_text));
    ring_contig = movie && movie_uses_h264(movie) ? h264_frame_ring_contiguous_ready_count(movie) : 0U;
    ring_cap = movie && movie_uses_h264(movie) ? active_h264_frame_ring_capacity(movie) : 0U;
    ready_target = movie && movie_uses_h264(movie) ? h264_prefetch_target_ready_count(movie, 0U) : 0U;
    snprintf(label_full, sizeof(label_full), "RAM %s/%s C%s %u%% F%s", app_text, total_text, prefetched_text, stats.percent_used, free_text);
    snprintf(label_medium, sizeof(label_medium), "RAM %s/%s C%s", app_text, total_text, prefetched_text);
    snprintf(label_short, sizeof(label_short), "RAM %s/%s", app_text, total_text);
    snprintf(
        perf_full,
        sizeof(perf_full),
        "F%lu R%lu/%lu T%lu L%lu H%lu D%lu",
        movie ? (unsigned long) movie->current_frame : 0UL,
        (unsigned long) ring_contig,
        (unsigned long) ring_cap,
        (unsigned long) ready_target,
        movie ? (unsigned long) movie->diag_lag_event_count : 0UL,
        movie ? (unsigned long) movie->diag_foreground_ring_hit_count : 0UL,
        movie ? (unsigned long) movie->diag_foreground_direct_decode_count : 0UL
    );
    snprintf(
        perf_medium,
        sizeof(perf_medium),
        "F%lu R%lu/%lu L%lu H%lu D%lu",
        movie ? (unsigned long) movie->current_frame : 0UL,
        (unsigned long) ring_contig,
        (unsigned long) ring_cap,
        movie ? (unsigned long) movie->diag_lag_event_count : 0UL,
        movie ? (unsigned long) movie->diag_foreground_ring_hit_count : 0UL,
        movie ? (unsigned long) movie->diag_foreground_direct_decode_count : 0UL
    );
    snprintf(
        perf_short,
        sizeof(perf_short),
        "F%lu L%lu",
        movie ? (unsigned long) movie->current_frame : 0UL,
        movie ? (unsigned long) movie->diag_lag_event_count : 0UL
    );

    left_x = playback_badge_visible ? 36 : 8;
    y = top_overlay_y_for_rect(video_rect, 16);

    if (left_x + nSDL_GetStringWidth(fonts->white, label_full) + 10 <= right_limit) {
        label = label_full;
    } else if (left_x + nSDL_GetStringWidth(fonts->white, label_medium) + 10 <= right_limit) {
        label = label_medium;
    } else if (left_x + nSDL_GetStringWidth(fonts->white, label_short) + 10 <= right_limit) {
        label = label_short;
    }

    if (label) {
        draw_left_text_badge(screen, fonts, left_x, y, label);
    }
    if (left_x + nSDL_GetStringWidth(fonts->white, perf_full) + 10 <= right_limit) {
        perf_label = perf_full;
    } else if (left_x + nSDL_GetStringWidth(fonts->white, perf_medium) + 10 <= right_limit) {
        perf_label = perf_medium;
    } else if (left_x + nSDL_GetStringWidth(fonts->white, perf_short) + 10 <= right_limit) {
        perf_label = perf_short;
    }

    if (perf_label) {
        draw_left_text_badge(screen, fonts, left_x, y + 18, perf_label);
    }
}

static void draw_help_row(SDL_Surface *screen, const Fonts *fonts, int x, int y, int panel_w, const char *shortcut, const char *description)
{
    nSDL_DrawString(screen, fonts->white, x + 6, y, "%s", shortcut);
    nSDL_DrawString(screen, fonts->white, x + 80, y, "%s", description);
}

static void draw_help_menu(SDL_Surface *screen, const Fonts *fonts)
{
    SDL_Rect border = {15, 11, 282, 190};
    SDL_Rect panel = {16, 12, 280, 188};
    SDL_Rect header = {16, 12, 280, 24};
    SDL_Rect accent = {16, 36, 280, 2};
    const char *close_text = "CAT close";
    int y = 48;

    SDL_FillRect(screen, &border, SDL_MapRGB(screen->format, 0, 0, 0));
    SDL_FillRect(screen, &panel, SDL_MapRGB(screen->format, 8, 10, 14));
    SDL_FillRect(screen, &header, SDL_MapRGB(screen->format, 12, 18, 28));
    SDL_FillRect(screen, &accent, SDL_MapRGB(screen->format, 32, 182, 255));

    nSDL_DrawString(screen, fonts->white, panel.x + 10, panel.y + 6, "Playback Controls");
    nSDL_DrawString(screen, fonts->white, panel.x + panel.w - 10 - nSDL_GetStringWidth(fonts->white, close_text), panel.y + 6, "%s", close_text);

    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "ENTER", "Play or pause");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "CLICK", "Toggle or seek bar");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "L / R", "Seek -/+5s");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "TAB", "Step one frame");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "/", "Scale mode");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "{ / }", "Playback speed");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "^", "Subtitle position");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "+ / -", "Subtitle size");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "F", "Cycle subtitle font");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "T", "Switch subtitle track");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "M", "Memory overlay");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "S", "Save BMP screenshot");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "TOUCHPAD", "Move cursor / show UI");
    y += 11;
    draw_help_row(screen, fonts, panel.x + 10, y, panel.w - 20, "ESC", "Close menu or exit");
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
    char remaining_text[24];
    char right_text[32];
    uint32_t duration_ms = movie_duration_ms(movie);
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
    format_clock(duration_ms > current_ms ? (duration_ms - current_ms) : 0, remaining_text, sizeof(remaining_text));
    snprintf(left_text, sizeof(left_text), "%s / %s", current_text, total_text);
    snprintf(right_text, sizeof(right_text), "-%s", remaining_text);
    nSDL_DrawString(screen, fonts->white, 12, SCREEN_H - UI_BAR_H + 7, "%s", left_text);
    nSDL_DrawString(
        screen,
        fonts->white,
        SCREEN_W - 12 - nSDL_GetStringWidth(fonts->white, right_text),
        SCREEN_H - UI_BAR_H + 7,
        "%s",
        right_text
    );
}

static bool blit_rgb565_direct(SDL_Surface *screen, const Movie *movie, const SDL_Rect *dst)
{
    const uint8_t *src_row;
    uint8_t *dst_row;
    int row;
    bool locked = false;

    if (!screen || !movie || !dst || !movie->framebuffer || !screen->pixels ||
        screen->format->BitsPerPixel != 16 ||
        screen->format->Rmask != 0xF800 ||
        screen->format->Gmask != 0x07E0 ||
        screen->format->Bmask != 0x001F) {
        return false;
    }

    if (SDL_MUSTLOCK(screen)) {
        if (SDL_LockSurface(screen) != 0) {
            return false;
        }
        locked = true;
    }

    src_row = (const uint8_t *) movie->framebuffer;
    dst_row = (uint8_t *) screen->pixels + ((size_t) dst->y * screen->pitch) + ((size_t) dst->x * 2U);
    for (row = 0; row < movie->header.video_height; ++row) {
        memcpy(dst_row, src_row, (size_t) movie->header.video_width * 2U);
        src_row += (size_t) movie->header.video_width * 2U;
        dst_row += screen->pitch;
    }

    if (locked) {
        SDL_UnlockSurface(screen);
    }
    return true;
}

static void render_movie(
    SDL_Surface *screen,
    const Fonts *fonts,
    Movie *movie,
    bool paused,
    bool show_ui,
    bool help_menu_open,
    ScaleMode scale_mode,
    const PlaybackRate *playback_rate,
    MemoryOverlayMode memory_overlay_mode,
    size_t subtitle_font_index,
    bool subtitle_font_overlay_visible,
    int subtitle_size,
    SubtitlePlacement subtitle_placement,
    const char *status_overlay_text,
    const PointerState *pointer
)
{
    SDL_Rect src;
    SDL_Rect dst;
    int memory_right_limit = SCREEN_W - 8;
    uint32_t current_ms = movie_frame_time_ms(movie, movie->current_frame);
    const char *subtitle = active_subtitle(movie, current_ms);
    bool playback_badge_visible = show_ui && !help_menu_open;
    bool memory_badge_visible = !help_menu_open && (memory_overlay_mode == MEMORY_OVERLAY_ALWAYS);

    compute_video_rects(movie, scale_mode, &src, &dst);
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
    if (src.w == movie->header.video_width && src.h == movie->header.video_height &&
        dst.w == movie->header.video_width && dst.h == movie->header.video_height) {
        if (!blit_rgb565_direct(screen, movie, &dst)) {
            SDL_BlitSurface(movie->frame_surface, NULL, screen, &dst);
        }
    } else {
        SDL_SoftStretch(movie->frame_surface, &src, screen, &dst);
    }
    draw_subtitle(screen, fonts, &dst, subtitle, show_ui, subtitle_font_index, subtitle_size, subtitle_placement);
    if (subtitle_font_overlay_visible) {
        int preview_size = subtitle_size < 0 ? 0 : clamp_int(subtitle_size, 0, 1);
        draw_subtitle(
            screen,
            fonts,
            &dst,
            subtitle_font_name_for_index(subtitle_font_index),
            show_ui,
            subtitle_font_index,
            preview_size,
            subtitle_opposite_placement(subtitle_placement)
        );
    }
    if (show_ui) {
        if (!help_menu_open) {
            if (playback_badge_visible) {
                draw_playback_badge(screen, &dst, paused);
            }
            memory_right_limit = draw_status_badges(screen, fonts, &dst, scale_mode, playback_rate);
            if (status_overlay_text && status_overlay_text[0] != '\0') {
                draw_left_text_badge(screen, fonts, playback_badge_visible ? 36 : 8, top_overlay_y_for_rect(&dst, 16), status_overlay_text);
            }
        }
        draw_progress(screen, fonts, movie, current_ms, pointer);
        if (!help_menu_open && pointer && pointer->visible) {
            draw_cursor(screen, pointer->x, pointer->y);
        }
    }
    if (memory_badge_visible) {
        draw_memory_badge(screen, fonts, movie, &dst, memory_right_limit, playback_badge_visible);
    }
    if (help_menu_open) {
        draw_help_menu(screen, fonts);
    }
    SDL_Flip(screen);
}

static int picker_row_index_at(size_t count, size_t selected, int y)
{
    size_t start_index;
    size_t end_index;
    size_t index;
    int row_y = 52;

    if (count == 0) {
        return -1;
    }
    start_index = selected > (PICKER_VISIBLE_ROWS / 2) ? selected - (PICKER_VISIBLE_ROWS / 2) : 0;
    if (start_index + PICKER_VISIBLE_ROWS > count) {
        start_index = count > PICKER_VISIBLE_ROWS ? count - PICKER_VISIBLE_ROWS : 0;
    }
    end_index = start_index + PICKER_VISIBLE_ROWS;
    if (end_index > count) {
        end_index = count;
    }
    for (index = start_index; index < end_index && row_y < SCREEN_H - 20; ++index) {
        if (y >= row_y - 4 && y < row_y + 14) {
            return (int) index;
        }
        row_y += 20;
    }
    return -1;
}

static void draw_loading_overlay(SDL_Surface *screen, const Fonts *fonts, const char *label, int phase)
{
    SDL_Rect shadow = {92, 88, 136, 46};
    SDL_Rect panel = {88, 84, 136, 46};
    SDL_Rect accent = {88, 84, 136, 2};
    char spinner[8];
    int dot_count = (phase % 3) + 1;

    memset(spinner, '.', (size_t) dot_count);
    spinner[dot_count] = '\0';
    SDL_FillRect(screen, &shadow, SDL_MapRGB(screen->format, 0, 0, 0));
    SDL_FillRect(screen, &panel, SDL_MapRGB(screen->format, 10, 14, 20));
    SDL_FillRect(screen, &accent, SDL_MapRGB(screen->format, 32, 182, 255));
    nSDL_DrawString(screen, fonts->white, panel.x + 12, panel.y + 12, "%s", label ? label : "Loading");
    nSDL_DrawString(screen, fonts->white, panel.x + panel.w - 18 - nSDL_GetStringWidth(fonts->white, spinner), panel.y + 12, "%s", spinner);
}

static void render_picker(SDL_Surface *screen, const Fonts *fonts, MovieFile *files, size_t count, size_t selected, const PointerState *pointer, const char *loading_label, int loading_phase)
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
        if (pointer && pointer->visible) {
            draw_cursor(screen, pointer->x, pointer->y);
        }
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
        const char *resume_label = files[index].has_resume ? "RESUME" : NULL;
        if (index == selected) {
            SDL_Rect accent = {8, (Sint16) (y - 4), 4, 18};
            SDL_FillRect(screen, &row, SDL_MapRGB(screen->format, 26, 118, 180));
            SDL_FillRect(screen, &accent, SDL_MapRGB(screen->format, 32, 182, 255));
            nSDL_DrawString(screen, fonts->white, 24, y, "%s", files[index].name);
        } else {
            SDL_FillRect(screen, &row, SDL_MapRGB(screen->format, 16, 20, 28));
            nSDL_DrawString(screen, fonts->white, 12, y, "%s", files[index].name);
        }
        if (resume_label) {
            draw_text_badge(screen, fonts, SCREEN_W - 16, y - 3, resume_label);
        }
        y += 20;
    }
    {
        char footer[32];
        snprintf(footer, sizeof(footer), "%lu file(s)", (unsigned long) count);
        nSDL_DrawString(screen, fonts->white, SCREEN_W - 10 - nSDL_GetStringWidth(fonts->white, footer), SCREEN_H - 16, "%s", footer);
    }
    if (loading_label) {
        draw_loading_overlay(screen, fonts, loading_label, loading_phase);
    }
    if (pointer && pointer->visible) {
        draw_cursor(screen, pointer->x, pointer->y);
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

static void history_path_for_directory(const char *directory, char *history_path, size_t history_path_size)
{
    if (!history_path || history_path_size == 0) {
        return;
    }
    snprintf(history_path, history_path_size, "%s/%s", directory && directory[0] != '\0' ? directory : ".", HISTORY_FILE_NAME);
}

static void history_path_for_movie(const char *movie_path, char *history_path, size_t history_path_size)
{
    char directory[MAX_PATH_LEN];

    if (!history_path || history_path_size == 0) {
        return;
    }
    if (movie_path && movie_path[0] != '\0') {
        snprintf(directory, sizeof(directory), "%s", movie_path);
        strip_filename(directory);
    } else {
        snprintf(directory, sizeof(directory), ".");
    }
    history_path_for_directory(directory, history_path, history_path_size);
}

static bool load_history_store_from_path(const char *history_path, HistoryStore *history)
{
    FILE *file;
    char line[MAX_PATH_LEN + 64];

    memset(history, 0, sizeof(*history));
    file = fopen(history_path, "rb");
    if (!file) {
        return true;
    }
    if (!fgets(line, sizeof(line), file) || strncmp(line, "NDVH1", 5) != 0) {
        fclose(file);
        return false;
    }
    while (history->count < HISTORY_MAX_ENTRIES && fgets(line, sizeof(line), file)) {
        char *separator = strchr(line, '\t');
        char *path;
        if (!separator) {
            continue;
        }
        *separator = '\0';
        path = separator + 1;
        path[strcspn(path, "\r\n")] = '\0';
        if (path[0] == '\0') {
            continue;
        }
        history->entries[history->count].frame = (uint32_t) strtoul(line, NULL, 10);
        history->entries[history->count].path = dup_string(path);
        if (!history->entries[history->count].path) {
            fclose(file);
            free_history_store(history);
            return false;
        }
        history->count++;
    }
    fclose(file);
    return true;
}

static bool load_history_store(const char *movie_path, HistoryStore *history)
{
    char history_path[MAX_PATH_LEN];
    history_path_for_movie(movie_path, history_path, sizeof(history_path));
    return load_history_store_from_path(history_path, history);
}

static bool save_history_store(const char *movie_path, const HistoryStore *history)
{
    char history_path[MAX_PATH_LEN];
    FILE *file;
    size_t index;

    history_path_for_movie(movie_path, history_path, sizeof(history_path));
    file = fopen(history_path, "wb");
    if (!file) {
        return false;
    }
    fputs("NDVH1\n", file);
    for (index = 0; index < history->count && index < HISTORY_MAX_ENTRIES; ++index) {
        fprintf(file, "%lu\t%s\n", (unsigned long) history->entries[index].frame, history->entries[index].path);
    }
    fclose(file);
    return true;
}

static int history_find_entry_index(const HistoryStore *history, const char *movie_path)
{
    size_t index;
    for (index = 0; index < history->count; ++index) {
        if (history->entries[index].path && strcmp(history->entries[index].path, movie_path) == 0) {
            return (int) index;
        }
    }
    return -1;
}

static void history_remove_entry(HistoryStore *history, const char *movie_path)
{
    int found_index = history_find_entry_index(history, movie_path);
    size_t index;
    if (found_index < 0) {
        return;
    }
    free(history->entries[found_index].path);
    for (index = (size_t) found_index; index + 1 < history->count; ++index) {
        history->entries[index] = history->entries[index + 1];
    }
    memset(&history->entries[history->count - 1], 0, sizeof(history->entries[history->count - 1]));
    history->count--;
}

static void history_upsert_entry(HistoryStore *history, const char *movie_path, uint32_t frame)
{
    HistoryEntry entry;
    size_t index;

    history_remove_entry(history, movie_path);
    entry.path = dup_string(movie_path);
    entry.frame = frame;
    if (!entry.path) {
        return;
    }
    if (history->count == HISTORY_MAX_ENTRIES) {
        free(history->entries[HISTORY_MAX_ENTRIES - 1].path);
        history->count--;
    }
    for (index = history->count; index > 0; --index) {
        history->entries[index] = history->entries[index - 1];
    }
    history->entries[0] = entry;
    history->count++;
}

static bool history_resume_frame_for_movie(const char *movie_path, uint32_t *out_frame)
{
    HistoryStore history;
    int found_index;
    if (!load_history_store(movie_path, &history)) {
        return false;
    }
    found_index = history_find_entry_index(&history, movie_path);
    if (found_index >= 0) {
        *out_frame = history.entries[found_index].frame;
        free_history_store(&history);
        return true;
    }
    free_history_store(&history);
    return false;
}

static bool should_save_history_position(const Movie *movie)
{
    uint32_t now_ms = movie_frame_time_ms(movie, movie->current_frame);
    uint32_t duration_ms = movie_duration_ms(movie);
    return now_ms >= RESUME_MIN_MS && (duration_ms == 0 || now_ms + RESUME_CLEAR_TAIL_MS < duration_ms);
}

static void save_history_position_for_movie(const char *movie_path, const Movie *movie)
{
    HistoryStore history;
    if (!load_history_store(movie_path, &history)) {
        return;
    }
    if (should_save_history_position(movie)) {
        history_upsert_entry(&history, movie_path, movie->current_frame);
    } else {
        history_remove_entry(&history, movie_path);
    }
    save_history_store(movie_path, &history);
    free_history_store(&history);
}

static void maybe_defer_history_save(const Movie *movie, uint32_t *last_history_saved_ms, bool *pending)
{
    uint32_t history_ms;

    if (!movie || !last_history_saved_ms || !pending) {
        return;
    }

    history_ms = movie_frame_time_ms(movie, movie->current_frame);
    if (history_ms + 1000U < *last_history_saved_ms || history_ms >= *last_history_saved_ms + 15000U) {
        if (!*pending) {
            debug_tracef(
                "history defer frame=%lu time_ms=%lu",
                (unsigned long) movie->current_frame,
                (unsigned long) history_ms
            );
        }
        *pending = true;
        *last_history_saved_ms = history_ms;
    }
}

static void flush_deferred_history_save(const char *movie_path, const Movie *movie, bool *pending, const char *reason, bool force)
{
    uint32_t start_ms;
    uint32_t elapsed_ms;

    if (!movie_path || !movie || !pending) {
        return;
    }
    if (!force && !*pending) {
        return;
    }

    start_ms = monotonic_clock_now_ms();
    save_history_position_for_movie(movie_path, movie);
    elapsed_ms = monotonic_clock_now_ms() - start_ms;
    debug_tracef(
        "history flush reason=%s frame=%lu ms=%lu",
        reason ? reason : "unknown",
        (unsigned long) movie->current_frame,
        (unsigned long) elapsed_ms
    );
    *pending = false;
}

static bool save_screenshot_bitmap(SDL_Surface *screen, const char *movie_path, char *saved_path, size_t saved_path_size)
{
    char directory[MAX_PATH_LEN];
    int index;

    if (saved_path && saved_path_size > 0) {
        saved_path[0] = '\0';
    }
    if (!screen) {
        return false;
    }

    if (movie_path && movie_path[0] != '\0') {
        snprintf(directory, sizeof(directory), "%s", movie_path);
        strip_filename(directory);
    } else {
        snprintf(directory, sizeof(directory), ".");
    }

    for (index = 1; index <= 9999; ++index) {
        char candidate[MAX_PATH_LEN];
        FILE *existing;
        int candidate_len = snprintf(candidate, sizeof(candidate), "%s/ndvideo-shot-%04d.bmp", directory, index);

        if (candidate_len < 0 || (size_t) candidate_len >= sizeof(candidate)) {
            return false;
        }
        existing = fopen(candidate, "rb");
        if (existing) {
            fclose(existing);
            continue;
        }
        if (SDL_SaveBMP(screen, candidate) == 0) {
            if (saved_path && saved_path_size > 0) {
                snprintf(saved_path, saved_path_size, "%s", candidate);
            }
            debug_tracef("screenshot saved path=%s", candidate);
            return true;
        }
        debug_tracef("screenshot save fail path=%s", candidate);
        return false;
    }

    return false;
}

static int pick_movie(SDL_Surface *screen, const Fonts *fonts, const char *directory, char *selected_path, size_t selected_size)
{
    bool prev_up = false;
    bool prev_down = false;
    bool prev_enter = false;
    bool prev_esc = false;
    PointerState pointer;
    MovieFile *files;
    size_t count = 0;
    size_t selected = 0;

    files = scan_movies(directory, &count);
    pointer_init(&pointer);
    while (1) {
        bool pointer_click = pointer_update(&pointer);
        int hovered_index = pointer.visible ? picker_row_index_at(count, selected, pointer.y) : -1;

        if (hovered_index >= 0) {
            selected = (size_t) hovered_index;
        }
        render_picker(screen, fonts, files, count, selected, &pointer, NULL, 0);
        if (key_pressed_edge(KEY_NSPIRE_UP, &prev_up) && selected > 0) {
            selected--;
        }
        if (key_pressed_edge(KEY_NSPIRE_DOWN, &prev_down) && selected + 1 < count) {
            selected++;
        }
        if (pointer_click && hovered_index >= 0 && (size_t) hovered_index < count) {
            int phase;
            for (phase = 0; phase < 6; ++phase) {
                render_picker(screen, fonts, files, count, selected, &pointer, "Loading", phase);
                msleep(30);
            }
            strncpy(selected_path, files[hovered_index].path, selected_size - 1);
            selected_path[selected_size - 1] = '\0';
            free_movie_files(files, count);
            return 0;
        }
        if (key_pressed_edge(KEY_NSPIRE_ENTER, &prev_enter) && count > 0) {
            int phase;
            for (phase = 0; phase < 6; ++phase) {
                render_picker(screen, fonts, files, count, selected, &pointer, "Loading", phase);
                msleep(30);
            }
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

static void draw_prompt_button(SDL_Surface *screen, const Fonts *fonts, const SDL_Rect *button, const char *label, bool selected)
{
    Uint32 background = selected
        ? SDL_MapRGB(screen->format, 26, 118, 180)
        : SDL_MapRGB(screen->format, 18, 24, 34);
    Uint32 accent = selected
        ? SDL_MapRGB(screen->format, 32, 182, 255)
        : SDL_MapRGB(screen->format, 52, 62, 78);
    SDL_Rect left_accent = {button->x, button->y, 4, button->h};

    SDL_FillRect(screen, (SDL_Rect *) button, background);
    SDL_FillRect(screen, &left_accent, accent);
    nSDL_DrawString(
        screen,
        fonts->white,
        button->x + (button->w - nSDL_GetStringWidth(fonts->white, label)) / 2,
        button->y + 6,
        "%s",
        label
    );
}

static int prompt_resume_position(
    SDL_Surface *screen,
    const Fonts *fonts,
    Movie *movie,
    const char *path,
    uint32_t resume_frame
)
{
    bool prev_left = false;
    bool prev_right = false;
    bool prev_enter = false;
    bool prev_esc = false;
    bool prev_click = false;
    PointerState pointer;
    SDL_Rect shadow = {34, 24, 252, 192};
    SDL_Rect panel = {28, 18, 252, 192};
    SDL_Rect header = {28, 18, 252, 26};
    SDL_Rect accent = {28, 44, 252, 2};
    SDL_Rect preview = {74, 56, 160, 90};
    SDL_Rect preview_border = {72, 54, 164, 94};
    SDL_Rect continue_button = {46, 162, 90, 22};
    SDL_Rect restart_button = {172, 162, 90, 22};
    char time_label[64];
    char time_value[24];
    char title[96];
    const char *filename = path;
    char *display_name = NULL;
    int selected_button = 0;
    uint32_t preview_ms;

    if (resume_frame >= movie->header.frame_count) {
        resume_frame = movie->header.frame_count ? (movie->header.frame_count - 1) : 0;
    }
    if (!decode_to_frame(movie, resume_frame)) {
        return 0;
    }
    preview_ms = movie_frame_time_ms(movie, movie->current_frame);
    format_clock(preview_ms, time_value, sizeof(time_value));
    snprintf(time_label, sizeof(time_label), "Resume from %s", time_value);
    if (strrchr(path, '/')) {
        filename = strrchr(path, '/') + 1;
    } else if (strrchr(path, '\\')) {
        filename = strrchr(path, '\\') + 1;
    }
    display_name = display_name_for_movie(filename);
    snprintf(title, sizeof(title), "%s", display_name ? display_name : filename);
    pointer_init(&pointer);

    while (1) {
        bool pointer_click = pointer_update(&pointer);

        SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
        SDL_FillRect(screen, &shadow, SDL_MapRGB(screen->format, 0, 0, 0));
        SDL_FillRect(screen, &panel, SDL_MapRGB(screen->format, 8, 10, 14));
        SDL_FillRect(screen, &header, SDL_MapRGB(screen->format, 12, 18, 28));
        SDL_FillRect(screen, &accent, SDL_MapRGB(screen->format, 32, 182, 255));
        SDL_FillRect(screen, &preview_border, SDL_MapRGB(screen->format, 0, 0, 0));
        SDL_SoftStretch(movie->frame_surface, NULL, screen, &preview);
        nSDL_DrawString(screen, fonts->white, panel.x + 12, panel.y + 8, "Continue Watching?");
        nSDL_DrawString(screen, fonts->white, panel.x + 12, panel.y + 28, "%s", title);
        nSDL_DrawString(screen, fonts->white, panel.x + 12, panel.y + 44, "%s", time_label);
        draw_prompt_button(screen, fonts, &continue_button, "CONTINUE", selected_button == 0);
        draw_prompt_button(screen, fonts, &restart_button, "START OVER", selected_button == 1);
        if (pointer.visible) {
            draw_cursor(screen, pointer.x, pointer.y);
        }
        SDL_Flip(screen);

        if (pointer.visible) {
            if (pointer.x >= continue_button.x && pointer.x < continue_button.x + continue_button.w &&
                pointer.y >= continue_button.y && pointer.y < continue_button.y + continue_button.h) {
                selected_button = 0;
            } else if (pointer.x >= restart_button.x && pointer.x < restart_button.x + restart_button.w &&
                pointer.y >= restart_button.y && pointer.y < restart_button.y + restart_button.h) {
                selected_button = 1;
            }
        }
        if (key_pressed_edge(KEY_NSPIRE_LEFT, &prev_left) || key_pressed_edge(KEY_NSPIRE_RIGHT, &prev_right)) {
            selected_button = 1 - selected_button;
        }
        if (key_pressed_edge(KEY_NSPIRE_CLICK, &prev_click)) {
            pointer_click = true;
        }
        if (pointer_click) {
            if (pointer.x >= continue_button.x && pointer.x < continue_button.x + continue_button.w &&
                pointer.y >= continue_button.y && pointer.y < continue_button.y + continue_button.h) {
                free(display_name);
                return 1;
            }
            if (pointer.x >= restart_button.x && pointer.x < restart_button.x + restart_button.w &&
                pointer.y >= restart_button.y && pointer.y < restart_button.y + restart_button.h) {
                free(display_name);
                return 0;
            }
        }
        if (key_pressed_edge(KEY_NSPIRE_ENTER, &prev_enter)) {
            free(display_name);
            return selected_button == 0 ? 1 : 0;
        }
        if (key_pressed_edge(KEY_NSPIRE_ESC, &prev_esc)) {
            free(display_name);
            return -1;
        }
        msleep(16);
    }
}

static int play_movie(SDL_Surface *screen, const Fonts *fonts, const char *path)
{
    static Movie movie;
    bool prev_enter = false;
    bool prev_left = false;
    bool prev_right = false;
    bool prev_tab = false;
    bool prev_esc = false;
    bool prev_click = false;
    bool prev_cat = false;
    bool prev_divide = false;
    bool prev_exp = false;
    bool prev_tenx = false;
    bool prev_lp = false;
    bool prev_rp = false;
    bool prev_lthan = false;
    bool prev_gthan = false;
    bool prev_f = false;
    bool prev_t = false;
    bool prev_m = false;
    bool prev_s = false;
    bool prev_plus = false;
    bool prev_minus = false;
    bool paused = false;
    uint64_t frame_interval_ticks;
    uint64_t next_frame_due_ticks;
    uint64_t playback_anchor_ticks;
    uint32_t playback_anchor_frame;
    uint32_t ui_visible_until;
    uint32_t subtitle_font_overlay_until = 0;
    int result = 0;
    ScaleMode scale_mode = SCALE_FIT;
    size_t playback_rate_index = PLAYBACK_RATE_DEFAULT_INDEX;
    size_t subtitle_font_index = SUBTITLE_FONT_DEFAULT_INDEX;
    MemoryOverlayMode memory_overlay_mode = MEMORY_OVERLAY_OFF;
    int subtitle_size = 0;
    SubtitlePlacement subtitle_placement = SUBTITLE_POS_BAR_BOTTOM;
    char status_overlay_text[64] = {0};
    uint32_t status_overlay_until = 0;
    PointerState pointer;
    bool help_menu_open = false;
    bool help_resume_playback = false;
    uint32_t resume_frame = 0;
    uint32_t last_history_saved_ms = 0;
    bool history_save_pending = false;

    g_debug_ring_count = 0;
    g_debug_ring_next = 0;
    debug_clear_last_error();
    {
        int phase;
        for (phase = 0; phase < 4; ++phase) {
            SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 8, 10, 14));
            draw_loading_overlay(screen, fonts, "Loading", phase);
            SDL_Flip(screen);
            msleep(24);
        }
    }
    if (!load_movie(path, &movie)) {
        report_movie_open_failure(path);
        return -1;
    }
    debug_tracef(
        "play start path=%s frames=%lu chunks=%lu",
        path,
        (unsigned long) movie.header.frame_count,
        (unsigned long) movie.header.chunk_count
    );
    if (history_resume_frame_for_movie(path, &resume_frame) && resume_frame < movie.header.frame_count) {
        int resume_choice = prompt_resume_position(screen, fonts, &movie, path, resume_frame);
        if (resume_choice < 0) {
            char log_path[MAX_PATH_LEN];
            debug_log_path_for_movie(path, log_path, sizeof(log_path));
            debug_dump_session(log_path, &movie, "resume-cancel");
            destroy_movie(&movie);
            return 0;
        }
        if (resume_choice == 0) {
            decode_to_frame(&movie, 0);
        } else {
            snprintf(status_overlay_text, sizeof(status_overlay_text), "RESUMED");
            status_overlay_until = monotonic_clock_now_ms() + STATUS_OVERLAY_MS;
        }
    }
    pointer_init(&pointer);
    prev_tab = isKeyPressed(KEY_NSPIRE_TAB);
    prev_t = isKeyPressed(KEY_NSPIRE_T);
    prev_m = isKeyPressed(KEY_NSPIRE_M);
    prev_s = isKeyPressed(KEY_NSPIRE_S);
    frame_interval_ticks = movie_frame_interval_ticks(&movie);
    playback_anchor_ticks = monotonic_clock_now_ticks();
    playback_anchor_frame = movie.current_frame;
    next_frame_due_ticks = playback_anchor_ticks + movie_frame_time_scaled_ticks(&movie, 1, playback_rate_for_index(playback_rate_index));
    ui_visible_until = monotonic_clock_ticks_to_ms(playback_anchor_ticks) + POINTER_UI_TIMEOUT_MS;
    last_history_saved_ms = movie_frame_time_ms(&movie, movie.current_frame);
    {
        prefetch_tick(&movie, true, 1000);
    }

    while (1) {
        bool pointer_click = pointer_update(&pointer);
        uint64_t now_ticks = monotonic_clock_now_ticks();
        uint32_t now_ms = monotonic_clock_ticks_to_ms(now_ticks);
        const PlaybackRate *playback_rate = playback_rate_for_index(playback_rate_index);
        bool show_ui = help_menu_open || paused || (now_ms <= ui_visible_until);
        bool enter_edge = key_pressed_edge(KEY_NSPIRE_ENTER, &prev_enter);
        bool tab_edge = key_pressed_edge(KEY_NSPIRE_TAB, &prev_tab);
        bool cat_edge = key_pressed_edge(KEY_NSPIRE_CAT, &prev_cat);
        bool divide_edge = key_pressed_edge(KEY_NSPIRE_DIVIDE, &prev_divide);
        bool exp_edge = key_pressed_edge(KEY_NSPIRE_EXP, &prev_exp);
        bool tenx_edge = key_pressed_edge(KEY_NSPIRE_TENX, &prev_tenx);
        bool lp_edge = key_pressed_edge(KEY_NSPIRE_LP, &prev_lp);
        bool rp_edge = key_pressed_edge(KEY_NSPIRE_RP, &prev_rp);
        bool lthan_edge = key_pressed_edge(KEY_NSPIRE_LTHAN, &prev_lthan);
        bool gthan_edge = key_pressed_edge(KEY_NSPIRE_GTHAN, &prev_gthan);
        bool speed_down_edge = lp_edge || lthan_edge;
        bool speed_up_edge = rp_edge || gthan_edge;
        bool subtitle_font_edge = key_pressed_edge(KEY_NSPIRE_F, &prev_f);
        bool subtitle_track_edge = key_pressed_edge(KEY_NSPIRE_T, &prev_t);
        bool memory_overlay_edge = key_pressed_edge(KEY_NSPIRE_M, &prev_m);
        bool screenshot_edge = key_pressed_edge(KEY_NSPIRE_S, &prev_s);
        bool click_edge = key_pressed_edge(KEY_NSPIRE_CLICK, &prev_click);
        bool take_screenshot = false;

        if (pointer.moved || pointer_click) {
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            show_ui = true;
        }
        if (click_edge) {
            pointer_click = true;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            show_ui = true;
        }
        if (screenshot_edge) {
            take_screenshot = true;
        }
        if (cat_edge) {
            if (help_menu_open) {
                help_menu_open = false;
                if (help_resume_playback) {
                    paused = false;
                }
                help_resume_playback = false;
            } else {
                help_menu_open = true;
                help_resume_playback = !paused;
                paused = true;
            }
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            show_ui = true;
        }
        if (key_pressed_edge(KEY_NSPIRE_ESC, &prev_esc)) {
            if (help_menu_open) {
                help_menu_open = false;
                if (help_resume_playback) {
                    paused = false;
                }
                help_resume_playback = false;
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
                show_ui = true;
            } else {
                break;
            }
        }
        if (help_menu_open) {
            render_movie(
                screen,
                fonts,
                &movie,
                paused,
                true,
                true,
                scale_mode,
                playback_rate,
                memory_overlay_mode,
                subtitle_font_index,
                false,
                subtitle_size,
                subtitle_placement,
                (now_ms <= status_overlay_until) ? status_overlay_text : NULL,
                &pointer
            );
            if (take_screenshot) {
                save_screenshot_bitmap(screen, path, NULL, 0);
            }
            flush_deferred_history_save(path, &movie, &history_save_pending, "help", false);
            prefetch_tick(&movie, true, 1000);
            msleep(16);
            continue;
        }
        if (enter_edge) {
            if (movie.current_frame + 1 >= movie.header.frame_count) {
                decode_to_frame(&movie, 0);
                paused = false;
            } else {
                paused = !paused;
            }
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (key_pressed_edge(KEY_NSPIRE_LEFT, &prev_left)) {
            clamp_seek(&movie, -SEEK_STEP_MS);
            paused = true;
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (key_pressed_edge(KEY_NSPIRE_RIGHT, &prev_right)) {
            clamp_seek(&movie, SEEK_STEP_MS);
            paused = true;
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (divide_edge) {
            scale_mode = (ScaleMode) ((scale_mode + 1) % 4);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (speed_down_edge && playback_rate_index > 0) {
            playback_rate_index--;
            playback_rate = playback_rate_for_index(playback_rate_index);
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (speed_up_edge && playback_rate_index + 1 < PLAYBACK_RATE_COUNT) {
            playback_rate_index++;
            playback_rate = playback_rate_for_index(playback_rate_index);
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (exp_edge || tenx_edge) {
            subtitle_placement = (SubtitlePlacement) ((subtitle_placement + 1) % SUBTITLE_POS_COUNT);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (key_pressed_edge(KEY_NSPIRE_PLUS, &prev_plus) && subtitle_size < 3) {
            subtitle_size++;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (key_pressed_edge(KEY_NSPIRE_MINUS, &prev_minus) && subtitle_size > -1) {
            subtitle_size--;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (subtitle_font_edge) {
            subtitle_font_index = (subtitle_font_index + 1) % SUBTITLE_FONT_CHOICE_COUNT;
            subtitle_font_overlay_until = now_ms + SUBTITLE_FONT_OVERLAY_MS;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (subtitle_track_edge) {
            if (movie.subtitle_track_count > 1) {
                movie.selected_subtitle_track = (uint16_t) ((movie.selected_subtitle_track + 1) % movie.subtitle_track_count);
            } else {
                movie.selected_subtitle_track = 0;
            }
            snprintf(
                status_overlay_text,
                sizeof(status_overlay_text),
                "SUB %.22s",
                active_subtitle_track_name(&movie)
            );
            status_overlay_until = now_ms + STATUS_OVERLAY_MS;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (memory_overlay_edge) {
            memory_overlay_mode = (memory_overlay_mode == MEMORY_OVERLAY_OFF)
                ? MEMORY_OVERLAY_ALWAYS
                : MEMORY_OVERLAY_OFF;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (tab_edge) {
            if (!paused) {
                paused = true;
            } else if (movie.current_frame + 1 < movie.header.frame_count) {
                if (!decode_to_frame(&movie, movie.current_frame + 1)) {
                    report_movie_decode_failure(&movie, path, "tab step");
                    result = -1;
                    break;
                }
            }
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (pointer_click) {
            SDL_Rect bar = progress_bar_rect();
            if (show_ui && pointer.y >= SCREEN_H - UI_BAR_H && pointer.y < SCREEN_H) {
                int seek_x = clamp_int(pointer.x, bar.x, bar.x + bar.w);
                seek_to_ratio(&movie, (uint32_t) (seek_x - bar.x), (uint32_t) bar.w);
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            } else {
                if (movie.current_frame + 1 >= movie.header.frame_count) {
                    decode_to_frame(&movie, 0);
                    paused = false;
                } else {
                    paused = !paused;
                }
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            }
        }
        maybe_defer_history_save(&movie, &last_history_saved_ms, &history_save_pending);
        if (!paused && frame_interval_ticks > 0) {
            if (now_ticks >= next_frame_due_ticks) {
                uint64_t elapsed_ticks = now_ticks - playback_anchor_ticks;
                uint32_t frames_to_advance = movie_frames_from_scaled_ticks(&movie, elapsed_ticks, playback_rate);
                uint32_t target_frame = playback_anchor_frame + frames_to_advance;
                bool lagged = false;
                uint32_t lag_frames = 0;
                uint32_t late_ms = 0;

                if (target_frame > movie.current_frame + 1U) {
                    lag_frames = target_frame - (movie.current_frame + 1U);
                    target_frame = movie.current_frame + 1U;
                    lagged = true;
                }
                if (lagged) {
                    late_ms = monotonic_clock_ticks_to_ms(now_ticks - next_frame_due_ticks);
                    movie.diag_lag_event_count++;
                    movie.diag_lag_frame_total += lag_frames;
                    if (lag_frames > movie.diag_max_lag_frames) {
                        movie.diag_max_lag_frames = lag_frames;
                    }
                    if (late_ms > movie.diag_max_late_ms) {
                        movie.diag_max_late_ms = late_ms;
                    }
                    debug_tracef(
                        "lag frame=%lu target=%lu late_ms=%lu lag_frames=%lu spare_prev=%lu",
                        (unsigned long) movie.current_frame,
                        (unsigned long) target_frame,
                        (unsigned long) late_ms,
                        (unsigned long) lag_frames,
                        (unsigned long) movie.diag_last_spare_ms
                    );
                }

                if (target_frame >= movie.header.frame_count) {
                    if (movie.current_frame + 1 < movie.header.frame_count) {
                        if (!decode_to_frame(&movie, movie.header.frame_count - 1)) {
                            report_movie_decode_failure(&movie, path, "final frame");
                            result = -1;
                            break;
                        }
                    }
                    paused = true;
                    ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
                } else {
                    if (target_frame > movie.current_frame) {
                        uint32_t decode_start_ms = monotonic_clock_now_ms();
                        uint32_t decode_elapsed_ms;

                        if (!decode_to_frame(&movie, target_frame)) {
                            report_movie_decode_failure(&movie, path, "playback advance");
                            result = -1;
                            break;
                        }
                        decode_elapsed_ms = monotonic_clock_now_ms() - decode_start_ms;
                        movie.diag_foreground_decode_count++;
                        record_h264_foreground_decode_time(&movie, decode_elapsed_ms);
                        if (decode_elapsed_ms >= DEBUG_TRACE_FOREGROUND_MS || lagged) {
                            debug_tracef(
                                "fg frame=%lu ms=%lu lagged=%u chunk=%d contig=%lu ringhits=%lu direct=%lu",
                                (unsigned long) target_frame,
                                (unsigned long) decode_elapsed_ms,
                                lagged ? 1U : 0U,
                                movie.loaded_chunk,
                                (unsigned long) h264_frame_ring_contiguous_ready_count(&movie),
                                (unsigned long) movie.diag_foreground_ring_hit_count,
                                (unsigned long) movie.diag_foreground_direct_decode_count
                            );
                        }
                        if (lagged || decode_elapsed_ms >= H264_FOREGROUND_DECODE_HARD_MS) {
                            movie.h264_active_prefetch_backoff = H264_ACTIVE_PREFETCH_BACKOFF_HEAVY;
                        } else if (decode_elapsed_ms >= H264_FOREGROUND_DECODE_SOFT_MS) {
                            if (movie.h264_active_prefetch_backoff < H264_ACTIVE_PREFETCH_BACKOFF_LIGHT) {
                                movie.h264_active_prefetch_backoff = H264_ACTIVE_PREFETCH_BACKOFF_LIGHT;
                            }
                        }
                    }
                    if (lagged) {
                        reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                    } else {
                        next_frame_due_ticks = playback_anchor_ticks + movie_frame_time_scaled_ticks(&movie, (target_frame - playback_anchor_frame) + 1, playback_rate);
                    }
                }
            }
        }
        show_ui = paused || (monotonic_clock_now_ms() <= ui_visible_until);
        render_movie(
            screen,
            fonts,
            &movie,
            paused,
            show_ui,
            false,
            scale_mode,
            playback_rate,
            memory_overlay_mode,
            subtitle_font_index,
            now_ms <= subtitle_font_overlay_until,
            subtitle_size,
            subtitle_placement,
            (now_ms <= status_overlay_until) ? status_overlay_text : NULL,
            &pointer
        );
        if (take_screenshot) {
            save_screenshot_bitmap(screen, path, NULL, 0);
        }
        if (paused || frame_interval_ticks == 0) {
            flush_deferred_history_save(path, &movie, &history_save_pending, "paused", false);
            prefetch_tick(&movie, true, 1000);
            if (now_ms - movie.diag_last_snapshot_ms >= DEBUG_SNAPSHOT_INTERVAL_MS) {
                movie.diag_last_snapshot_ms = now_ms;
                debug_trace_runtime_snapshot(&movie, true, 1000U, playback_rate, "paused");
            }
            msleep(16);
        } else {
            uint64_t after_render_ticks = monotonic_clock_now_ticks();
            uint64_t spare_ticks = next_frame_due_ticks > after_render_ticks ? (next_frame_due_ticks - after_render_ticks) : 0;
            uint32_t spare_ms = monotonic_clock_ticks_to_ms(spare_ticks);
            uint64_t wait_target_ticks = next_frame_due_ticks;
            if (spare_ms > movie.diag_max_spare_ms) {
                movie.diag_max_spare_ms = spare_ms;
            }
            movie.diag_last_spare_ms = spare_ms;
            prefetch_tick(&movie, false, spare_ms);
            if (now_ms - movie.diag_last_snapshot_ms >= DEBUG_SNAPSHOT_INTERVAL_MS) {
                movie.diag_last_snapshot_ms = now_ms;
                debug_trace_runtime_snapshot(&movie, false, spare_ms, playback_rate, "play");
            }
            after_render_ticks = monotonic_clock_now_ticks();
            spare_ticks = next_frame_due_ticks > after_render_ticks ? (next_frame_due_ticks - after_render_ticks) : 0;
            if (spare_ticks > 0) {
                uint64_t max_wait_ticks = (((uint64_t) monotonic_clock_ticks_per_second()) * 8U) / 1000U;
                if (spare_ticks > max_wait_ticks) {
                    wait_target_ticks = after_render_ticks + max_wait_ticks;
                }
                wait_until_ticks_precise(wait_target_ticks);
            }
        }
    }

    flush_deferred_history_save(path, &movie, &history_save_pending, "exit", true);
    {
        char log_path[MAX_PATH_LEN];
        debug_log_path_for_movie(path, log_path, sizeof(log_path));
        debug_dump_session(log_path, &movie, result == 0 ? "normal-exit" : "aborted");
    }
    destroy_movie(&movie);
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
