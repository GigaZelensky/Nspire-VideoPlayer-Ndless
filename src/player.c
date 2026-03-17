#define OLD_SCREEN_API
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
#include "h264bsd_decoder.h"
#include "h264bsd_sram.h"
#include "h264bsd_util.h"
#include "sram.h"

extern void FastMemcpy(void* dest, const void* src, size_t chunks_32byte);

#define SCREEN_W 320
#define SCREEN_H 240
#define UI_BAR_H 28
#define SEEK_STEP_MS 5000
#define SEEK_STACK_DELAY_MS 450U
#define TAB_HOLD_FRAME_REPEAT_DELAY_MS 250U
#define TAB_HOLD_FRAME_REPEAT_FALLBACK_INTERVAL_MS 80U
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
#define POINTER_GAIN_NUM 3
#define POINTER_GAIN_DEN 4
#define POINTER_JITTER_THRESHOLD 2
#define POINTER_DECISIVE_SUM_THRESHOLD 5
#define POINTER_AXIS_LOCK_RATIO_NUM 2
#define POINTER_AXIS_LOCK_RATIO_DEN 1
#define POINTER_SPIKE_DELTA_DIVISOR 3
#define PREFETCH_FILE_BLOCK_SIZE 32768U
#define PREFETCH_ACTIVE_FILE_BLOCK_SIZE 2048U
#define PREFETCH_INFLATE_OUTPUT_SLICE 2048U
#define PREFETCH_PAUSED_SLICE_MS 12U
#define PREFETCH_ACTIVE_H264_MIN_SPARE_MS 12U
#define PREFETCH_ACTIVE_H264_SLICE_MS 8U
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
#define H264_PREFETCH_NEXT_CHUNK_IO_CATCHUP_FRAMES 20U
#define H264_PREFETCH_SECOND_NEXT_CHUNK_WINDOW_FRAMES 32U
#define H264_PREFETCH_DECODE_GUARD_DEFAULT_MS 16U
#define H264_PREFETCH_DECODE_GUARD_MIN_MS 10U
#define H264_PREFETCH_DECODE_GUARD_MAX_MS 22U
#define H264_FOREGROUND_DECODE_SOFT_MS 35U
#define H264_FOREGROUND_DECODE_HARD_MS 50U
#define H264_ACTIVE_PREFETCH_BACKOFF_LIGHT 0U
#define H264_ACTIVE_PREFETCH_BACKOFF_HEAVY 1U
#define H264_SAME_CHUNK_RESCUE_MIN_SPARE_MS 10U
#define H264_NEXT_CHUNK_IDR_MIN_SPARE_MS 4U
#define H264_NEXT_CHUNK_IDR_BUDGET_GUARD_MS 3U
#define H264_NEXT_CHUNK_IDR_DEFAULT_MBS_PER_MS_Q8 640U
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
#define HISTORY_MAGIC_V1 "NDVH1"
#define HISTORY_MAGIC_V2 "NDVH2"
#define RESUME_MIN_MS 5000U
#define RESUME_CLEAR_TAIL_MS 3000U
#define STATUS_OVERLAY_MS 1200U
#define SCREENSHOT_PREVIEW_MS 1200U
#define SEEK_PREROLL_TIMEOUT_MS 120U
#define SEEK_PREROLL_TARGET_LOW_FRAMES 3U
#define SEEK_PREROLL_TARGET_HIGH_FRAMES 6U
#define SEEK_BAR_PREVIEW_DEBOUNCE_MS 250U
#define SCREENSHOT_PREVIEW_MAX_W 96
#define SCREENSHOT_PREVIEW_MAX_H 72
#define SEEK_BAR_PREVIEW_MAX_W 80
#define SEEK_BAR_PREVIEW_MAX_H 60
#define MOVIE_VERSION_H264 9
#define MOVIE_VERSION_POSITIONED_SUBS 10
#define SUBTITLE_COORD_SCALE 10000U
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
    uint32_t start_ms;
    uint32_t end_ms;
    char *text;
    uint8_t position_mode;
    uint8_t align;
    uint16_t pos_x;
    uint16_t pos_y;
    uint16_t margin_l;
    uint16_t margin_r;
    uint16_t margin_v;
} SubtitleCue;

typedef struct {
    char *name;
    uint32_t cue_start;
    uint32_t cue_count;
    uint8_t supports_positioning;
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
    bool has_resume;
    uint8_t scale_mode;
    uint8_t playback_rate_index;
    uint8_t subtitle_font_index;
    int8_t subtitle_size;
    uint8_t subtitle_placement;
    uint16_t selected_subtitle_track;
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
    SUBTITLE_POS_AUTO,
    SUBTITLE_POS_COUNT,
} SubtitlePlacement;

typedef enum {
    SUBTITLE_CUE_POSITION_NONE = 0,
    SUBTITLE_CUE_POSITION_MARGIN = 1,
    SUBTITLE_CUE_POSITION_ABSOLUTE = 2,
} SubtitleCuePositionMode;

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
    int max_touch_dx;
    int max_touch_dy;
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
    PREFETCH_READY,
} PrefetchState;

typedef struct {
    uint8_t *chunk_storage;
    size_t chunk_storage_size;
    int chunk_index;
    PrefetchState state;
    size_t read_offset;
} PrefetchedChunk;

typedef struct {
    uint8_t *data;
    uint8_t *allocation;
    uint32_t frame_index;
    bool valid;
} H264FrameSlot;

typedef struct {
    storage_t *decoder;
    bool decoder_initialized;
    int chunk_index;
    int decoded_local_frame;
    bool chunk_dirty;
    bool frame_ready;
    const uint8_t *chunk_storage;
    size_t chunk_storage_size;
    uint8_t *chunk_bytes;
    size_t chunk_size;
    uint32_t *frame_offsets;
    size_t consumed_bytes;
    unsigned zero_advance_retries;
    uint16_t avg_mbs_per_ms_q8;
} H264BoundaryWarmup;

typedef struct {
    FILE *file;
    long current_file_pos;
    MovieHeader header;
    ChunkIndexEntry *chunk_index;
    SubtitleCue *subtitles;
    SubtitleTrack *subtitle_tracks;
    uint16_t subtitle_track_count;
    uint16_t selected_subtitle_track;
    uint16_t *framebuffer;
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
    bool io_throttled;
    uint32_t last_read_bytes;
    uint32_t last_read_time_ms;
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
    uint32_t diag_prefetch_read_bytes;
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
    H264BoundaryWarmup h264_boundary_warmup;
} Movie;

typedef struct {
    SDL_Surface *surface;
    const char *text;
    size_t subtitle_font_index;
    int subtitle_size;
    int wrap_width;
} SubtitleSurfaceCache;

typedef struct {
    uint8_t mode;
    uint8_t align;
    int wrap_width;
    SDL_Rect video_rect;
    int absolute_x;
    int absolute_y;
    int margin_l;
    int margin_r;
    int margin_v;
    SubtitlePlacement manual_placement;
    bool overlay_visible;
} SubtitleLayoutSpec;

typedef struct {
    SDL_Surface *surface;
    char label[96];
    uint32_t until_ms;
} ScreenshotPreviewState;

typedef struct {
    SDL_Surface *surface;
    int decoded_chunk_index;
    int marker_x;
    uint32_t hover_ms;
    uint32_t last_move_ms;
    int last_pointer_x;
    bool tracking;
    bool over_bar;
} SeekBarPreviewState;

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
static char (*g_debug_ring)[DEBUG_LINE_LEN] = NULL;
static size_t g_debug_ring_count = 0;
static size_t g_debug_ring_next = 0;
static char g_last_error_message[DEBUG_LINE_LEN];
static bool g_debug_logging_enabled = false;
static bool g_debug_metrics_enabled = false;
static H264ColorTables g_h264_color_tables_storage;
static H264ColorTables *g_h264_color_tables = &g_h264_color_tables_storage;
static bool g_h264_color_tables_in_sram = false;
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
static bool decode_h264_frame(
    Movie *movie,
    uint32_t frame_index,
    bool blit_output,
    bool store_prefetched
);
static bool load_chunk(Movie *movie, int chunk_index);
static bool prefetch_chunk(Movie *movie, int chunk_index);
static void prefetch_ahead(Movie *movie, int current_chunk, int max_new_chunks, int max_new_distance);
static PrefetchedChunk *find_prefetched_chunk(Movie *movie, int chunk_index);
static void clear_prefetched_chunk(PrefetchedChunk *chunk);
static bool prefetch_finish_chunk(Movie *movie, PrefetchedChunk *chunk);
static void prefetch_do_work(Movie *movie, int current_chunk, int max_work_distance, uint32_t deadline_ms, bool single_step);
static void prefetch_h264_frames(Movie *movie, bool paused, uint32_t spare_ms, uint32_t deadline_ms);
static void prefetch_h264_boundary_idr(Movie *movie, bool paused, uint32_t spare_ms, uint32_t deadline_ms);
static bool prefetch_one_h264_same_chunk_frame(Movie *movie, uint32_t spare_ms, uint32_t deadline_ms) __attribute__((unused));
static int movie_chunk_for_frame(const Movie *movie, uint32_t frame_index);
static int64_t h264_decoded_global_frame(const Movie *movie);
static bool next_chunk_needs_prefetch(const Movie *movie, int current_chunk);
static bool next_chunk_prefetched_ready(Movie *movie, int current_chunk);
static uint32_t next_chunk_prefetch_guard_frames(const Movie *movie, int current_chunk);
static bool should_run_h264_boundary_warmup(Movie *movie, int current_chunk);
static bool should_prioritize_h264_boundary_warmup(Movie *movie, int current_chunk);
static bool should_accelerate_next_chunk_io(Movie *movie, int current_chunk);
static bool should_prefetch_second_next_chunk(Movie *movie, int current_chunk);
static size_t active_h264_frame_ring_capacity(const Movie *movie);
static size_t h264_prefetch_target_ready_count(const Movie *movie, uint32_t spare_ms);
static void discard_h264_frame_ring_before(Movie *movie, uint32_t first_frame_to_keep);
static uint32_t h264_boundary_total_mbs(const Movie *movie, const H264BoundaryWarmup *warmup);
static void update_h264_boundary_warmup_rate(H264BoundaryWarmup *warmup, uint32_t elapsed_ms, uint32_t decoded_mbs);
static uint32_t h264_boundary_warmup_budget(const Movie *movie, const H264BoundaryWarmup *warmup, uint32_t spare_ms);
static void clear_h264_boundary_warmup(Movie *movie);
static bool prepare_h264_boundary_warmup(Movie *movie, int chunk_index);
static bool step_h264_boundary_warmup(Movie *movie, uint32_t macroblock_budget, uint32_t *out_elapsed_ms, uint32_t *out_decoded_mbs);
static bool activate_h264_boundary_warmup(Movie *movie);
static void free_fonts(Fonts *fonts);
static uint32_t monotonic_clock_now_ms(void);
static void debug_dump_session(const char *path, const Movie *movie, const char *reason);
static void history_path_for_directory(const char *directory, char *history_path, size_t history_path_size);
static bool load_history_store_from_path(const char *history_path, HistoryStore *history);
static int history_find_entry_index(const HistoryStore *history, const char *movie_path);
static void history_entry_init_defaults(HistoryEntry *entry);
static void apply_history_entry_settings(
    const HistoryEntry *entry,
    Movie *movie,
    ScaleMode *scale_mode,
    size_t *playback_rate_index,
    size_t *subtitle_font_index,
    int *subtitle_size,
    SubtitlePlacement *subtitle_placement
);
static bool debug_is_runtime_logging_enabled(void);
static void debug_clear_last_error(void);
static bool debug_should_collect_metrics(void);
static void debug_log_sram_status(void);
static void free_subtitle_surface_cache(SubtitleSurfaceCache *cache);
static void invalidate_subtitle_surface_cache(SubtitleSurfaceCache *cache);
static SDL_Surface *create_rgb565_surface(int width, int height);
static SDL_Surface *create_scaled_surface_from_surface(SDL_Surface *source, int max_width, int max_height);
static const char *filename_from_path(const char *path);
static bool decode_h264_frame_to_rgb565_buffer(Movie *movie, uint32_t frame_index, uint16_t *dst_pixels, size_t dst_pitch_pixels);
static bool finish_h264_boundary_warmup(Movie *movie);
static bool update_seek_bar_preview(Movie *movie, SeekBarPreviewState *preview, const PointerState *pointer, bool show_ui, uint32_t now_ms);
static void clear_seek_bar_preview(SeekBarPreviewState *preview);
static void clear_screenshot_preview(ScreenshotPreviewState *preview);
static void prepare_screenshot_preview(ScreenshotPreviewState *preview, SDL_Surface *screen, const char *saved_path);
static void begin_seek_preroll(const Movie *movie, bool *active, uint32_t *started_ms, size_t *target_ready_count);

static bool ensure_debug_ring_storage(void)
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

static void release_debug_ring_storage(void)
{
    free(g_debug_ring);
    g_debug_ring = NULL;
    g_debug_ring_count = 0;
    g_debug_ring_next = 0;
}

static bool debug_is_runtime_logging_enabled(void)
{
    return g_debug_logging_enabled;
}

static bool debug_should_collect_metrics(void)
{
    return g_debug_metrics_enabled || g_debug_logging_enabled;
}

static uint8_t *hardware_screen_bytes(void)
{
    return (uint8_t *) (uintptr_t) SCREEN_BASE_ADDRESS;
}

static void present_screen(SDL_Surface *screen)
{
    bool locked = false;

    if (!screen) {
        return;
    }
    if (SDL_MUSTLOCK(screen)) {
        if (SDL_LockSurface(screen) != 0) {
            return;
        }
        locked = true;
    }
    FastMemcpy(hardware_screen_bytes(), screen->pixels, SCREEN_BYTES_SIZE / 32);
    if (locked) {
        SDL_UnlockSurface(screen);
    }
}

static void debug_set_metrics_collection(bool enabled)
{
    g_debug_metrics_enabled = enabled;
}

static void debug_set_runtime_logging(bool enabled)
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

static void debug_tracevf(bool force, const char *fmt, va_list args)
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

static void debug_tracef(const char *fmt, ...)
{
    va_list args;

    if (!g_debug_logging_enabled) {
        return;
    }

    va_start(args, fmt);
    debug_tracevf(false, fmt, args);
    va_end(args);
}

static void debug_tracef_force(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    debug_tracevf(true, fmt, args);
    va_end(args);
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
    debug_tracef_force("%s", g_last_error_message);
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
    clear_h264_boundary_warmup(movie);
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
        if (movie->h264_boundary_warmup.chunk_index == victim->chunk_index) {
            clear_h264_boundary_warmup(movie);
        }
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

static void report_movie_open_failure(const char *movie_path)
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

static const char *filename_from_path(const char *path)
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

static SDL_Surface *create_rgb565_surface(int width, int height)
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

static SDL_Surface *create_scaled_surface_from_surface(SDL_Surface *source, int max_width, int max_height)
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

static void invalidate_subtitle_surface_cache(SubtitleSurfaceCache *cache)
{
    if (!cache) {
        return;
    }
    if (cache->surface) {
        SDL_FreeSurface(cache->surface);
    }
    memset(cache, 0, sizeof(*cache));
}

static void free_subtitle_surface_cache(SubtitleSurfaceCache *cache)
{
    invalidate_subtitle_surface_cache(cache);
}

static void clear_screenshot_preview(ScreenshotPreviewState *preview)
{
    if (!preview) {
        return;
    }
    if (preview->surface) {
        SDL_FreeSurface(preview->surface);
    }
    memset(preview, 0, sizeof(*preview));
}

static void clear_seek_bar_preview(SeekBarPreviewState *preview)
{
    if (!preview) {
        return;
    }
    if (preview->surface) {
        SDL_FreeSurface(preview->surface);
    }
    memset(preview, 0, sizeof(*preview));
    preview->decoded_chunk_index = -1;
    preview->last_pointer_x = -1;
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
    if (pointer->info) {
        pointer->max_touch_dx = pointer->info->width / POINTER_SPIKE_DELTA_DIVISOR;
        pointer->max_touch_dy = pointer->info->height / POINTER_SPIKE_DELTA_DIVISOR;
        if (pointer->max_touch_dx <= POINTER_JITTER_THRESHOLD) {
            pointer->max_touch_dx = POINTER_JITTER_THRESHOLD + 1;
        }
        if (pointer->max_touch_dy <= POINTER_JITTER_THRESHOLD) {
            pointer->max_touch_dy = POINTER_JITTER_THRESHOLD + 1;
        }
    }
    pointer->visible = pointer->info != NULL;
}

static bool pointer_update(PointerState *pointer)
{
    touchpad_report_t report;
    bool click_edge = false;
    bool current_down = false;
    bool has_touch_position;
    if (!pointer->info || touchpad_scan(&report) != 0) {
        return false;
    }
    pointer->moved = false;
    current_down = (report.pressed && report.arrow == TPAD_ARROW_CLICK) ? true : false;
    has_touch_position =
        (report.contact || report.proximity) &&
        report.x < pointer->info->width &&
        report.y < pointer->info->height;
    if (has_touch_position) {
        if (!pointer->tracking) {
            pointer->last_touch_x = report.x;
            pointer->last_touch_y = report.y;
            pointer->tracking = true;
        } else {
            int dx = (int) report.x - pointer->last_touch_x;
            int dy = (int) report.y - pointer->last_touch_y;
            int abs_dx;
            int abs_dy;
            abs_dx = dx < 0 ? -dx : dx;
            abs_dy = dy < 0 ? -dy : dy;
            if (abs_dx > pointer->max_touch_dx || abs_dy > pointer->max_touch_dy) {
                pointer->tracking = false;
            } else if (abs_dx <= POINTER_JITTER_THRESHOLD && abs_dy <= POINTER_JITTER_THRESHOLD) {
                dx = 0;
                dy = 0;
            } else {
                if (abs_dx * POINTER_AXIS_LOCK_RATIO_DEN >= abs_dy * POINTER_AXIS_LOCK_RATIO_NUM) {
                    dy = 0;
                } else if (abs_dy * POINTER_AXIS_LOCK_RATIO_DEN >= abs_dx * POINTER_AXIS_LOCK_RATIO_NUM) {
                    dx = 0;
                } else if (abs_dx + abs_dy < POINTER_DECISIVE_SUM_THRESHOLD) {
                    dx = 0;
                    dy = 0;
                }
            }
            if (pointer->tracking && (dx != 0 || dy != 0)) {
                pointer->fx += (dx * SCREEN_W * POINTER_GAIN_NUM << POINTER_FIXED_SHIFT) / ((int) pointer->info->width * POINTER_GAIN_DEN);
                pointer->fy -= (dy * SCREEN_H * POINTER_GAIN_NUM << POINTER_FIXED_SHIFT) / ((int) pointer->info->height * POINTER_GAIN_DEN);
                pointer->x = clamp_int(pointer->fx >> POINTER_FIXED_SHIFT, 0, SCREEN_W - 1);
                pointer->y = clamp_int(pointer->fy >> POINTER_FIXED_SHIFT, 0, SCREEN_H - 1);
                pointer->fx = pointer->x << POINTER_FIXED_SHIFT;
                pointer->fy = pointer->y << POINTER_FIXED_SHIFT;
                pointer->moved = true;
            }
            if (pointer->tracking) {
                pointer->last_touch_x = report.x;
                pointer->last_touch_y = report.y;
            }
        }
        pointer->visible = true;
    } else {
        pointer->tracking = false;
        if (current_down) {
            pointer->visible = true;
        }
    }
    /* Treat only the touchpad center click as a pointer activation.
     * Generic "pressed" also fires for directional pad presses, which should
     * not activate UI elements such as progress-bar seeking. */
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
    if (movie->h264_frame_bytes > 0) {
        int h264_index;
        for (h264_index = 0; h264_index < (int) active_h264_frame_ring_capacity(movie); ++h264_index) {
            if (movie->h264_frame_ring[h264_index].data) {
                stats.used_bytes += movie->h264_frame_bytes;
                h264_ring_bytes += movie->h264_frame_bytes;
            }
        }
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

static uint32_t tab_hold_frame_repeat_interval_ms(const Movie *movie)
{
    uint64_t interval_ms;

    if (!movie || !movie->header.fps_num || !movie->header.fps_den) {
        return TAB_HOLD_FRAME_REPEAT_FALLBACK_INTERVAL_MS;
    }

    interval_ms = (2000ULL * movie->header.fps_den + (movie->header.fps_num - 1U)) / movie->header.fps_num;
    if (interval_ms == 0ULL) {
        interval_ms = 1ULL;
    }
    if (interval_ms > UINT32_MAX) {
        interval_ms = UINT32_MAX;
    }
    return (uint32_t) interval_ms;
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

static bool step_movie_forward_one_frame(Movie *movie, bool *hover_preview_needs_rebuffer)
{
    if (!movie || movie->current_frame + 1 >= movie->header.frame_count) {
        return true;
    }
    if (!decode_to_frame(movie, movie->current_frame + 1)) {
        return false;
    }
    if (hover_preview_needs_rebuffer) {
        *hover_preview_needs_rebuffer = false;
    }
    return true;
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
    if (!debug_should_collect_metrics()) {
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
    free(movie->chunk_storage);
    free(movie->frame_offsets);
    free(movie->h264_boundary_warmup.frame_offsets);
    if (movie->h264_decoder) {
        if (movie->h264_decoder_initialized) {
            h264bsdShutdown(movie->h264_decoder);
        }
        h264bsdFree(movie->h264_decoder);
    }
    if (movie->h264_boundary_warmup.decoder) {
        if (movie->h264_boundary_warmup.decoder_initialized) {
            h264bsdShutdown(movie->h264_boundary_warmup.decoder);
        }
        h264bsdFree(movie->h264_boundary_warmup.decoder);
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
    movie->h264_boundary_warmup.chunk_index = -1;
    movie->h264_boundary_warmup.decoded_local_frame = -1;
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
    return movie &&
           (movie->header.version == MOVIE_VERSION_H264 || movie->header.version == MOVIE_VERSION_POSITIONED_SUBS);
}

static bool init_h264_color_tables(void)
{
    int index;
    H264ColorTables *tables = g_h264_color_tables;

    if (tables->initialized) {
        return true;
    }
    if (tables == &g_h264_color_tables_storage) {
        H264ColorTables *sram_tables = (H264ColorTables *) sram_alloc(sizeof(*sram_tables), 32U);
        if (sram_tables) {
            memset(sram_tables, 0, sizeof(*sram_tables));
            g_h264_color_tables = sram_tables;
            g_h264_color_tables_in_sram = true;
            tables = sram_tables;
        } else {
            memset(&g_h264_color_tables_storage, 0, sizeof(g_h264_color_tables_storage));
            g_h264_color_tables = &g_h264_color_tables_storage;
            g_h264_color_tables_in_sram = false;
            tables = &g_h264_color_tables_storage;
        }
    }

    for (index = 0; index < 256; ++index) {
        int y = index - 16;
        int chroma = index - 128;
        if (y < 0) {
            y = 0;
        }
        tables->y_base[index] = (298 * y) + 128;
        tables->u_to_blue[index] = 516 * chroma;
        tables->u_to_green[index] = -100 * chroma;
        tables->v_to_red[index] = 409 * chroma;
        tables->v_to_green[index] = -208 * chroma;
        tables->red565[index] = (uint16_t) ((index & 0xF8) << 8);
        tables->green565[index] = (uint16_t) ((index & 0xFC) << 3);
        tables->blue565[index] = (uint16_t) (index >> 3);
    }

    for (index = 0; index < H264_CLIP_TABLE_SIZE; ++index) {
        int value = index - H264_CLIP_OFFSET;
        if (value < 0) {
            value = 0;
        } else if (value > 255) {
            value = 255;
        }
        tables->clip[index] = (uint8_t) value;
    }

    tables->initialized = true;
    return true;
}

static inline uint8_t h264_clip_byte(int32_t value)
{
    /* The YUV->RGB fixed-point path keeps this in [-258, 534]. */
    return g_h264_color_tables->clip[value + H264_CLIP_OFFSET];
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
    *red = g_h264_color_tables->v_to_red[v_sample];
    *green = g_h264_color_tables->u_to_green[u_sample] + g_h264_color_tables->v_to_green[v_sample];
    *blue = g_h264_color_tables->u_to_blue[u_sample];
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
    bool clip_in_sram = false;
    bool qpc_in_sram = false;
    bool deblocking_in_sram = false;

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
    fprintf(log_file, "verbose_logging=%u\n", debug_is_runtime_logging_enabled() ? 1U : 0U);
    fprintf(log_file, "metrics_collection=%u\n", debug_should_collect_metrics() ? 1U : 0U);
    h264bsdGetSramStatus(&clip_in_sram, &qpc_in_sram, &deblocking_in_sram);
    fprintf(
        log_file,
        "sram enabled=%u used=%lu cap=%lu state=%s color=%u clip=%u qpc=%u deblock=%u\n",
        sram_is_enabled() ? 1U : 0U,
        (unsigned long) sram_bytes_used(),
        (unsigned long) sram_bytes_capacity(),
        sram_status_message(),
        g_h264_color_tables_in_sram ? 1U : 0U,
        clip_in_sram ? 1U : 0U,
        qpc_in_sram ? 1U : 0U,
        deblocking_in_sram ? 1U : 0U
    );

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
            "chunk loads_sync=%lu loads_prefetched=%lu read_ops=%lu read_bytes=%lu max_spare_ms=%lu\n",
            (unsigned long) movie->diag_chunk_load_sync_count,
            (unsigned long) movie->diag_chunk_load_prefetched_count,
            (unsigned long) movie->diag_prefetch_read_ops,
            (unsigned long) movie->diag_prefetch_read_bytes,
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

static void debug_log_sram_status(void)
{
    bool clip_in_sram = false;
    bool qpc_in_sram = false;
    bool deblocking_in_sram = false;

    h264bsdGetSramStatus(&clip_in_sram, &qpc_in_sram, &deblocking_in_sram);
    debug_tracef(
        "sram status enabled=%u used=%lu/%lu state=%s color=%u clip=%u qpc=%u deblock=%u",
        sram_is_enabled() ? 1U : 0U,
        (unsigned long) sram_bytes_used(),
        (unsigned long) sram_bytes_capacity(),
        sram_status_message(),
        g_h264_color_tables_in_sram ? 1U : 0U,
        clip_in_sram ? 1U : 0U,
        qpc_in_sram ? 1U : 0U,
        deblocking_in_sram ? 1U : 0U
    );
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
    {
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
    if (contiguous_ready < H264_PREFETCH_ACTIVE_LOW_WATERMARK && spare_ms >= 14U) {
        return 3U;
    }
    return 2U;
}

static bool h264_should_allow_active_prefetch(const Movie *movie, uint32_t spare_ms)
{
    if (spare_ms >= 16U) {
        return true;
    }
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
        if (debug_should_collect_metrics()) {
            movie->diag_prefetch_frame_decode_fail_count++;
        }
        debug_tracef("prefetch rescue fail frame=%lu chunk=%d", (unsigned long) next_frame, current_chunk);
        return false;
    }
    decode_elapsed_ms = monotonic_clock_now_ms() - decode_start_ms;
    if (debug_should_collect_metrics()) {
        movie->diag_prefetch_frame_decode_count++;
    }
    record_h264_prefetch_decode_time(movie, decode_elapsed_ms);
    record_h264_background_decode_kind(movie, true, decode_elapsed_ms);
    if (debug_is_runtime_logging_enabled() && decode_elapsed_ms >= DEBUG_TRACE_PREFETCH_MS) {
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

static bool reset_h264_storage_decoder(storage_t *decoder, bool *initialized)
{
    if (!decoder || !initialized) {
        debug_failf("h264 decoder reset failed: decoder missing");
        return false;
    }
    if (*initialized) {
        h264bsdShutdown(decoder);
    }
    if (h264bsdInit(decoder, HANTRO_TRUE) != HANTRO_OK) {
        *initialized = false;
        debug_failf("h264 decoder init failed");
        return false;
    }
    *initialized = true;
    return true;
}

static bool read_h264_picture_params(
    const Movie *movie,
    storage_t *decoder,
    uint32_t *full_width,
    uint32_t *full_height,
    uint32_t *crop_left,
    uint32_t *crop_top,
    uint32_t *crop_width,
    uint32_t *crop_height
)
{
    u32 cropping_flag = 0;
    u32 out_crop_left = 0;
    u32 out_crop_width = 0;
    u32 out_crop_top = 0;
    u32 out_crop_height = 0;
    u32 out_full_width;
    u32 out_full_height;

    if (!movie || !decoder || !full_width || !full_height ||
        !crop_left || !crop_top || !crop_width || !crop_height) {
        debug_failf("h264 picture params failed: decoder missing");
        return false;
    }

    out_full_width = h264bsdPicWidth(decoder) * 16U;
    out_full_height = h264bsdPicHeight(decoder) * 16U;
    h264bsdCroppingParams(decoder, &cropping_flag, &out_crop_left, &out_crop_width, &out_crop_top, &out_crop_height);
    if (!cropping_flag) {
        out_crop_left = 0;
        out_crop_top = 0;
        out_crop_width = out_full_width;
        out_crop_height = out_full_height;
    }
    if (out_crop_width != movie->header.video_width || out_crop_height != movie->header.video_height) {
        debug_failf(
            "h264 size mismatch crop=%lux%lu header=%ux%u",
            (unsigned long) out_crop_width,
            (unsigned long) out_crop_height,
            (unsigned) movie->header.video_width,
            (unsigned) movie->header.video_height
        );
        return false;
    }

    *full_width = out_full_width;
    *full_height = out_full_height;
    *crop_left = out_crop_left;
    *crop_top = out_crop_top;
    *crop_width = out_crop_width;
    *crop_height = out_crop_height;
    return true;
}

static void store_h264_picture_params(
    Movie *movie,
    uint32_t full_width,
    uint32_t full_height,
    uint32_t crop_left,
    uint32_t crop_top,
    uint32_t crop_width,
    uint32_t crop_height
)
{
    if (!movie) {
        return;
    }

    movie->h264_full_width = full_width;
    movie->h264_full_height = full_height;
    movie->h264_crop_left = crop_left;
    movie->h264_crop_top = crop_top;
    movie->h264_crop_width = crop_width;
    movie->h264_crop_height = crop_height;
    movie->h264_headers_ready = true;
}

static bool sync_h264_picture_params(Movie *movie, storage_t *decoder, bool force_commit)
{
    uint32_t full_width = 0;
    uint32_t full_height = 0;
    uint32_t crop_left = 0;
    uint32_t crop_top = 0;
    uint32_t crop_width = 0;
    uint32_t crop_height = 0;

    if (!movie || !decoder) {
        debug_failf("h264 picture params failed: decoder missing");
        return false;
    }
    if (!read_h264_picture_params(
            movie,
            decoder,
            &full_width,
            &full_height,
            &crop_left,
            &crop_top,
            &crop_width,
            &crop_height)) {
        return false;
    }
    if (force_commit || !movie->h264_headers_ready) {
        store_h264_picture_params(movie, full_width, full_height, crop_left, crop_top, crop_width, crop_height);
    }
    return true;
}

static bool reset_h264_decoder(Movie *movie)
{
    if (!movie || !movie->h264_decoder) {
        debug_failf("h264 decoder reset failed: decoder missing");
        return false;
    }
    if (!reset_h264_storage_decoder(movie->h264_decoder, &movie->h264_decoder_initialized)) {
        return false;
    }
    movie->h264_headers_ready = false;
    movie->h264_full_width = 0;
    movie->h264_full_height = 0;
    movie->h264_crop_left = 0;
    movie->h264_crop_top = 0;
    movie->h264_crop_width = 0;
    movie->h264_crop_height = 0;
    movie->h264_chunk_dirty = false;
    return true;
}

static inline uint32_t h264_pack_rgb565_pair(uint16_t left_pixel, uint16_t right_pixel)
{
    return ((uint32_t) right_pixel << 16) | left_pixel;
}

static bool blit_h264_planes_to_rgb565_target(
    const Movie *movie,
    const uint8_t *restrict y_plane,
    const uint8_t *restrict u_plane,
    const uint8_t *restrict v_plane,
    size_t luma_stride,
    size_t chroma_stride,
    uint16_t *restrict dst_pixels,
    size_t dst_pitch_pixels
)
{
    const int32_t *restrict y_base = g_h264_color_tables->y_base;
    const uint16_t *restrict red565 = g_h264_color_tables->red565;
    const uint16_t *restrict green565 = g_h264_color_tables->green565;
    const uint16_t *restrict blue565 = g_h264_color_tables->blue565;
    size_t y;

    if (!movie || !y_plane || !u_plane || !v_plane || !dst_pixels || !movie->h264_headers_ready) {
        return false;
    }

    for (y = 0; y < movie->h264_crop_height; y += 2U) {
        const uint8_t *restrict y_row0 = y_plane + ((size_t) y * luma_stride);
        const uint8_t *restrict y_row1 = y_row0 + luma_stride;
        const uint8_t *restrict u_row = u_plane + ((size_t) (y / 2U) * chroma_stride);
        const uint8_t *restrict v_row = v_plane + ((size_t) (y / 2U) * chroma_stride);
        uint16_t *restrict dst_row0_16 = dst_pixels + ((size_t) y * dst_pitch_pixels);
        uint16_t *restrict dst_row1_16 = dst_row0_16 + dst_pitch_pixels;
        const bool packed_writes = ((((uintptr_t) dst_row0_16) | ((uintptr_t) dst_row1_16)) & (sizeof(uint32_t) - 1U)) == 0U;
        const size_t main_width = movie->h264_crop_width & ~(size_t) 3U;
        size_t x;

        if (packed_writes) {
            uint32_t *restrict dst_row0 = (uint32_t *) (void *) dst_row0_16;
            uint32_t *restrict dst_row1 = (uint32_t *) (void *) dst_row1_16;

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
        } else {
            for (x = 0; x < main_width; x += 4U) {
                const size_t chroma_index = x / 2U;
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
                dst_row0_16[x] = p0;
                dst_row0_16[x + 1U] = p1;
                dst_row0_16[x + 2U] = p2;
                dst_row0_16[x + 3U] = p3;

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
                dst_row1_16[x] = p0;
                dst_row1_16[x + 1U] = p1;
                dst_row1_16[x + 2U] = p2;
                dst_row1_16[x + 3U] = p3;
            }

            for (; x < movie->h264_crop_width; x += 2U) {
                const size_t chroma_index = x / 2U;
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
                dst_row0_16[x] = p0;
                dst_row0_16[x + 1U] = p1;

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
                dst_row1_16[x] = p0;
                dst_row1_16[x + 1U] = p1;
            }
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

static bool blit_h264_picture_to_target(
    Movie *movie,
    const uint8_t *picture,
    uint16_t *dst_pixels,
    size_t dst_pitch_pixels
)
{
    const size_t luma_stride = movie->h264_full_width;
    const size_t chroma_stride = luma_stride / 2U;
    const size_t luma_plane_size = luma_stride * movie->h264_full_height;
    const size_t chroma_plane_size = chroma_stride * (movie->h264_full_height / 2U);
    const uint8_t *y_plane;
    const uint8_t *u_plane;
    const uint8_t *v_plane;

    if (!movie || !picture || !movie->h264_headers_ready || !dst_pixels) {
        return false;
    }

    y_plane = picture + ((size_t) movie->h264_crop_top * luma_stride) + movie->h264_crop_left;
    u_plane = picture + luma_plane_size
        + ((size_t) (movie->h264_crop_top / 2U) * chroma_stride)
        + (movie->h264_crop_left / 2U);
    v_plane = picture + luma_plane_size + chroma_plane_size
        + ((size_t) (movie->h264_crop_top / 2U) * chroma_stride)
        + (movie->h264_crop_left / 2U);

    return blit_h264_planes_to_rgb565_target(
        movie,
        y_plane,
        u_plane,
        v_plane,
        luma_stride,
        chroma_stride,
        dst_pixels,
        dst_pitch_pixels
    );
}

static bool blit_h264_frame_slot(Movie *movie, const H264FrameSlot *slot)
{
    const size_t luma_plane_size = (size_t) movie->header.video_width * movie->header.video_height;
    const size_t chroma_plane_size = ((size_t) movie->header.video_width / 2U) * ((size_t) movie->header.video_height / 2U);

    if (!movie || !slot || !slot->valid || !slot->data) {
        return false;
    }

    return blit_h264_planes_to_rgb565_target(
        movie,
        slot->data,
        slot->data + luma_plane_size,
        slot->data + luma_plane_size + chroma_plane_size,
        movie->header.video_width,
        movie->header.video_width / 2U,
        movie->framebuffer,
        movie->header.video_width
    );
}

static uint8_t *take_h264_output_picture(storage_t *decoder, const char *context)
{
    const char *label = context ? context : "h264";
    u32 pic_id = 0;
    u32 is_idr_pic = 0;
    u32 num_err_mbs = 0;
    uint8_t *picture;

    if (!decoder) {
        debug_failf("%s decoder missing", label);
        return NULL;
    }

    picture = h264bsdNextOutputPicture(decoder, &pic_id, &is_idr_pic, &num_err_mbs);
    if (!picture) {
        debug_failf("%s picture ready but no output picture", label);
        return NULL;
    }
    return picture;
}

static bool cache_h264_picture(Movie *movie, uint32_t frame_index, const uint8_t *picture, const char *context)
{
    const char *label = context ? context : "h264";
    H264FrameSlot *slot;

    if (!movie || !picture) {
        debug_failf("%s frame cache invalid input frame=%lu", label, (unsigned long) frame_index);
        return false;
    }

    slot = reserve_h264_frame_slot(movie, frame_index);
    if (!slot) {
        debug_tracef("%s frame ring full frame=%lu", label, (unsigned long) frame_index);
        return false;
    }
    if (!copy_h264_picture_to_slot(movie, slot, picture)) {
        debug_failf("%s frame copy failed frame=%lu", label, (unsigned long) frame_index);
        return false;
    }
    return true;
}

static bool pump_h264_access_unit(
    Movie *movie,
    storage_t *decoder,
    uint8_t *frame_data,
    size_t frame_size,
    size_t *inout_consumed,
    unsigned *inout_zero_advance_retries,
    uint32_t macroblock_budget,
    bool force_commit_picture_params,
    const char *context,
    bool *out_picture_ready,
    bool *out_pending,
    uint8_t **out_picture
)
{
    const char *label = context ? context : "h264";

    if (out_picture_ready) {
        *out_picture_ready = false;
    }
    if (out_pending) {
        *out_pending = false;
    }
    if (out_picture) {
        *out_picture = NULL;
    }
    if (!movie || !decoder || !frame_data || frame_size == 0 ||
        !inout_consumed || !inout_zero_advance_retries ||
        !out_picture_ready || !out_pending || !out_picture) {
        debug_failf("%s access-unit decode invalid input size=%lu", label, (unsigned long) frame_size);
        return false;
    }

    h264bsdSetMacroblockBudget(decoder, macroblock_budget);

    while (*inout_consumed < frame_size) {
        u32 read_bytes = 0;
        u32 result = h264bsdDecode(
            decoder,
            frame_data + *inout_consumed,
            (u32) (frame_size - *inout_consumed),
            0,
            &read_bytes
        );

        if (result == H264BSD_HDRS_RDY) {
            if (!sync_h264_picture_params(movie, decoder, force_commit_picture_params)) {
                return false;
            }
        } else if (result == H264BSD_PIC_RDY) {
            if (!movie->h264_headers_ready && !sync_h264_picture_params(movie, decoder, false)) {
                return false;
            }
            *out_picture = take_h264_output_picture(decoder, label);
            if (!(*out_picture)) {
                return false;
            }
            *out_picture_ready = true;
        } else if (result == H264BSD_PENDING) {
            *out_pending = true;
        } else if (result != H264BSD_RDY) {
            debug_failf("%s decode error result=%lu read=%lu", label, (unsigned long) result, (unsigned long) read_bytes);
            return false;
        }

        if (read_bytes == 0U) {
            if (result == H264BSD_HDRS_RDY && *inout_zero_advance_retries < 8U) {
                (*inout_zero_advance_retries)++;
                debug_tracef(
                    "%s hdrs ready retry=%u consumed=%lu size=%lu",
                    label,
                    *inout_zero_advance_retries,
                    (unsigned long) *inout_consumed,
                    (unsigned long) frame_size
                );
                continue;
            }
            if (!(*out_picture_ready) && !(*out_pending)) {
                debug_failf(
                    "%s decode stalled with zero-byte advance result=%lu retries=%u consumed=%lu size=%lu",
                    label,
                    (unsigned long) result,
                    *inout_zero_advance_retries,
                    (unsigned long) *inout_consumed,
                    (unsigned long) frame_size
                );
                return false;
            }
            return true;
        }

        *inout_zero_advance_retries = 0U;
        *inout_consumed += read_bytes;

        if (*out_picture_ready || *out_pending) {
            return true;
        }
        if (macroblock_budget > 0U && decoder->macroblockBudget == 0U) {
            *out_pending = true;
            return true;
        }
    }

    return *out_picture_ready;
}

static bool decode_h264_access_unit_to_target(
    Movie *movie,
    uint8_t *frame_data,
    size_t frame_size,
    uint32_t frame_index,
    uint16_t *dst_pixels,
    size_t dst_pitch_pixels,
    bool store_prefetched
)
{
    size_t consumed = 0;
    bool picture_ready = false;
    bool pending = false;
    unsigned zero_advance_retries = 0;
    uint8_t *picture = NULL;

    if (!movie || !movie->h264_decoder) {
        debug_failf("h264 access-unit decode invalid decoder");
        return false;
    }
    if (!pump_h264_access_unit(
            movie,
            movie->h264_decoder,
            frame_data,
            frame_size,
            &consumed,
            &zero_advance_retries,
            0U,
            true,
            "h264",
            &picture_ready,
            &pending,
            &picture)) {
        return false;
    }
    if (pending) {
        debug_failf("h264 decode yielded unexpectedly");
        return false;
    }
    if (!picture_ready || !picture) {
        return false;
    }
    if (store_prefetched && !cache_h264_picture(movie, frame_index, picture, "h264")) {
        return false;
    }
    if (dst_pixels && !blit_h264_picture_to_target(movie, picture, dst_pixels, dst_pitch_pixels)) {
        debug_failf("h264 blit failed");
        return false;
    }
    return true;
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
    return decode_h264_access_unit_to_target(
        movie,
        frame_data,
        frame_size,
        frame_index,
        blit_output ? movie->framebuffer : NULL,
        movie ? movie->header.video_width : 0U,
        store_prefetched
    );
}

static bool configure_chunk_view_from_storage(
    const Movie *movie,
    int chunk_index,
    const uint8_t *chunk_storage,
    size_t chunk_storage_size,
    uint32_t **out_frame_offsets,
    uint8_t **out_chunk_bytes,
    size_t *out_chunk_size
)
{
    const ChunkIndexEntry *entry;
    size_t local_offset;
    size_t table_bytes;
    size_t header_size;
    uint32_t *frame_offsets = NULL;
    uint8_t *chunk_bytes = NULL;
    size_t chunk_size = 0;
    uint32_t payload_size;

    if (!movie || !chunk_storage || !out_frame_offsets || !out_chunk_bytes || !out_chunk_size ||
        chunk_index < 0 || (uint32_t) chunk_index >= movie->header.chunk_count) {
        return false;
    }
    entry = movie->chunk_index + chunk_index;
    local_offset = (size_t) entry->frame_table_offset;
    table_bytes = (size_t) entry->frame_count * sizeof(uint32_t);
    header_size = 4U + table_bytes;

    if (local_offset + header_size > chunk_storage_size) {
        debug_failf(
            "chunk view invalid table chunk=%d table=%lu storage=%lu",
            chunk_index,
            (unsigned long) entry->frame_table_offset,
            (unsigned long) chunk_storage_size
        );
        return false;
    }
    payload_size = read_le32(chunk_storage + local_offset);
    if (local_offset + header_size + payload_size > chunk_storage_size) {
        debug_failf(
            "chunk view payload overflow chunk=%d payload=%lu table=%lu storage=%lu",
            chunk_index,
            (unsigned long) payload_size,
            (unsigned long) entry->frame_table_offset,
            (unsigned long) chunk_storage_size
        );
        return false;
    }
    frame_offsets = (uint32_t *) calloc((size_t) entry->frame_count, sizeof(uint32_t));
    if (!frame_offsets) {
        debug_failf("chunk view frame offset alloc failed chunk=%d count=%u", chunk_index, (unsigned) entry->frame_count);
        return false;
    }
    memcpy(frame_offsets, chunk_storage + local_offset + 4U, table_bytes);
    chunk_bytes = (uint8_t *) chunk_storage + local_offset + header_size;
    chunk_size = payload_size;
    *out_frame_offsets = frame_offsets;
    *out_chunk_bytes = chunk_bytes;
    *out_chunk_size = chunk_size;
    return true;
}

static bool configure_chunk_view(Movie *movie, int chunk_index)
{
    uint32_t *frame_offsets = NULL;
    uint8_t *chunk_bytes = NULL;
    size_t chunk_size = 0;

    free(movie->frame_offsets);
    movie->frame_offsets = NULL;
    movie->chunk_bytes = NULL;
    movie->chunk_size = 0;

    if (!configure_chunk_view_from_storage(
            movie,
            chunk_index,
            movie->chunk_storage,
            movie->chunk_storage_size,
            &frame_offsets,
            &chunk_bytes,
            &chunk_size)) {
        return false;
    }
    movie->frame_offsets = frame_offsets;
    movie->chunk_bytes = chunk_bytes;
    movie->chunk_size = chunk_size;
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
}

static void clear_prefetched_chunk(PrefetchedChunk *chunk)
{
    if (!chunk) {
        return;
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

static PrefetchedChunk *find_prefetch_work_chunk(Movie *movie, int current_chunk, int max_distance)
{
    PrefetchedChunk *best = NULL;
    int index;
    int wanted_min = current_chunk + 1;
    int wanted_max = current_chunk + max_distance;

    if (max_distance < 1) {
        return NULL;
    }

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

static bool prefetch_read_step(Movie *movie, PrefetchedChunk *chunk, bool respect_deadline, uint32_t deadline_ms)
{
    const ChunkIndexEntry *entry;
    size_t remaining;
    size_t read_size;
    size_t block_size;
    size_t bytes_read;
    long target_pos;
    uint32_t bytes_per_ms;
    uint32_t read_start_ms;
    uint32_t read_elapsed_ms;

    if (!movie || !chunk || chunk->chunk_index < 0) {
        return false;
    }
    entry = movie->chunk_index + chunk->chunk_index;
    if (chunk->read_offset > entry->packed_size) {
        return false;
    }
    remaining = (size_t) entry->packed_size - chunk->read_offset;
    if (remaining == 0) {
        chunk->state = PREFETCH_READY;
        return true;
    }
    if (entry->packed_size != entry->unpacked_size) {
        debug_failf(
            "prefetch chunk=%d unsupported packed=%lu unpacked=%lu",
            chunk->chunk_index,
            (unsigned long) entry->packed_size,
            (unsigned long) entry->unpacked_size
        );
        return false;
    }

    if (respect_deadline) {
        int32_t time_left_ms = (int32_t) (deadline_ms - monotonic_clock_now_ms());
        uint64_t dynamic_block_size;

        if (time_left_ms <= 0) {
            return false;
        }
        bytes_per_ms = movie->last_read_bytes / (movie->last_read_time_ms > 0 ? movie->last_read_time_ms : 1U);
        dynamic_block_size = (uint64_t) (uint32_t) time_left_ms * (uint64_t) bytes_per_ms;
        if (dynamic_block_size < 512U) {
            dynamic_block_size = 512U;
        } else if (dynamic_block_size > PREFETCH_FILE_BLOCK_SIZE) {
            dynamic_block_size = PREFETCH_FILE_BLOCK_SIZE;
        }
        block_size = (size_t) dynamic_block_size;
    } else {
        block_size = PREFETCH_FILE_BLOCK_SIZE;
    }
    read_size = remaining > block_size ? block_size : remaining;
    target_pos = (long) (entry->offset + chunk->read_offset);
    if (movie->current_file_pos != target_pos) {
        if (fseek(movie->file, target_pos, SEEK_SET) != 0) {
            movie->current_file_pos = -1;
            return false;
        }
        movie->current_file_pos = target_pos;
    }
    read_start_ms = monotonic_clock_now_ms();
    bytes_read = fread(chunk->chunk_storage + chunk->read_offset, 1, read_size, movie->file);
    read_elapsed_ms = monotonic_clock_now_ms() - read_start_ms;
    movie->last_read_time_ms = read_elapsed_ms;
    movie->last_read_bytes = (uint32_t) bytes_read;
    if (bytes_read != read_size) {
        movie->current_file_pos = -1;
        return false;
    }
    movie->current_file_pos += (long) read_size;
    if (debug_should_collect_metrics()) {
        movie->diag_prefetch_read_ops++;
        movie->diag_prefetch_read_bytes += (uint32_t) read_size;
    }
    chunk->read_offset += read_size;
    if (chunk->read_offset == entry->packed_size) {
        chunk->state = PREFETCH_READY;
    }
    return true;
}

static bool prefetch_process_chunk(Movie *movie, PrefetchedChunk *chunk, uint32_t deadline_ms, bool respect_deadline)
{
    while (chunk && chunk->state != PREFETCH_READY) {
        if (chunk->state != PREFETCH_READING) {
            return false;
        }
        if (!prefetch_read_step(movie, chunk, respect_deadline, deadline_ms)) {
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

    if (movie && movie->h264_boundary_warmup.chunk_index == chunk_index) {
        clear_h264_boundary_warmup(movie);
    }

retry:
    free(movie->chunk_storage);
    movie->chunk_storage = NULL;
    movie->chunk_storage_size = 0;

    if (entry->packed_size != entry->unpacked_size) {
        debug_failf(
            "load chunk=%d unsupported packed=%lu unpacked=%lu",
            chunk_index,
            (unsigned long) entry->packed_size,
            (unsigned long) entry->unpacked_size
        );
        return false;
    }

    ensure_app_memory_headroom(movie, entry->packed_size, 2U);
    movie->chunk_storage = (uint8_t *) malloc(entry->packed_size);
    if (!movie->chunk_storage) {
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
        movie->current_file_pos = -1;
        free(movie->chunk_storage);
        movie->chunk_storage = NULL;
        return false;
    }
    if (fread(movie->chunk_storage, 1, entry->packed_size, movie->file) != entry->packed_size) {
        debug_failf("load chunk=%d fread fail packed=%lu", chunk_index, (unsigned long) entry->packed_size);
        movie->current_file_pos = -1;
        free(movie->chunk_storage);
        movie->chunk_storage = NULL;
        return false;
    }
    movie->current_file_pos = (long) entry->offset + (long) entry->packed_size;
    movie->chunk_storage_size = entry->packed_size;
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
    if (debug_should_collect_metrics()) {
        movie->diag_chunk_load_sync_count++;
    }
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
        if (movie->h264_boundary_warmup.chunk_index == chunk_index) {
            clear_h264_boundary_warmup(movie);
            prefetched = find_prefetched_chunk(movie, chunk_index);
            if (!prefetched) {
                return load_chunk_from_file(movie, chunk_index, true);
            }
        }
        if (prefetched->state != PREFETCH_READY) {
            if (!prefetch_finish_chunk(movie, prefetched)) {
                debug_tracef(
                    "prefetch finish fail chunk=%d state=%d read=%lu",
                    chunk_index,
                    (int) prefetched->state,
                    (unsigned long) prefetched->read_offset
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
        if (debug_should_collect_metrics()) {
            movie->diag_chunk_load_prefetched_count++;
        }
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
        if (movie->h264_boundary_warmup.chunk_index == slot->chunk_index) {
            clear_h264_boundary_warmup(movie);
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
    if (movie->h264_boundary_warmup.chunk_index == slot->chunk_index) {
        clear_h264_boundary_warmup(movie);
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
    if (entry->packed_size != entry->unpacked_size) {
        debug_tracef(
            "prefetch unsupported chunk=%d packed=%lu unpacked=%lu",
            chunk_index,
            (unsigned long) entry->packed_size,
            (unsigned long) entry->unpacked_size
        );
        clear_prefetched_chunk(slot);
        return false;
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

static void prefetch_ahead(Movie *movie, int current_chunk, int max_new_chunks, int max_new_distance)
{
    int index;
    int loaded = 0;

    if (max_new_distance < 1) {
        max_new_distance = 1;
    } else if (max_new_distance > PREFETCH_CHUNK_COUNT) {
        max_new_distance = PREFETCH_CHUNK_COUNT;
    }

    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        int wanted_min = current_chunk + 1;
        int wanted_max = current_chunk + PREFETCH_CHUNK_COUNT;
        if (movie->prefetched[index].chunk_index >= 0 &&
            (movie->prefetched[index].chunk_index < wanted_min || movie->prefetched[index].chunk_index > wanted_max)) {
            if (movie->h264_boundary_warmup.chunk_index == movie->prefetched[index].chunk_index) {
                clear_h264_boundary_warmup(movie);
            }
            clear_prefetched_chunk(&movie->prefetched[index]);
        }
    }
    for (index = 1; index <= max_new_distance; ++index) {
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

static void prefetch_do_work(Movie *movie, int current_chunk, int max_work_distance, uint32_t deadline_ms, bool single_step)
{
    while (!prefetch_deadline_reached(deadline_ms)) {
        PrefetchedChunk *slot = find_prefetch_work_chunk(movie, current_chunk, max_work_distance);
        if (!slot) {
            break;
        }
        if (!prefetch_process_chunk(movie, slot, deadline_ms, true)) {
            debug_tracef(
                "prefetch work fail chunk=%d state=%d read=%lu",
                slot->chunk_index,
                (int) slot->state,
                (unsigned long) slot->read_offset
            );
            if (movie->h264_boundary_warmup.chunk_index == slot->chunk_index) {
                clear_h264_boundary_warmup(movie);
            }
            clear_prefetched_chunk(slot);
            break;
        }
        if (single_step) {
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

static bool next_chunk_prefetched_ready(Movie *movie, int current_chunk)
{
    PrefetchedChunk *prefetched;
    int next_chunk = current_chunk + 1;

    if (!movie || current_chunk < 0 || (uint32_t) next_chunk >= movie->header.chunk_count) {
        return false;
    }
    if (movie->loaded_chunk == next_chunk) {
        return false;
    }

    prefetched = find_prefetched_chunk(movie, next_chunk);
    return prefetched != NULL && prefetched->state == PREFETCH_READY && prefetched->chunk_storage != NULL;
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

static bool should_run_h264_boundary_warmup(Movie *movie, int current_chunk)
{
    H264BoundaryWarmup *warmup;
    int next_chunk = current_chunk + 1;

    if (!movie || !movie_uses_h264(movie) || current_chunk < 0 ||
        (uint32_t) current_chunk >= movie->header.chunk_count ||
        (uint32_t) next_chunk >= movie->header.chunk_count) {
        return false;
    }

    warmup = &movie->h264_boundary_warmup;
    if (warmup->chunk_index == next_chunk) {
        return !warmup->frame_ready;
    }
    return next_chunk_prefetched_ready(movie, current_chunk);
}

static bool should_prioritize_h264_boundary_warmup(Movie *movie, int current_chunk)
{
    const ChunkIndexEntry *entry;
    uint32_t frames_remaining;

    if (!should_run_h264_boundary_warmup(movie, current_chunk)) {
        return false;
    }

    entry = movie->chunk_index + current_chunk;
    if (movie->current_frame < entry->first_frame) {
        return true;
    }

    frames_remaining = (entry->first_frame + entry->frame_count) - movie->current_frame;
    return frames_remaining <= next_chunk_prefetch_guard_frames(movie, current_chunk);
}

static bool should_accelerate_next_chunk_io(Movie *movie, int current_chunk)
{
    const ChunkIndexEntry *entry;
    PrefetchedChunk *prefetched;
    uint32_t frames_remaining;
    uint32_t catchup_window;
    int next_chunk = current_chunk + 1;

    if (!movie || !movie_uses_h264(movie) || current_chunk < 0 ||
        (uint32_t) current_chunk >= movie->header.chunk_count ||
        (uint32_t) next_chunk >= movie->header.chunk_count) {
        return false;
    }

    prefetched = find_prefetched_chunk(movie, next_chunk);
    if (!prefetched || prefetched->state != PREFETCH_READING || prefetched->chunk_storage == NULL) {
        return false;
    }

    entry = movie->chunk_index + current_chunk;
    if (movie->current_frame < entry->first_frame) {
        return true;
    }

    frames_remaining = (entry->first_frame + entry->frame_count) - movie->current_frame;
    catchup_window = next_chunk_prefetch_guard_frames(movie, current_chunk) + H264_PREFETCH_NEXT_CHUNK_IO_CATCHUP_FRAMES;
    return frames_remaining <= catchup_window;
}

static bool should_prefetch_second_next_chunk(Movie *movie, int current_chunk)
{
    const ChunkIndexEntry *entry;
    uint32_t frames_remaining;
    int second_next_chunk = current_chunk + 2;

    if (!movie || !movie_uses_h264(movie) || current_chunk < 0 ||
        (uint32_t) current_chunk >= movie->header.chunk_count ||
        (uint32_t) second_next_chunk >= movie->header.chunk_count) {
        return false;
    }

    entry = movie->chunk_index + current_chunk;
    if (movie->current_frame < entry->first_frame) {
        return true;
    }

    frames_remaining = (entry->first_frame + entry->frame_count) - movie->current_frame;
    return frames_remaining <= H264_PREFETCH_SECOND_NEXT_CHUNK_WINDOW_FRAMES;
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
            if (debug_should_collect_metrics()) {
                movie->diag_prefetch_frame_decode_fail_count++;
            }
            debug_tracef("h264 predecode fail frame=%lu", (unsigned long) next_frame);
            break;
        }
        decode_elapsed_ms = monotonic_clock_now_ms() - decode_start_ms;
        if (debug_should_collect_metrics()) {
            movie->diag_prefetch_frame_decode_count++;
        }
        record_h264_prefetch_decode_time(movie, decode_elapsed_ms);
        record_h264_background_decode_kind(movie, same_chunk, decode_elapsed_ms);
        if (debug_is_runtime_logging_enabled() && decode_elapsed_ms >= DEBUG_TRACE_PREFETCH_MS) {
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

static void prefetch_h264_boundary_idr(Movie *movie, bool paused, uint32_t spare_ms, uint32_t deadline_ms)
{
    H264BoundaryWarmup *warmup;
    int current_chunk;
    int next_chunk;
    uint32_t next_frame;
    uint32_t remaining_ms;
    uint32_t budget_mbs;
    uint32_t elapsed_ms = 0U;
    uint32_t decoded_mbs = 0U;

    if (!movie || paused || !movie_uses_h264(movie) || spare_ms < H264_NEXT_CHUNK_IDR_MIN_SPARE_MS) {
        return;
    }

    warmup = &movie->h264_boundary_warmup;
    current_chunk = movie_chunk_for_frame(movie, movie->current_frame);
    if (current_chunk < 0 || (uint32_t) (current_chunk + 1) >= movie->header.chunk_count) {
        clear_h264_boundary_warmup(movie);
        return;
    }

    next_chunk = current_chunk + 1;
    next_frame = movie->chunk_index[next_chunk].first_frame;
    if (warmup->chunk_index >= 0 && warmup->chunk_index != next_chunk) {
        clear_h264_boundary_warmup(movie);
        warmup = &movie->h264_boundary_warmup;
    }
    if (warmup->chunk_index == next_chunk &&
        (movie->loaded_chunk == next_chunk || h264_decoded_global_frame(movie) >= (int64_t) next_frame)) {
        clear_h264_boundary_warmup(movie);
        return;
    }
    if (!should_run_h264_boundary_warmup(movie, current_chunk)) {
        return;
    }
    if (find_h264_frame_slot(movie, next_frame)) {
        return;
    }
    if (!prepare_h264_boundary_warmup(movie, next_chunk)) {
        return;
    }

    warmup = &movie->h264_boundary_warmup;
    if (warmup->frame_ready && !find_h264_frame_slot(movie, next_frame)) {
        clear_h264_boundary_warmup(movie);
        return;
    }
    if (warmup->frame_ready) {
        return;
    }
    remaining_ms = spare_ms;
    if (deadline_ms != 0U) {
        uint32_t now_ms = monotonic_clock_now_ms();
        if ((int32_t) (deadline_ms - now_ms) <= 0) {
            return;
        }
        remaining_ms = deadline_ms - now_ms;
    }
    budget_mbs = h264_boundary_warmup_budget(movie, warmup, remaining_ms);
    if (budget_mbs == 0U) {
        return;
    }
    if (!step_h264_boundary_warmup(movie, budget_mbs, &elapsed_ms, &decoded_mbs)) {
        debug_tracef("boundary warmup fail chunk=%d frame=%lu", next_chunk, (unsigned long) next_frame);
        clear_h264_boundary_warmup(movie);
        return;
    }
    update_h264_boundary_warmup_rate(warmup, elapsed_ms, decoded_mbs);
    if (debug_is_runtime_logging_enabled() && warmup->frame_ready) {
        debug_tracef(
            "boundary warmup ready frame=%lu chunk=%d ms=%lu",
            (unsigned long) next_frame,
            next_chunk,
            (unsigned long) elapsed_ms
        );
    }
}

static void prefetch_tick(Movie *movie, bool paused, uint32_t spare_ms)
{
    uint32_t time_slice_ms = paused && spare_ms > PREFETCH_PAUSED_SLICE_MS ? PREFETCH_PAUSED_SLICE_MS : spare_ms;
    uint32_t deadline_ms = spare_ms > 0 ? (monotonic_clock_now_ms() + spare_ms) : 0;
    int current_chunk = prefetch_target_chunk(movie);
    int budget = prefetch_budget_for_state(movie, paused, spare_ms);
    size_t ring_contig = movie_uses_h264(movie) ? h264_frame_ring_contiguous_ready_count(movie) : 0U;
    bool prioritize_io = false;
    bool prioritize_boundary_warmup = false;
    bool accelerate_next_chunk_io = false;
    bool allow_active_h264_prefetch = false;
    bool active_backoff = false;
    int max_new_prefetch_distance = PREFETCH_CHUNK_COUNT;
    int max_work_distance = PREFETCH_CHUNK_COUNT;
    size_t ring_growth = paused ? 8U : 0U;

    if (movie_uses_h264(movie)) {
        /* Let decoded-frame H.264 prefetch run whenever the cooperative
         * playback loop has enough slack to hide the work. */
        if (debug_should_collect_metrics()) {
            movie->diag_prefetch_tick_count++;
        }
        if (!paused) {
            if (debug_should_collect_metrics()) {
                movie->diag_active_prefetch_tick_count++;
            }
            if (movie->h264_active_prefetch_backoff > 0) {
                if (spare_ms >= 16U) {
                    movie->h264_active_prefetch_backoff = 0;
                } else {
                    active_backoff = true;
                    if (debug_should_collect_metrics()) {
                        movie->diag_prefetch_backoff_skip_count++;
                    }
                    movie->h264_active_prefetch_backoff--;
                }
            }
        }
        allow_active_h264_prefetch = paused || h264_should_allow_active_prefetch(movie, spare_ms);
        if (!paused && debug_should_collect_metrics() && !allow_active_h264_prefetch) {
            movie->diag_prefetch_suppressed_count++;
        }
    }

    if (!paused && movie_uses_h264(movie) && current_chunk >= 0) {
        uint32_t io_cushion;
        bool entered_rest = false;
        bool allow_second_next_chunk = false;

        /* Keep active-playback chunk I/O focused on the immediate next chunk.
         * Farther speculative chunks may stay resident, but do not start or
         * advance them on the frame-critical playback path. */
        max_new_prefetch_distance = 1;
        max_work_distance = 1;
        allow_second_next_chunk = should_prefetch_second_next_chunk(movie, current_chunk);
        if (allow_second_next_chunk) {
            max_new_prefetch_distance = 2;
            max_work_distance = 2;
        }
        prioritize_boundary_warmup = should_prioritize_h264_boundary_warmup(movie, current_chunk);
        accelerate_next_chunk_io = should_accelerate_next_chunk_io(movie, current_chunk);
        prioritize_io = should_prioritize_next_chunk_io(movie, current_chunk);
        if (prioritize_io || accelerate_next_chunk_io) {
            if (debug_should_collect_metrics()) {
                movie->diag_io_priority_count++;
            }
            if (time_slice_ms < H264_PREFETCH_IO_PRIORITY_SLICE_MS && spare_ms > 0) {
                time_slice_ms = spare_ms < H264_PREFETCH_IO_PRIORITY_SLICE_MS
                    ? spare_ms
                    : H264_PREFETCH_IO_PRIORITY_SLICE_MS;
            }
        }

        if (ring_contig >= 32U) {
            if (!movie->io_throttled) {
                movie->io_throttled = true;
                entered_rest = true;
            }
        } else if (ring_contig < 25U) {
            movie->io_throttled = false;
        }

        if (ring_contig >= 26U) {
            io_cushion = 50U;
        } else if (ring_contig >= 15U) {
            io_cushion = 10U;
        } else {
            io_cushion = 0U;
        }

        if (prioritize_io) {
            budget = 1;
        } else if (movie->io_throttled) {
            budget = 0;
            if (debug_is_runtime_logging_enabled() && entered_rest) {
                debug_tracef("io rest: buffer=%lu", (unsigned long) ring_contig);
            }
        } else if (spare_ms < io_cushion) {
            budget = 0;
            if (debug_is_runtime_logging_enabled() && ((movie->current_frame & 15U) == 0U)) {
                debug_tracef(
                    "io safety skip: buffer=%lu spare=%lu cushion=%u",
                    (unsigned long) ring_contig,
                    (unsigned long) spare_ms,
                    (unsigned) io_cushion
                );
            }
        } else {
            budget = 1;
        }
    }

    if (paused && movie_uses_h264(movie)) {
        try_grow_h264_frame_ring(movie, ring_growth);
    }
    if (!paused && movie_uses_h264(movie) && !prioritize_io) {
        uint32_t max_io_slice = accelerate_next_chunk_io
            ? spare_ms
            : ((spare_ms > 16U) ? (spare_ms / 2U) : PREFETCH_ACTIVE_H264_SLICE_MS);
        if (time_slice_ms > max_io_slice) {
            time_slice_ms = max_io_slice;
        }
    }
    if (movie_uses_h264(movie) && allow_active_h264_prefetch && !prioritize_io && !accelerate_next_chunk_io) {
        if (prioritize_boundary_warmup) {
            prefetch_h264_boundary_idr(movie, paused, spare_ms, deadline_ms);
        } else {
            prefetch_h264_frames(movie, paused, spare_ms, deadline_ms);
            prefetch_h264_boundary_idr(movie, paused, spare_ms, deadline_ms);
        }
    }
    if (current_chunk >= 0 && budget > 0 && time_slice_ms > 0) {
        uint32_t io_start_ms = monotonic_clock_now_ms();
        uint32_t io_deadline_ms = io_start_ms + time_slice_ms;
        if (!io_deadline_ms || !prefetch_deadline_reached(io_deadline_ms)) {
            prefetch_ahead(movie, current_chunk, budget, max_new_prefetch_distance);
            prefetch_do_work(
                movie,
                current_chunk,
                max_work_distance,
                io_deadline_ms,
                !paused && !prioritize_io && !accelerate_next_chunk_io
            );
            if (!paused) {
                uint32_t io_elapsed_ms = monotonic_clock_now_ms() - io_start_ms;
                if (io_elapsed_ms >= time_slice_ms + 8U) {
                    debug_tracef(
                        "prefetch io overrun ms=%lu slice=%lu spare=%lu chunk=%d prio=%u backoff=%u",
                        (unsigned long) io_elapsed_ms,
                        (unsigned long) time_slice_ms,
                        (unsigned long) spare_ms,
                        current_chunk,
                        prioritize_io ? 1U : 0U,
                        active_backoff ? 1U : 0U
                    );
                }
            }
        }
    }
    if (movie_uses_h264(movie) &&
        allow_active_h264_prefetch &&
        (!deadline_ms || !h264_prefetch_deadline_reached(deadline_ms))) {
        prefetch_h264_boundary_idr(movie, paused, spare_ms, deadline_ms);
        if (!prioritize_boundary_warmup || movie->h264_boundary_warmup.frame_ready) {
            prefetch_h264_frames(movie, paused, spare_ms, deadline_ms);
        }
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

static bool decode_h264_frame(
    Movie *movie,
    uint32_t frame_index,
    bool blit_output,
    bool store_prefetched
)
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
        if (debug_should_collect_metrics()) {
            movie->diag_h264_replay_count++;
            movie->diag_h264_replay_frames_total += replay_distance;
            if (replay_distance > movie->diag_h264_replay_max_distance) {
                movie->diag_h264_replay_max_distance = replay_distance;
            }
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

static bool decode_h264_frame_to_rgb565_buffer(
    Movie *movie,
    uint32_t frame_index,
    uint16_t *dst_pixels,
    size_t dst_pitch_pixels
)
{
    int chunk_index = movie_chunk_for_frame(movie, frame_index);
    const ChunkIndexEntry *entry;
    uint32_t local_index;
    uint32_t replay_index;

    if (!movie || !dst_pixels || dst_pitch_pixels == 0) {
        return false;
    }
    if (chunk_index < 0) {
        debug_failf("preview frame=%lu invalid h264 chunk", (unsigned long) frame_index);
        return false;
    }
    if (!load_chunk(movie, chunk_index)) {
        debug_tracef("preview frame=%lu load h264 chunk=%d fail", (unsigned long) frame_index, chunk_index);
        return false;
    }

    entry = movie->chunk_index + chunk_index;
    local_index = frame_index - entry->first_frame;
    if (movie->decoded_local_frame > (int) local_index) {
        if (movie->h264_chunk_dirty) {
            if (!load_chunk_from_file(movie, chunk_index, true)) {
                debug_failf("preview chunk reload failed chunk=%d", chunk_index);
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
        if (!decode_h264_access_unit_to_target(
                movie,
                movie->chunk_bytes + start,
                end - start,
                decoded_frame_index,
                decoded_frame_index == frame_index ? dst_pixels : NULL,
                dst_pitch_pixels,
                false)) {
            debug_tracef(
                "preview frame decode fail frame=%lu chunk=%d local=%lu size=%lu",
                (unsigned long) decoded_frame_index,
                chunk_index,
                (unsigned long) replay_index,
                (unsigned long) (end - start)
            );
            return false;
        }
        movie->decoded_local_frame = (int) replay_index;
    }

    return true;
}

static void invalidate_loaded_chunk_state(Movie *movie)
{
    if (!movie) {
        return;
    }
    free(movie->frame_offsets);
    movie->frame_offsets = NULL;
    movie->chunk_bytes = NULL;
    movie->chunk_size = 0;
    movie->loaded_chunk = -1;
    movie->decoded_local_frame = -1;
    movie->h264_chunk_dirty = false;
    clear_h264_boundary_warmup(movie);
}

static bool decode_to_frame(Movie *movie, uint32_t frame_index)
{
    int chunk_index = movie_chunk_for_frame(movie, frame_index);
    H264FrameSlot *prefetched_frame = NULL;
    const ChunkIndexEntry *entry;

    if (chunk_index < 0) {
        debug_failf("decode frame=%lu invalid chunk", (unsigned long) frame_index);
        return false;
    }
    if (frame_index > movie->current_frame) {
        int current_chunk = movie_chunk_for_frame(movie, movie->current_frame);
        if (movie_uses_h264(movie) &&
            movie->h264_boundary_warmup.chunk_index == chunk_index &&
            movie->h264_boundary_warmup.chunk_index >= 0 &&
            movie->chunk_index[chunk_index].first_frame == frame_index) {
            if ((!movie->h264_boundary_warmup.frame_ready && !finish_h264_boundary_warmup(movie)) ||
                !movie->h264_boundary_warmup.frame_ready ||
                !activate_h264_boundary_warmup(movie)) {
                clear_h264_boundary_warmup(movie);
            } else {
                prefetched_frame = find_h264_frame_slot(movie, frame_index);
            }
        } else {
            prefetched_frame = find_h264_frame_slot(movie, frame_index);
        }
        if (prefetched_frame) {
            if (debug_should_collect_metrics()) {
                movie->diag_foreground_ring_hit_count++;
            }
            if (!blit_h264_frame_slot(movie, prefetched_frame)) {
                debug_failf("h264 frame ring blit failed frame=%lu", (unsigned long) frame_index);
                return false;
            }
            movie->current_frame = frame_index;
            discard_h264_frame_ring_before(movie, frame_index + 1U);
            return true;
        }
        if (debug_should_collect_metrics()) {
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
    }
    if (frame_index < movie->current_frame) {
        clear_h264_frame_ring(movie);
        clear_h264_boundary_warmup(movie);
    }
    if (!load_chunk(movie, chunk_index)) {
        debug_tracef("decode frame=%lu load chunk=%d fail", (unsigned long) frame_index, chunk_index);
        return false;
    }
    entry = movie->chunk_index + chunk_index;
    if (entry->frame_count == 0) {
        debug_failf("decode frame=%lu empty chunk=%d", (unsigned long) frame_index, chunk_index);
        return false;
    }
    if (debug_should_collect_metrics()) {
        movie->diag_foreground_direct_decode_count++;
    }
    if (!decode_h264_frame(movie, frame_index, true, false)) {
        return false;
    }
    movie->current_frame = frame_index;
    discard_h264_frame_ring_before(movie, frame_index + 1U);
    return true;
}

static bool load_subtitles(
    Movie *movie,
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
    if (movie) {
        movie->current_file_pos = -1;
    }

    {
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
    }

    for (cue_index = 0; cue_index < header->subtitle_count; ++cue_index) {
        uint8_t meta[22];
        size_t meta_size = header->version >= MOVIE_VERSION_POSITIONED_SUBS ? 22U : 10U;
        uint16_t text_len;
        if (fread(meta, 1, meta_size, file) != meta_size) {
            debug_failf("subtitle cue meta read failed cue=%lu", (unsigned long) cue_index);
            goto fail;
        }
        cues[cue_index].start_ms = read_le32(meta);
        cues[cue_index].end_ms = read_le32(meta + 4);
        text_len = read_le16(meta + 8);
        if (meta_size > 10U) {
            cues[cue_index].position_mode = meta[10];
            cues[cue_index].align = meta[11];
            cues[cue_index].pos_x = read_le16(meta + 12);
            cues[cue_index].pos_y = read_le16(meta + 14);
            cues[cue_index].margin_l = read_le16(meta + 16);
            cues[cue_index].margin_r = read_le16(meta + 18);
            cues[cue_index].margin_v = read_le16(meta + 20);
        }
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
    for (track_index = 0; track_index < *out_track_count; ++track_index) {
        uint32_t start_index = tracks[track_index].cue_start;
        uint32_t end_index = start_index + tracks[track_index].cue_count;

        if (end_index > header->subtitle_count) {
            end_index = header->subtitle_count;
        }
        tracks[track_index].supports_positioning = 0;
        for (cue_index = start_index; cue_index < end_index; ++cue_index) {
            if ((cues[cue_index].position_mode == SUBTITLE_CUE_POSITION_MARGIN ||
                 cues[cue_index].position_mode == SUBTITLE_CUE_POSITION_ABSOLUTE) &&
                cues[cue_index].align >= 1 &&
                cues[cue_index].align <= 9) {
                tracks[track_index].supports_positioning = 1;
                break;
            }
        }
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
    memset(movie, 0, sizeof(*movie));
    movie->loaded_chunk = -1;
    movie->io_throttled = false;
    movie->last_read_bytes = 2048U;
    movie->last_read_time_ms = 1U;
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
    movie->h264_boundary_warmup.chunk_index = -1;
    movie->h264_boundary_warmup.decoded_local_frame = -1;
    debug_tracef("open start path=%s", path ? path : "(null)");

    movie->file = fopen(path, "rb");
    if (!movie->file) {
        debug_failf("open failed: fopen");
        return false;
    }
    movie->current_file_pos = 0;
    if (fread(&movie->header, 1, sizeof(movie->header), movie->file) != sizeof(movie->header)) {
        debug_failf("open failed: header read");
        return false;
    }
    movie->current_file_pos = (long) sizeof(movie->header);
    if (memcmp(movie->header.magic, "NVP1", 4) != 0) {
        debug_failf("open failed: bad magic");
        return false;
    }
    if (!movie_uses_h264(movie)) {
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
    movie->current_file_pos = (long) movie->header.index_offset;
    if (fread(movie->chunk_index, sizeof(ChunkIndexEntry), movie->header.chunk_count, movie->file) != movie->header.chunk_count) {
        debug_failf("open failed: chunk index read count=%lu", (unsigned long) movie->header.chunk_count);
        return false;
    }
    movie->current_file_pos += (long) (sizeof(ChunkIndexEntry) * movie->header.chunk_count);
    debug_tracef("open index loaded chunks=%lu", (unsigned long) movie->header.chunk_count);
    framebuffer_words = (size_t) movie->header.video_width * movie->header.video_height;
    movie->framebuffer = (uint16_t *) calloc(framebuffer_words, sizeof(uint16_t));
    init_h264_color_tables();
    movie->h264_decoder = h264bsdAlloc();
    if (movie->h264_decoder) {
        memset(movie->h264_decoder, 0, sizeof(*movie->h264_decoder));
    }
    movie->h264_boundary_warmup.decoder = h264bsdAlloc();
    if (movie->h264_boundary_warmup.decoder) {
        memset(movie->h264_boundary_warmup.decoder, 0, sizeof(*movie->h264_boundary_warmup.decoder));
    }
    movie->h264_frame_bytes = h264_cropped_frame_bytes(movie);
    movie->h264_frame_ring_capacity = 0;
    clear_h264_frame_ring(movie);
    if (!movie->framebuffer ||
        !movie->h264_decoder ||
        !movie->h264_boundary_warmup.decoder ||
        !reset_h264_decoder(movie) ||
        !reset_h264_storage_decoder(
            movie->h264_boundary_warmup.decoder,
            &movie->h264_boundary_warmup.decoder_initialized)) {
        if (!movie->framebuffer) {
            debug_failf("open failed: framebuffer alloc words=%lu", (unsigned long) framebuffer_words);
        } else if (!movie->h264_decoder) {
            debug_failf("open failed: h264 decoder alloc");
        } else if (!movie->h264_boundary_warmup.decoder) {
            debug_failf("open failed: h264 boundary decoder alloc");
        }
        return false;
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
    if (!load_subtitles(movie, movie->file, &movie->header, &movie->subtitles, &movie->subtitle_tracks, &movie->subtitle_track_count)) {
        debug_tracef("open subtitles disabled after alloc/read failure");
    }
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
                    files[count].has_resume = history.entries[history_index].has_resume;
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

static const SubtitleCue *active_subtitle_cue(const Movie *movie, uint32_t now_ms)
{
    uint32_t index;
    uint32_t start_index;
    uint32_t end_index;

    if (!movie || !movie->subtitles) {
        return NULL;
    }
    start_index = 0;
    end_index = movie->header.subtitle_count;

    if (movie->subtitle_track_count > 0 && movie->selected_subtitle_track < movie->subtitle_track_count) {
        start_index = movie->subtitle_tracks[movie->selected_subtitle_track].cue_start;
        end_index = start_index + movie->subtitle_tracks[movie->selected_subtitle_track].cue_count;
    }
    for (index = start_index; index < end_index; ++index) {
        if (now_ms >= movie->subtitles[index].start_ms && now_ms <= movie->subtitles[index].end_ms) {
            return &movie->subtitles[index];
        }
    }
    return NULL;
}

static uint32_t h264_boundary_total_mbs(const Movie *movie, const H264BoundaryWarmup *warmup)
{
    uint32_t width_mbs;
    uint32_t height_mbs;

    if (warmup && warmup->decoder && warmup->decoder->picSizeInMbs > 0U) {
        return warmup->decoder->picSizeInMbs;
    }
    if (!movie) {
        return 0U;
    }
    width_mbs = ((uint32_t) movie->header.video_width + 15U) / 16U;
    height_mbs = ((uint32_t) movie->header.video_height + 15U) / 16U;
    return width_mbs * height_mbs;
}

static void update_h264_boundary_warmup_rate(H264BoundaryWarmup *warmup, uint32_t elapsed_ms, uint32_t decoded_mbs)
{
    uint32_t sample_q8;

    if (!warmup || elapsed_ms == 0U || decoded_mbs == 0U) {
        return;
    }

    sample_q8 = (decoded_mbs << 8) / elapsed_ms;
    if (sample_q8 == 0U) {
        sample_q8 = 1U;
    }
    if (warmup->avg_mbs_per_ms_q8 == 0U) {
        warmup->avg_mbs_per_ms_q8 = (uint16_t) sample_q8;
    } else {
        warmup->avg_mbs_per_ms_q8 = (uint16_t) (((uint32_t) warmup->avg_mbs_per_ms_q8 * 3U + sample_q8 + 2U) / 4U);
    }
}

static uint32_t h264_boundary_warmup_budget(const Movie *movie, const H264BoundaryWarmup *warmup, uint32_t spare_ms)
{
    uint32_t rate_q8;
    uint32_t usable_ms;
    uint32_t decoded_mbs = 0U;
    uint32_t total_mbs;
    uint32_t remaining_mbs;
    uint32_t budget;

    if (!movie || !warmup || spare_ms < H264_NEXT_CHUNK_IDR_MIN_SPARE_MS) {
        return 0U;
    }

    total_mbs = h264_boundary_total_mbs(movie, warmup);
    if (warmup->decoder) {
        decoded_mbs = warmup->decoder->slice->numDecodedMbs;
    }
    remaining_mbs = total_mbs > decoded_mbs ? (total_mbs - decoded_mbs) : 1U;
    usable_ms = spare_ms > H264_NEXT_CHUNK_IDR_BUDGET_GUARD_MS
        ? (spare_ms - H264_NEXT_CHUNK_IDR_BUDGET_GUARD_MS)
        : 1U;
    rate_q8 = warmup->avg_mbs_per_ms_q8 > 0U
        ? (uint32_t) warmup->avg_mbs_per_ms_q8
        : H264_NEXT_CHUNK_IDR_DEFAULT_MBS_PER_MS_Q8;
    budget = (usable_ms * rate_q8) >> 8;
    if (budget == 0U) {
        budget = 1U;
    }
    if (budget > remaining_mbs) {
        budget = remaining_mbs;
    }
    return budget;
}

static void clear_h264_boundary_warmup(Movie *movie)
{
    H264BoundaryWarmup *warmup;
    uint16_t avg_mbs_per_ms_q8;
    storage_t *decoder;
    bool decoder_initialized;

    if (!movie) {
        return;
    }
    warmup = &movie->h264_boundary_warmup;
    avg_mbs_per_ms_q8 = warmup->avg_mbs_per_ms_q8;
    decoder = warmup->decoder;
    decoder_initialized = warmup->decoder_initialized;
    free(warmup->frame_offsets);
    memset(warmup, 0, sizeof(*warmup));
    warmup->decoder = decoder;
    warmup->decoder_initialized = decoder_initialized;
    warmup->avg_mbs_per_ms_q8 = avg_mbs_per_ms_q8;
    warmup->chunk_index = -1;
    warmup->decoded_local_frame = -1;
}

static bool prepare_h264_boundary_warmup(Movie *movie, int chunk_index)
{
    H264BoundaryWarmup *warmup;
    PrefetchedChunk *prefetched;
    uint32_t *frame_offsets = NULL;
    uint8_t *chunk_bytes = NULL;
    size_t chunk_size = 0;

    if (!movie || !movie_uses_h264(movie) || chunk_index < 0 ||
        (uint32_t) chunk_index >= movie->header.chunk_count) {
        return false;
    }

    warmup = &movie->h264_boundary_warmup;
    prefetched = find_prefetched_chunk(movie, chunk_index);
    if (!prefetched || prefetched->state != PREFETCH_READY || !prefetched->chunk_storage) {
        return false;
    }

    if (warmup->chunk_index == chunk_index &&
        warmup->chunk_storage == prefetched->chunk_storage &&
        warmup->frame_offsets != NULL) {
        return true;
    }

    clear_h264_boundary_warmup(movie);
    warmup = &movie->h264_boundary_warmup;
    if (!warmup->decoder || !reset_h264_storage_decoder(warmup->decoder, &warmup->decoder_initialized)) {
        return false;
    }
    if (!configure_chunk_view_from_storage(
            movie,
            chunk_index,
            prefetched->chunk_storage,
            prefetched->chunk_storage_size,
            &frame_offsets,
            &chunk_bytes,
            &chunk_size)) {
        return false;
    }

    warmup->chunk_index = chunk_index;
    warmup->decoded_local_frame = -1;
    warmup->chunk_dirty = false;
    warmup->frame_ready = false;
    warmup->chunk_storage = prefetched->chunk_storage;
    warmup->chunk_storage_size = prefetched->chunk_storage_size;
    warmup->chunk_bytes = chunk_bytes;
    warmup->chunk_size = chunk_size;
    warmup->frame_offsets = frame_offsets;
    warmup->consumed_bytes = 0U;
    warmup->zero_advance_retries = 0U;
    return true;
}

static bool step_h264_boundary_warmup(Movie *movie, uint32_t macroblock_budget, uint32_t *out_elapsed_ms, uint32_t *out_decoded_mbs)
{
    H264BoundaryWarmup *warmup;
    const ChunkIndexEntry *entry;
    uint32_t frame_index;
    uint8_t *frame_data;
    size_t start;
    size_t end;
    size_t frame_size;
    uint32_t start_ms;
    uint32_t start_decoded_mbs;
    size_t start_consumed_bytes;
    uint32_t total_mbs;
    bool picture_ready = false;
    bool pending = false;
    uint8_t *picture = NULL;

    if (out_elapsed_ms) {
        *out_elapsed_ms = 0U;
    }
    if (out_decoded_mbs) {
        *out_decoded_mbs = 0U;
    }
    if (!movie) {
        return false;
    }

    warmup = &movie->h264_boundary_warmup;
    if (!warmup->decoder || warmup->chunk_index < 0 || !warmup->frame_offsets || warmup->frame_ready) {
        return warmup->frame_ready;
    }

    entry = movie->chunk_index + warmup->chunk_index;
    if (entry->frame_count == 0U) {
        debug_failf("boundary warmup empty chunk=%d", warmup->chunk_index);
        return false;
    }

    start = warmup->frame_offsets[0];
    end = (entry->frame_count > 1U) ? warmup->frame_offsets[1] : warmup->chunk_size;
    if (end <= start || end > warmup->chunk_size) {
        debug_failf(
            "boundary warmup invalid frame bounds chunk=%d start=%lu end=%lu size=%lu",
            warmup->chunk_index,
            (unsigned long) start,
            (unsigned long) end,
            (unsigned long) warmup->chunk_size
        );
        return false;
    }

    frame_index = entry->first_frame;
    frame_data = warmup->chunk_bytes + start;
    frame_size = end - start;
    start_ms = monotonic_clock_now_ms();
    start_consumed_bytes = warmup->consumed_bytes;
    start_decoded_mbs = warmup->decoder->slice->numDecodedMbs;
    total_mbs = h264_boundary_total_mbs(movie, warmup);
    if (!pump_h264_access_unit(
            movie,
            warmup->decoder,
            frame_data,
            frame_size,
            &warmup->consumed_bytes,
            &warmup->zero_advance_retries,
            macroblock_budget,
            false,
            "boundary warmup",
            &picture_ready,
            &pending,
            &picture)) {
        return false;
    }

    if (warmup->consumed_bytes != start_consumed_bytes ||
        warmup->decoder->slice->numDecodedMbs != start_decoded_mbs) {
        warmup->chunk_dirty = true;
    }

    if (picture_ready) {
        if (!picture || !cache_h264_picture(movie, frame_index, picture, "boundary warmup")) {
            return false;
        }
        warmup->frame_ready = true;
        warmup->decoded_local_frame = 0;
        warmup->consumed_bytes = frame_size;
        if (out_elapsed_ms) {
            *out_elapsed_ms = monotonic_clock_now_ms() - start_ms;
        }
        if (out_decoded_mbs) {
            *out_decoded_mbs = total_mbs > start_decoded_mbs ? (total_mbs - start_decoded_mbs) : 0U;
        }
        return true;
    }

    if (pending) {
        if (out_elapsed_ms) {
            *out_elapsed_ms = monotonic_clock_now_ms() - start_ms;
        }
        if (out_decoded_mbs) {
            *out_decoded_mbs = warmup->decoder->slice->numDecodedMbs - start_decoded_mbs;
        }
        return true;
    }

    if (out_elapsed_ms) {
        *out_elapsed_ms = monotonic_clock_now_ms() - start_ms;
    }
    if (out_decoded_mbs) {
        *out_decoded_mbs = warmup->decoder->slice->numDecodedMbs - start_decoded_mbs;
    }
    return warmup->frame_ready;
}

static bool finish_h264_boundary_warmup(Movie *movie)
{
    uint32_t elapsed_ms = 0U;
    uint32_t decoded_mbs = 0U;

    if (!movie || movie->h264_boundary_warmup.chunk_index < 0) {
        return false;
    }
    if (movie->h264_boundary_warmup.frame_ready) {
        return true;
    }
    if (!step_h264_boundary_warmup(movie, 0U, &elapsed_ms, &decoded_mbs)) {
        return false;
    }
    update_h264_boundary_warmup_rate(&movie->h264_boundary_warmup, elapsed_ms, decoded_mbs);
    return movie->h264_boundary_warmup.frame_ready;
}

static bool activate_h264_boundary_warmup(Movie *movie)
{
    H264BoundaryWarmup *warmup;
    PrefetchedChunk *prefetched;
    storage_t *decoder;
    bool decoder_initialized;
    uint32_t frame_index;

    if (!movie) {
        return false;
    }

    warmup = &movie->h264_boundary_warmup;
    if (warmup->chunk_index < 0 || !warmup->frame_ready || !warmup->frame_offsets) {
        return false;
    }
    frame_index = movie->chunk_index[warmup->chunk_index].first_frame;
    if (!find_h264_frame_slot(movie, frame_index)) {
        return false;
    }

    prefetched = find_prefetched_chunk(movie, warmup->chunk_index);
    if (!prefetched || prefetched->state != PREFETCH_READY || prefetched->chunk_storage != warmup->chunk_storage) {
        return false;
    }

    free(movie->chunk_storage);
    movie->chunk_storage = prefetched->chunk_storage;
    movie->chunk_storage_size = prefetched->chunk_storage_size;
    prefetched->chunk_storage = NULL;
    reset_prefetched_chunk(prefetched);

    free(movie->frame_offsets);
    movie->frame_offsets = warmup->frame_offsets;
    movie->chunk_bytes = warmup->chunk_bytes;
    movie->chunk_size = warmup->chunk_size;
    movie->loaded_chunk = warmup->chunk_index;
    movie->decoded_local_frame = warmup->decoded_local_frame;
    movie->h264_chunk_dirty = warmup->chunk_dirty;

    decoder = movie->h264_decoder;
    decoder_initialized = movie->h264_decoder_initialized;
    movie->h264_decoder = warmup->decoder;
    movie->h264_decoder_initialized = warmup->decoder_initialized;
    warmup->decoder = decoder;
    warmup->decoder_initialized = decoder_initialized;

    warmup->frame_offsets = NULL;
    if (debug_should_collect_metrics()) {
        movie->diag_chunk_load_prefetched_count++;
    }
    debug_tracef(
        "load chunk=%d boundary prefetched packed=%lu unpacked=%lu prefetched=%lu",
        movie->loaded_chunk,
        (unsigned long) movie->chunk_storage_size,
        (unsigned long) movie->chunk_storage_size,
        (unsigned long) total_prefetched_chunk_bytes(movie)
    );
    clear_h264_boundary_warmup(movie);
    return true;
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

static const char *subtitle_placement_label(SubtitlePlacement placement)
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

static bool subtitle_track_supports_auto_positioning(const Movie *movie, uint16_t track_index)
{
    if (!movie || !movie->subtitle_tracks || movie->subtitle_track_count == 0 || track_index >= movie->subtitle_track_count) {
        return false;
    }
    return movie->subtitle_tracks[track_index].supports_positioning != 0;
}

static bool selected_subtitle_track_supports_auto_positioning(const Movie *movie)
{
    if (!movie || movie->subtitle_track_count == 0 || movie->selected_subtitle_track >= movie->subtitle_track_count) {
        return false;
    }
    return subtitle_track_supports_auto_positioning(movie, movie->selected_subtitle_track);
}

static SubtitlePlacement subtitle_normalize_placement(SubtitlePlacement placement, bool auto_supported)
{
    placement = (SubtitlePlacement) clamp_int((int) placement, SUBTITLE_POS_BAR_BOTTOM, SUBTITLE_POS_COUNT - 1);
    if (!auto_supported && placement == SUBTITLE_POS_AUTO) {
        return SUBTITLE_POS_BAR_BOTTOM;
    }
    return placement;
}

static SubtitlePlacement subtitle_cycle_placement(SubtitlePlacement placement, bool auto_supported)
{
    SubtitlePlacement next = subtitle_normalize_placement(placement, auto_supported);

    do {
        next = (SubtitlePlacement) ((next + 1) % SUBTITLE_POS_COUNT);
    } while (!auto_supported && next == SUBTITLE_POS_AUTO);
    return next;
}

static SubtitlePlacement subtitle_effective_manual_placement(SubtitlePlacement placement, SubtitlePlacement fallback)
{
    if (placement == SUBTITLE_POS_AUTO) {
        if (fallback == SUBTITLE_POS_AUTO) {
            return SUBTITLE_POS_BAR_BOTTOM;
        }
        return fallback;
    }
    return placement;
}

static int subtitle_align_column(uint8_t align)
{
    int value = clamp_int((int) align, 1, 9) - 1;
    return value % 3;
}

static int subtitle_align_row(uint8_t align)
{
    int value = clamp_int((int) align, 1, 9) - 1;
    return value / 3;
}

static int subtitle_scale_coord(uint16_t value, int extent)
{
    if (extent <= 0) {
        return 0;
    }
    return (int) (((uint32_t) value * (uint32_t) extent + (SUBTITLE_COORD_SCALE / 2U)) / SUBTITLE_COORD_SCALE);
}

static bool subtitle_resolve_layout_spec(
    const SDL_Rect *video_rect,
    bool overlay_visible,
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
    layout->overlay_visible = overlay_visible;
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
        layout->overlay_visible = overlay_visible;
        layout->manual_placement = effective_manual;
    }

    if (layout->manual_placement == SUBTITLE_POS_BAR_BOTTOM || layout->manual_placement == SUBTITLE_POS_BAR_TOP) {
        layout->wrap_width = SCREEN_W - 12;
    } else {
        layout->wrap_width = video_rect->w - 12;
    }
    return layout->wrap_width > 0;
}

static void subtitle_layout_dst_rect(
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
        y = clamp_int(y, video_rect->y, video_rect->y + video_rect->h - surface_h);
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
            y = (video_rect->y + video_rect->h) - layout->margin_v - surface_h;
        } else if (row == 2) {
            y = video_rect->y + layout->margin_v;
        } else {
            y = video_rect->y + ((video_rect->h - surface_h) / 2);
        }

        x = clamp_int(x, video_rect->x, video_rect->x + video_rect->w - surface_w);
        y = clamp_int(y, video_rect->y, video_rect->y + video_rect->h - surface_h);
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
                area_bottom = (layout->overlay_visible ? (SCREEN_H - UI_BAR_H) : SCREEN_H) - 2;
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
                area_y = (video_rect->y + video_rect->h - 8) - surface_h;
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

static bool ensure_subtitle_surface_cache(
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

static void draw_subtitle_cached(
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

static void draw_subtitle(
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

static void draw_centered_text_badge(SDL_Surface *screen, const Fonts *fonts, int center_x, int y, const char *label)
{
    int text_w = nSDL_GetStringWidth(fonts->white, label);
    int width = text_w + 10;
    SDL_Rect badge = {
        (Sint16) clamp_int(center_x - (width / 2), 0, SCREEN_W - width),
        (Sint16) y,
        (Uint16) width,
        16
    };

    SDL_FillRect(screen, &badge, SDL_MapRGB(screen->format, 18, 18, 24));
    nSDL_DrawString(screen, fonts->white, badge.x + 5, badge.y + 4, "%s", label);
}

static void draw_surface_panel(SDL_Surface *screen, SDL_Surface *surface, int x, int y)
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

    SDL_FillRect(screen, &border, SDL_MapRGB(screen->format, 0, 0, 0));
    SDL_FillRect(screen, &inner, SDL_MapRGB(screen->format, 12, 18, 28));
    SDL_BlitSurface(surface, NULL, screen, &dst);
}

static void draw_screenshot_preview_osd(
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

static void format_seek_delta(int32_t delta_ms, char *buffer, size_t buffer_size)
{
    uint32_t magnitude_ms;
    char time_text[24];

    if (!buffer || buffer_size == 0) {
        return;
    }

    if (delta_ms == 0) {
        snprintf(buffer, buffer_size, "0s");
        return;
    }

    magnitude_ms = (uint32_t) (delta_ms < 0 ? -delta_ms : delta_ms);
    format_clock(magnitude_ms, time_text, sizeof(time_text));
    snprintf(buffer, buffer_size, "%c%s", delta_ms < 0 ? '-' : '+', time_text);
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
        "F%lu R%lu/%lu T%lu L%lu H%lu D%lu%s",
        movie ? (unsigned long) movie->current_frame : 0UL,
        (unsigned long) ring_contig,
        (unsigned long) ring_cap,
        (unsigned long) ready_target,
        movie ? (unsigned long) movie->diag_lag_event_count : 0UL,
        movie ? (unsigned long) movie->diag_foreground_ring_hit_count : 0UL,
        movie ? (unsigned long) movie->diag_foreground_direct_decode_count : 0UL,
        debug_is_runtime_logging_enabled() ? " DBG ON" : ""
    );
    snprintf(
        perf_medium,
        sizeof(perf_medium),
        "F%lu R%lu/%lu L%lu%s",
        movie ? (unsigned long) movie->current_frame : 0UL,
        (unsigned long) ring_contig,
        (unsigned long) ring_cap,
        movie ? (unsigned long) movie->diag_lag_event_count : 0UL,
        debug_is_runtime_logging_enabled() ? " DBG ON" : ""
    );
    snprintf(
        perf_short,
        sizeof(perf_short),
        "F%lu L%lu%s",
        movie ? (unsigned long) movie->current_frame : 0UL,
        movie ? (unsigned long) movie->diag_lag_event_count : 0UL,
        debug_is_runtime_logging_enabled() ? " DBG" : ""
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

static void draw_help_row(SDL_Surface *screen, const Fonts *fonts, int shortcut_x, int description_x, int y, const char *shortcut, const char *description)
{
    nSDL_DrawString(screen, fonts->white, shortcut_x, y, "%s", shortcut);
    nSDL_DrawString(screen, fonts->white, description_x, y, "%s", description);
}

static void draw_help_menu(SDL_Surface *screen, const Fonts *fonts)
{
    const int menu_w = 296;
    const int menu_h = 200;
    const int safe_h = SCREEN_H - UI_BAR_H;
    SDL_Rect border = {(SCREEN_W - menu_w) / 2, (safe_h - menu_h) / 2, menu_w, menu_h};
    SDL_Rect panel = {border.x + 1, border.y + 1, border.w - 2, border.h - 2};
    SDL_Rect header = {panel.x, panel.y, panel.w, 24};
    SDL_Rect accent = {panel.x, panel.y + header.h, panel.w, 2};
    const char *close_text = "CAT close";
    const char *title_text = "Playback Controls";
    const struct {
        const char *shortcut;
        const char *description;
    } rows[] = {
        {"ENTER", "Play or pause"},
        {"CLICK", "Toggle or seek bar"},
        {"L / R", "Seek -/+5s"},
        {"TAB", "Step one frame"},
        {"/", "Scale mode"},
        {"{ / }", "Playback speed"},
        {"^", "Subtitle position"},
        {"+ / -", "Subtitle size"},
        {"F", "Cycle subtitle font"},
        {"T", "Switch subtitle track"},
        {"M", "Memory overlay"},
        {"D", "Toggle debug logging"},
        {"S", "Save BMP screenshot"},
        {"TOUCHPAD", "Move cursor / show UI"},
        {"ESC", "Close menu or exit"},
    };
    int max_shortcut_w = 0;
    int shortcut_x;
    int description_x;
    int y;
    size_t index;

    SDL_FillRect(screen, &border, SDL_MapRGB(screen->format, 0, 0, 0));
    SDL_FillRect(screen, &panel, SDL_MapRGB(screen->format, 8, 10, 14));
    SDL_FillRect(screen, &header, SDL_MapRGB(screen->format, 12, 18, 28));
    SDL_FillRect(screen, &accent, SDL_MapRGB(screen->format, 32, 182, 255));

    nSDL_DrawString(screen, fonts->white, panel.x + 10, panel.y + 6, "%s", title_text);
    nSDL_DrawString(
        screen,
        fonts->white,
        panel.x + panel.w - 10 - nSDL_GetStringWidth(fonts->white, close_text),
        panel.y + 6,
        "%s",
        close_text
    );

    for (index = 0; index < sizeof(rows) / sizeof(rows[0]); ++index) {
        int width = nSDL_GetStringWidth(fonts->white, rows[index].shortcut);
        if (width > max_shortcut_w) {
            max_shortcut_w = width;
        }
    }
    shortcut_x = panel.x + 12;
    description_x = shortcut_x + max_shortcut_w + 16;
    y = accent.y + 10;
    for (index = 0; index < sizeof(rows) / sizeof(rows[0]); ++index) {
        draw_help_row(screen, fonts, shortcut_x, description_x, y, rows[index].shortcut, rows[index].description);
        y += 11;
    }
}

static void draw_progress(
    SDL_Surface *screen,
    const Fonts *fonts,
    const Movie *movie,
    uint32_t current_ms,
    const PointerState *pointer,
    int32_t pending_seek_ms,
    const SeekBarPreviewState *seek_preview
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
    char hover_text[24];
    char seek_text[24];
    uint32_t duration_ms = movie_duration_ms(movie);
    bool hover_bar = false;
    uint32_t hover_ms = 0;
    int hover_badge_y = bar_back.y - 20;
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
        int marker_x = clamp_int(pointer->x, bar_back.x, bar_back.x + bar_back.w - 1);
        SDL_Rect marker = {(Sint16) marker_x, (Sint16) (bar_back.y - 2), 1, 10};
        SDL_FillRect(screen, &marker, SDL_MapRGB(screen->format, 255, 255, 255));
        hover_bar = pointer->y >= overlay.y && pointer->y < SCREEN_H;
        if (hover_bar && duration_ms > 0) {
            int preview_x;
            int preview_y;
            hover_ms = (uint32_t) (((uint64_t) duration_ms * (uint32_t) (marker_x - bar_back.x)) / (uint32_t) bar_back.w);
            format_clock(hover_ms, hover_text, sizeof(hover_text));
            if (seek_preview && seek_preview->surface && seek_preview->over_bar) {
                preview_x = clamp_int(
                    marker_x - ((seek_preview->surface->w + 4) / 2),
                    0,
                    SCREEN_W - (seek_preview->surface->w + 4)
                );
                preview_y = bar_back.y - seek_preview->surface->h - 28;
                if (preview_y < 0) {
                    preview_y = 0;
                }
                draw_surface_panel(screen, seek_preview->surface, preview_x, preview_y);
                hover_badge_y = preview_y + seek_preview->surface->h + 8;
            }
            draw_centered_text_badge(screen, fonts, marker_x, hover_badge_y, hover_text);
        }
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
    if (pending_seek_ms != 0) {
        format_seek_delta(pending_seek_ms, seek_text, sizeof(seek_text));
        if (pending_seek_ms < 0) {
            draw_left_text_badge(screen, fonts, 10, (SCREEN_H / 2) - 8, seek_text);
        } else {
            draw_text_badge(screen, fonts, SCREEN_W - 10, (SCREEN_H / 2) - 8, seek_text);
        }
    }
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
    SubtitleSurfaceCache *subtitle_cache,
    size_t subtitle_font_index,
    bool subtitle_font_overlay_visible,
    int subtitle_size,
    SubtitlePlacement subtitle_placement,
    const char *status_overlay_text,
    const ScreenshotPreviewState *screenshot_preview,
    const SeekBarPreviewState *seek_preview,
    uint32_t now_ms,
    const PointerState *pointer,
    int32_t pending_seek_ms
)
{
    SDL_Rect src;
    SDL_Rect dst;
    Uint32 black = SDL_MapRGB(screen->format, 0, 0, 0);
    int memory_right_limit = SCREEN_W - 8;
    uint32_t current_ms = movie_frame_time_ms(movie, movie->current_frame);
    const SubtitleCue *subtitle_cue = active_subtitle_cue(movie, current_ms);
    const char *subtitle = subtitle_cue ? subtitle_cue->text : NULL;
    SubtitlePlacement effective_subtitle_placement = subtitle_normalize_placement(
        subtitle_placement,
        selected_subtitle_track_supports_auto_positioning(movie)
    );
    bool playback_badge_visible = show_ui && !help_menu_open;
    bool memory_badge_visible = !help_menu_open && (memory_overlay_mode == MEMORY_OVERLAY_ALWAYS);

    compute_video_rects(movie, scale_mode, &src, &dst);
    if (dst.y > 0) {
        SDL_Rect top_bar = {0, 0, SCREEN_W, dst.y};
        int bottom_y = dst.y + dst.h;
        SDL_Rect bottom_bar = {0, bottom_y, SCREEN_W, SCREEN_H - bottom_y};

        SDL_FillRect(screen, &top_bar, black);
        if (bottom_bar.h > 0) {
            SDL_FillRect(screen, &bottom_bar, black);
        }
    }
    if (dst.x > 0) {
        SDL_Rect left_bar = {0, dst.y, dst.x, dst.h};
        int right_x = dst.x + dst.w;
        SDL_Rect right_bar = {right_x, dst.y, SCREEN_W - right_x, dst.h};

        SDL_FillRect(screen, &left_bar, black);
        if (right_bar.w > 0) {
            SDL_FillRect(screen, &right_bar, black);
        }
    }
    if (dst.w == movie->header.video_width && dst.h == movie->header.video_height) {
        SDL_BlitSurface(movie->frame_surface, NULL, screen, &dst);
    } else {
        SDL_SoftStretch(movie->frame_surface, &src, screen, &dst);
    }
    if (subtitle && subtitle_size >= 0) {
        SubtitleLayoutSpec subtitle_layout;

        if (subtitle_resolve_layout_spec(
                &dst,
                show_ui,
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
                show_ui,
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
        draw_progress(screen, fonts, movie, current_ms, pointer, pending_seek_ms, seek_preview);
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
    draw_screenshot_preview_osd(screen, fonts, screenshot_preview, now_ms);
    present_screen(screen);
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
        present_screen(screen);
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
    present_screen(screen);
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

static void history_entry_init_defaults(HistoryEntry *entry)
{
    if (!entry) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    entry->scale_mode = (uint8_t) SCALE_FIT;
    entry->playback_rate_index = (uint8_t) PLAYBACK_RATE_DEFAULT_INDEX;
    entry->subtitle_font_index = (uint8_t) SUBTITLE_FONT_DEFAULT_INDEX;
    entry->subtitle_size = 0;
    entry->subtitle_placement = (uint8_t) SUBTITLE_POS_BAR_BOTTOM;
}

static void apply_history_entry_settings(
    const HistoryEntry *entry,
    Movie *movie,
    ScaleMode *scale_mode,
    size_t *playback_rate_index,
    size_t *subtitle_font_index,
    int *subtitle_size,
    SubtitlePlacement *subtitle_placement
)
{
    if (!entry || !movie || !scale_mode || !playback_rate_index || !subtitle_font_index || !subtitle_size || !subtitle_placement) {
        return;
    }

    *scale_mode = (ScaleMode) clamp_int((int) entry->scale_mode, SCALE_FIT, SCALE_NATIVE);
    *playback_rate_index = (size_t) clamp_int((int) entry->playback_rate_index, 0, (int) (PLAYBACK_RATE_COUNT - 1U));
    *subtitle_font_index = (size_t) clamp_int((int) entry->subtitle_font_index, 0, (int) (SUBTITLE_FONT_CHOICE_COUNT - 1U));
    *subtitle_size = clamp_int((int) entry->subtitle_size, -1, 3);

    if (movie->subtitle_track_count == 0) {
        movie->selected_subtitle_track = 0;
    } else {
        uint16_t max_track = (uint16_t) (movie->subtitle_track_count - 1U);
        movie->selected_subtitle_track = (uint16_t) clamp_int((int) entry->selected_subtitle_track, 0, (int) max_track);
    }
    *subtitle_placement = subtitle_normalize_placement(
        (SubtitlePlacement) clamp_int((int) entry->subtitle_placement, SUBTITLE_POS_BAR_BOTTOM, SUBTITLE_POS_COUNT - 1),
        selected_subtitle_track_supports_auto_positioning(movie)
    );
}

static bool load_history_store_from_path(const char *history_path, HistoryStore *history)
{
    FILE *file;
    char line[MAX_PATH_LEN + 64];
    bool version2 = false;

    memset(history, 0, sizeof(*history));
    file = fopen(history_path, "rb");
    if (!file) {
        return true;
    }
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return false;
    }
    if (strncmp(line, HISTORY_MAGIC_V2, 5) == 0) {
        version2 = true;
    } else if (strncmp(line, HISTORY_MAGIC_V1, 5) != 0) {
        fclose(file);
        return false;
    }
    while (history->count < HISTORY_MAX_ENTRIES && fgets(line, sizeof(line), file)) {
        char *path = NULL;
        HistoryEntry entry;

        history_entry_init_defaults(&entry);
        if (version2) {
            char *fields[9];
            size_t field_index;

            fields[0] = line;
            for (field_index = 1; field_index < 9; ++field_index) {
                char *separator = strchr(fields[field_index - 1], '\t');
                if (!separator) {
                    break;
                }
                *separator = '\0';
                fields[field_index] = separator + 1;
            }
            if (field_index < 9) {
                continue;
            }

            path = fields[8];
            entry.has_resume = strtoul(fields[0], NULL, 10) != 0;
            entry.frame = (uint32_t) strtoul(fields[1], NULL, 10);
            entry.scale_mode = (uint8_t) strtoul(fields[2], NULL, 10);
            entry.playback_rate_index = (uint8_t) strtoul(fields[3], NULL, 10);
            entry.subtitle_font_index = (uint8_t) strtoul(fields[4], NULL, 10);
            entry.subtitle_size = (int8_t) strtol(fields[5], NULL, 10);
            entry.subtitle_placement = (uint8_t) strtoul(fields[6], NULL, 10);
            entry.selected_subtitle_track = (uint16_t) strtoul(fields[7], NULL, 10);
        } else {
            char *separator = strchr(line, '\t');
            if (!separator) {
                continue;
            }
            *separator = '\0';
            path = separator + 1;
            entry.has_resume = true;
            entry.frame = (uint32_t) strtoul(line, NULL, 10);
        }

        path[strcspn(path, "\r\n")] = '\0';
        if (path[0] == '\0') {
            continue;
        }
        entry.path = dup_string(path);
        history->entries[history->count] = entry;
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
    fputs(HISTORY_MAGIC_V2 "\n", file);
    for (index = 0; index < history->count && index < HISTORY_MAX_ENTRIES; ++index) {
        fprintf(
            file,
            "%u\t%lu\t%u\t%u\t%u\t%d\t%u\t%u\t%s\n",
            history->entries[index].has_resume ? 1U : 0U,
            (unsigned long) history->entries[index].frame,
            (unsigned) history->entries[index].scale_mode,
            (unsigned) history->entries[index].playback_rate_index,
            (unsigned) history->entries[index].subtitle_font_index,
            (int) history->entries[index].subtitle_size,
            (unsigned) history->entries[index].subtitle_placement,
            (unsigned) history->entries[index].selected_subtitle_track,
            history->entries[index].path
        );
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

static void history_upsert_entry(
    HistoryStore *history,
    const char *movie_path,
    const Movie *movie,
    bool has_resume,
    uint32_t frame,
    ScaleMode scale_mode,
    size_t playback_rate_index,
    size_t subtitle_font_index,
    int subtitle_size,
    SubtitlePlacement subtitle_placement
)
{
    HistoryEntry entry;
    size_t index;

    history_entry_init_defaults(&entry);
    history_remove_entry(history, movie_path);
    entry.path = dup_string(movie_path);
    entry.has_resume = has_resume;
    entry.frame = frame;
    entry.scale_mode = (uint8_t) clamp_int((int) scale_mode, SCALE_FIT, SCALE_NATIVE);
    entry.playback_rate_index = (uint8_t) clamp_int((int) playback_rate_index, 0, (int) (PLAYBACK_RATE_COUNT - 1U));
    entry.subtitle_font_index = (uint8_t) clamp_int((int) subtitle_font_index, 0, (int) (SUBTITLE_FONT_CHOICE_COUNT - 1U));
    entry.subtitle_size = (int8_t) clamp_int(subtitle_size, -1, 3);
    entry.subtitle_placement = (uint8_t) clamp_int((int) subtitle_placement, SUBTITLE_POS_BAR_BOTTOM, SUBTITLE_POS_COUNT - 1);
    entry.selected_subtitle_track = movie ? movie->selected_subtitle_track : 0;
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

static bool history_resume_frame_for_movie(const char *movie_path, uint32_t *out_frame) __attribute__((unused));
static bool history_resume_frame_for_movie(const char *movie_path, uint32_t *out_frame)
{
    HistoryStore history;
    int found_index;
    if (!load_history_store(movie_path, &history)) {
        return false;
    }
    found_index = history_find_entry_index(&history, movie_path);
    if (found_index >= 0 && history.entries[found_index].has_resume) {
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

static void save_history_position_for_movie(
    const char *movie_path,
    const Movie *movie,
    ScaleMode scale_mode,
    size_t playback_rate_index,
    size_t subtitle_font_index,
    int subtitle_size,
    SubtitlePlacement subtitle_placement
)
{
    HistoryStore history;
    bool has_resume = false;
    uint32_t frame = 0;

    if (!load_history_store(movie_path, &history)) {
        return;
    }
    if (should_save_history_position(movie)) {
        has_resume = true;
        frame = movie->current_frame;
    }
    history_upsert_entry(
        &history,
        movie_path,
        movie,
        has_resume,
        frame,
        scale_mode,
        playback_rate_index,
        subtitle_font_index,
        subtitle_size,
        subtitle_placement
    );
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

static void flush_deferred_history_save(
    const char *movie_path,
    const Movie *movie,
    bool *pending,
    const char *reason,
    bool force,
    ScaleMode scale_mode,
    size_t playback_rate_index,
    size_t subtitle_font_index,
    int subtitle_size,
    SubtitlePlacement subtitle_placement
)
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
    save_history_position_for_movie(
        movie_path,
        movie,
        scale_mode,
        playback_rate_index,
        subtitle_font_index,
        subtitle_size,
        subtitle_placement
    );
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

static void prepare_screenshot_preview(ScreenshotPreviewState *preview, SDL_Surface *screen, const char *saved_path)
{
    SDL_Surface *thumbnail;

    if (!preview) {
        return;
    }

    clear_screenshot_preview(preview);
    if (!screen || !saved_path || saved_path[0] == '\0') {
        return;
    }

    thumbnail = create_scaled_surface_from_surface(screen, SCREENSHOT_PREVIEW_MAX_W, SCREENSHOT_PREVIEW_MAX_H);
    if (!thumbnail) {
        return;
    }

    preview->surface = thumbnail;
    snprintf(preview->label, sizeof(preview->label), "Saved %.72s", filename_from_path(saved_path));
    preview->until_ms = monotonic_clock_now_ms() + SCREENSHOT_PREVIEW_MS;
}

static void begin_seek_preroll(const Movie *movie, bool *active, uint32_t *started_ms, size_t *target_ready_count)
{
    size_t capacity;
    size_t target;

    if (!active || !started_ms || !target_ready_count) {
        return;
    }

    *active = false;
    *started_ms = 0;
    *target_ready_count = 0;
    if (!movie || !movie_uses_h264(movie)) {
        return;
    }

    capacity = active_h264_frame_ring_capacity(movie);
    if (capacity >= SEEK_PREROLL_TARGET_HIGH_FRAMES) {
        target = SEEK_PREROLL_TARGET_HIGH_FRAMES;
    } else if (capacity >= SEEK_PREROLL_TARGET_LOW_FRAMES) {
        target = SEEK_PREROLL_TARGET_LOW_FRAMES;
    } else if (capacity == 0) {
        target = SEEK_PREROLL_TARGET_LOW_FRAMES;
    } else {
        target = capacity;
    }
    if (target == 0) {
        target = 1;
    }

    *active = true;
    *started_ms = monotonic_clock_now_ms();
    *target_ready_count = target;
}

static bool update_seek_bar_preview(Movie *movie, SeekBarPreviewState *preview, const PointerState *pointer, bool show_ui, uint32_t now_ms)
{
    SDL_Rect bar = progress_bar_rect();
    int marker_x;
    uint32_t duration_ms;
    uint32_t target_ms;
    uint32_t target_frame;
    int chunk_index;
    const ChunkIndexEntry *entry;
    uint16_t *preview_pixels = NULL;
    SDL_Surface *full_surface = NULL;
    SDL_Surface *thumbnail = NULL;
    bool ok = true;

    if (!preview) {
        return false;
    }

    preview->over_bar = false;
    if (!movie || !pointer || !pointer->visible || !show_ui ||
        pointer->y < SCREEN_H - UI_BAR_H || pointer->y >= SCREEN_H) {
        preview->tracking = false;
        preview->last_pointer_x = -1;
        return false;
    }

    preview->over_bar = true;
    marker_x = clamp_int(pointer->x, bar.x, bar.x + bar.w - 1);
    preview->marker_x = marker_x;
    duration_ms = movie_duration_ms(movie);
    if (duration_ms > 0) {
        target_ms = (uint32_t) (((uint64_t) duration_ms * (uint32_t) (marker_x - bar.x)) / (uint32_t) bar.w);
    } else {
        target_ms = 0;
    }
    preview->hover_ms = target_ms;

    if (!preview->tracking || pointer->moved || preview->last_pointer_x != marker_x) {
        preview->tracking = true;
        preview->last_pointer_x = marker_x;
        preview->last_move_ms = now_ms;
        return true;
    }
    if ((int32_t) (now_ms - preview->last_move_ms) < (int32_t) SEEK_BAR_PREVIEW_DEBOUNCE_MS) {
        return true;
    }
    if (movie->header.frame_count == 0) {
        return true;
    }

    target_frame = movie_frames_from_ms(movie, target_ms);
    if (target_frame >= movie->header.frame_count) {
        target_frame = movie->header.frame_count - 1U;
    }
    chunk_index = movie_chunk_for_frame(movie, target_frame);
    if (chunk_index < 0) {
        return false;
    }
    if (preview->surface && preview->decoded_chunk_index == chunk_index) {
        return true;
    }

    entry = movie->chunk_index + chunk_index;
    clear_h264_frame_ring(movie);
    clear_all_prefetched_chunks(movie);
    movie->h264_active_prefetch_backoff = H264_ACTIVE_PREFETCH_BACKOFF_HEAVY;
    preview_pixels = (uint16_t *) calloc((size_t) movie->header.video_width * movie->header.video_height, sizeof(uint16_t));
    if (!preview_pixels) {
        ok = false;
        goto cleanup;
    }
    if (!decode_h264_frame_to_rgb565_buffer(movie, entry->first_frame, preview_pixels, movie->header.video_width)) {
        ok = false;
        goto cleanup;
    }

    full_surface = SDL_CreateRGBSurfaceFrom(
        preview_pixels,
        movie->header.video_width,
        movie->header.video_height,
        16,
        movie->header.video_width * 2,
        0xF800, 0x07E0, 0x001F, 0
    );
    if (!full_surface) {
        ok = false;
        goto cleanup;
    }
    thumbnail = create_scaled_surface_from_surface(full_surface, SEEK_BAR_PREVIEW_MAX_W, SEEK_BAR_PREVIEW_MAX_H);
    if (!thumbnail) {
        ok = false;
        goto cleanup;
    }

cleanup:
    if (full_surface) {
        SDL_FreeSurface(full_surface);
    }
    free(preview_pixels);
    invalidate_loaded_chunk_state(movie);
    reset_h264_decoder(movie);
    if (!ok) {
        return false;
    }

    if (preview->surface) {
        SDL_FreeSurface(preview->surface);
    }
    preview->surface = thumbnail;
    preview->decoded_chunk_index = chunk_index;
    return true;
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

static bool clamp_seek(Movie *movie, int32_t delta_ms)
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
    return decode_to_frame(movie, target_frame);
}

static bool seek_to_ratio(Movie *movie, uint32_t numerator, uint32_t denominator)
{
    uint32_t duration_ms;
    uint32_t target_ms;
    uint32_t target_frame;
    if (denominator == 0 || movie->header.frame_count == 0) {
        return false;
    }
    duration_ms = movie_duration_ms(movie);
    target_ms = (uint32_t) (((uint64_t) duration_ms * numerator) / denominator);
    target_frame = movie_frames_from_ms(movie, target_ms);
    if (target_frame >= movie->header.frame_count) {
        target_frame = movie->header.frame_count - 1;
    }
    return decode_to_frame(movie, target_frame);
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
    const int title_y = panel.y + 28;
    int title_preview_gap;
    int time_label_y;

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
    title_preview_gap = preview.y - (title_y + nSDL_GetStringHeight(fonts->white, title));
    if (title_preview_gap < 0) {
        title_preview_gap = 0;
    }
    time_label_y = preview.y + preview.h + title_preview_gap;
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
        nSDL_DrawString(screen, fonts->white, panel.x + 12, title_y, "%s", title);
        nSDL_DrawString(screen, fonts->white, panel.x + 12, time_label_y, "%s", time_label);
        draw_prompt_button(screen, fonts, &continue_button, "CONTINUE", selected_button == 0);
        draw_prompt_button(screen, fonts, &restart_button, "START OVER", selected_button == 1);
        if (pointer.visible) {
            draw_cursor(screen, pointer.x, pointer.y);
        }
        present_screen(screen);

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
    bool prev_d = false;
    bool prev_s = false;
    bool prev_plus = false;
    bool prev_minus = false;
    bool paused = false;
    uint64_t frame_interval_ticks;
    uint64_t next_frame_due_ticks;
    uint64_t playback_anchor_ticks;
    uint32_t playback_anchor_frame;
    uint32_t tab_hold_repeat_interval_ms;
    uint32_t tab_repeat_next_ms = 0;
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
    SubtitleSurfaceCache subtitle_cache;
    ScreenshotPreviewState screenshot_preview;
    SeekBarPreviewState seek_preview;
    PointerState pointer;
    bool help_menu_open = false;
    bool help_resume_playback = false;
    uint32_t resume_frame = 0;
    uint32_t last_history_saved_ms = 0;
    bool history_save_pending = false;
    HistoryStore startup_history;
    int startup_history_index = -1;
    bool startup_has_resume = false;
    int32_t pending_seek_ms = 0;
    uint32_t pending_seek_commit_at_ms = 0;
    bool seek_preroll_active = false;
    uint32_t seek_preroll_started_ms = 0;
    size_t seek_preroll_target_ready_count = 0;
    bool hover_preview_needs_rebuffer = false;

    memset(&subtitle_cache, 0, sizeof(subtitle_cache));
    memset(&screenshot_preview, 0, sizeof(screenshot_preview));
    memset(&seek_preview, 0, sizeof(seek_preview));
    seek_preview.decoded_chunk_index = -1;
    seek_preview.last_pointer_x = -1;

    if (debug_is_runtime_logging_enabled()) {
        g_debug_ring_count = 0;
        g_debug_ring_next = 0;
    } else {
        release_debug_ring_storage();
    }
    debug_clear_last_error();
    {
        int phase;
        for (phase = 0; phase < 4; ++phase) {
            SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 8, 10, 14));
            draw_loading_overlay(screen, fonts, "Loading", phase);
            present_screen(screen);
            msleep(24);
        }
    }
    if (!load_movie(path, &movie)) {
        report_movie_open_failure(path);
        return -1;
    }
    if (load_history_store(path, &startup_history)) {
        startup_history_index = history_find_entry_index(&startup_history, path);
        if (startup_history_index >= 0) {
            apply_history_entry_settings(
                &startup_history.entries[startup_history_index],
                &movie,
                &scale_mode,
                &playback_rate_index,
                &subtitle_font_index,
                &subtitle_size,
                &subtitle_placement
            );
            if (startup_history.entries[startup_history_index].has_resume) {
                startup_has_resume = true;
                resume_frame = startup_history.entries[startup_history_index].frame;
            }
        }
        free_history_store(&startup_history);
    }
    if (startup_history_index < 0 && selected_subtitle_track_supports_auto_positioning(&movie)) {
        subtitle_placement = SUBTITLE_POS_AUTO;
    }
    subtitle_placement = subtitle_normalize_placement(
        subtitle_placement,
        selected_subtitle_track_supports_auto_positioning(&movie)
    );
    if (startup_has_resume && resume_frame < movie.header.frame_count) {
        int resume_choice = prompt_resume_position(screen, fonts, &movie, path, resume_frame);
        if (resume_choice < 0) {
            if (debug_is_runtime_logging_enabled()) {
                char log_path[MAX_PATH_LEN];
                debug_log_path_for_movie(path, log_path, sizeof(log_path));
                debug_dump_session(log_path, &movie, "resume-cancel");
            }
            destroy_movie(&movie);
            return 0;
        }
        if (resume_choice == 0) {
            decode_to_frame(&movie, 0);
        } else {
            snprintf(status_overlay_text, sizeof(status_overlay_text), "RESUMED");
            status_overlay_until = monotonic_clock_now_ms() + STATUS_OVERLAY_MS;
            begin_seek_preroll(&movie, &seek_preroll_active, &seek_preroll_started_ms, &seek_preroll_target_ready_count);
        }
    }
    pointer_init(&pointer);
    prev_tab = isKeyPressed(KEY_NSPIRE_TAB);
    prev_t = isKeyPressed(KEY_NSPIRE_T);
    prev_m = isKeyPressed(KEY_NSPIRE_M);
    prev_d = isKeyPressed(KEY_NSPIRE_D);
    prev_s = isKeyPressed(KEY_NSPIRE_S);
    debug_set_metrics_collection(debug_is_runtime_logging_enabled());
    frame_interval_ticks = movie_frame_interval_ticks(&movie);
    tab_hold_repeat_interval_ms = tab_hold_frame_repeat_interval_ms(&movie);
    last_history_saved_ms = movie_frame_time_ms(&movie, movie.current_frame);
    {
        prefetch_tick(&movie, true, 1000);
    }
    debug_tracef(
        "play start path=%s frames=%lu chunks=%lu frame=%lu chunk=%d",
        path,
        (unsigned long) movie.header.frame_count,
        (unsigned long) movie.header.chunk_count,
        (unsigned long) movie.current_frame,
        movie.loaded_chunk
    );
    debug_log_sram_status();
    playback_anchor_ticks = monotonic_clock_now_ticks();
    playback_anchor_frame = movie.current_frame;
    next_frame_due_ticks = playback_anchor_ticks;
    if (!seek_preroll_active) {
        reset_playback_timeline(
            &movie,
            playback_rate_for_index(playback_rate_index),
            &playback_anchor_ticks,
            &playback_anchor_frame,
            &next_frame_due_ticks
        );
    }
    ui_visible_until = monotonic_clock_now_ms() + POINTER_UI_TIMEOUT_MS;

    while (1) {
        bool pointer_click = pointer_update(&pointer);
        bool pending_seek_consumed_click = false;
        uint64_t now_ticks = monotonic_clock_now_ticks();
        uint32_t now_ms = monotonic_clock_ticks_to_ms(now_ticks);
        const PlaybackRate *playback_rate = playback_rate_for_index(playback_rate_index);
        bool show_ui = help_menu_open || paused || (now_ms <= ui_visible_until);
        bool enter_edge = key_pressed_edge(KEY_NSPIRE_ENTER, &prev_enter);
        bool tab_edge = key_pressed_edge(KEY_NSPIRE_TAB, &prev_tab);
        bool tab_down = prev_tab;
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
        bool seek_left_edge = key_pressed_edge(KEY_NSPIRE_LEFT, &prev_left);
        bool seek_right_edge = key_pressed_edge(KEY_NSPIRE_RIGHT, &prev_right);
        bool subtitle_font_edge = key_pressed_edge(KEY_NSPIRE_F, &prev_f);
        bool subtitle_track_edge = key_pressed_edge(KEY_NSPIRE_T, &prev_t);
        bool memory_overlay_edge = key_pressed_edge(KEY_NSPIRE_M, &prev_m);
        bool debug_logging_edge = key_pressed_edge(KEY_NSPIRE_D, &prev_d);
        bool screenshot_edge = key_pressed_edge(KEY_NSPIRE_S, &prev_s);
        bool take_screenshot = false;
        bool tab_repeat_step = false;

        if (tab_edge) {
            tab_repeat_next_ms = now_ms + TAB_HOLD_FRAME_REPEAT_DELAY_MS;
        } else if (!tab_down) {
            tab_repeat_next_ms = 0;
        } else if (tab_repeat_next_ms != 0U &&
                   paused &&
                   !help_menu_open &&
                   !seek_preroll_active &&
                   pending_seek_ms == 0 &&
                   (int32_t) (now_ms - tab_repeat_next_ms) >= 0) {
            tab_repeat_step = true;
            tab_repeat_next_ms = now_ms + tab_hold_repeat_interval_ms;
        }

        if (pointer.moved || pointer_click) {
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            show_ui = true;
        }
        if (!pointer_click) {
            bool allow_seek_preview = paused && show_ui && !help_menu_open;
            int previous_preview_chunk = seek_preview.decoded_chunk_index;
            if (allow_seek_preview &&
                update_seek_bar_preview(&movie, &seek_preview, &pointer, allow_seek_preview, now_ms) &&
                seek_preview.surface &&
                seek_preview.decoded_chunk_index != previous_preview_chunk) {
                hover_preview_needs_rebuffer = true;
            } else if (!allow_seek_preview) {
                seek_preview.over_bar = false;
                seek_preview.tracking = false;
                seek_preview.last_pointer_x = -1;
            }
        } else {
            seek_preview.over_bar = false;
        }
        if (seek_left_edge || seek_right_edge) {
            int64_t next_seek_ms = (int64_t) pending_seek_ms + (seek_left_edge ? -SEEK_STEP_MS : SEEK_STEP_MS);
            int64_t seek_limit_ms = (int64_t) movie_duration_ms(&movie);

            if (!paused) {
                paused = true;
            }
            if (next_seek_ms < -seek_limit_ms) {
                next_seek_ms = -seek_limit_ms;
            }
            if (next_seek_ms > seek_limit_ms) {
                next_seek_ms = seek_limit_ms;
            }
            pending_seek_ms = (int32_t) next_seek_ms;
            pending_seek_commit_at_ms = now_ms + SEEK_STACK_DELAY_MS;
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (pending_seek_ms != 0 &&
            !seek_left_edge &&
            !seek_right_edge &&
            (now_ms >= pending_seek_commit_at_ms || enter_edge || tab_edge || pointer_click)) {
            pending_seek_consumed_click = pointer_click;
            if (!clamp_seek(&movie, pending_seek_ms)) {
                report_movie_decode_failure(&movie, path, "seek");
                result = -1;
                break;
            }
            begin_seek_preroll(&movie, &seek_preroll_active, &seek_preroll_started_ms, &seek_preroll_target_ready_count);
            hover_preview_needs_rebuffer = false;
            pending_seek_ms = 0;
            pending_seek_commit_at_ms = 0;
            if (!seek_preroll_active) {
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            }
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (pending_seek_consumed_click) {
            pointer_click = false;
        }
        if (screenshot_edge) {
            take_screenshot = true;
        }
        if (cat_edge) {
            if (help_menu_open) {
                help_menu_open = false;
                if (help_resume_playback) {
                    paused = false;
                    if (hover_preview_needs_rebuffer) {
                        begin_seek_preroll(&movie, &seek_preroll_active, &seek_preroll_started_ms, &seek_preroll_target_ready_count);
                        hover_preview_needs_rebuffer = false;
                    }
                }
                help_resume_playback = false;
            } else {
                help_menu_open = true;
                help_resume_playback = !paused;
                paused = true;
            }
            if (!seek_preroll_active) {
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            }
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            show_ui = true;
        }
        if (key_pressed_edge(KEY_NSPIRE_ESC, &prev_esc)) {
            if (help_menu_open) {
                help_menu_open = false;
                if (help_resume_playback) {
                    paused = false;
                    if (hover_preview_needs_rebuffer) {
                        begin_seek_preroll(&movie, &seek_preroll_active, &seek_preroll_started_ms, &seek_preroll_target_ready_count);
                        hover_preview_needs_rebuffer = false;
                    }
                }
                help_resume_playback = false;
                if (!seek_preroll_active) {
                    reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                }
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
                &subtitle_cache,
                subtitle_font_index,
                false,
                subtitle_size,
                subtitle_placement,
                (now_ms <= status_overlay_until) ? status_overlay_text : NULL,
                &screenshot_preview,
                &seek_preview,
                now_ms,
                &pointer,
                0
            );
            if (take_screenshot) {
                char saved_path[MAX_PATH_LEN];
                if (save_screenshot_bitmap(screen, path, saved_path, sizeof(saved_path))) {
                    prepare_screenshot_preview(&screenshot_preview, screen, saved_path);
                }
            }
            flush_deferred_history_save(
                path,
                &movie,
                &history_save_pending,
                "help",
                false,
                scale_mode,
                playback_rate_index,
                subtitle_font_index,
                subtitle_size,
                subtitle_placement
            );
            prefetch_tick(&movie, true, 1000);
            msleep(16);
            continue;
        }
        if (enter_edge) {
            if (movie.current_frame + 1 >= movie.header.frame_count) {
                if (!decode_to_frame(&movie, 0)) {
                    report_movie_decode_failure(&movie, path, "restart");
                    result = -1;
                    break;
                }
                hover_preview_needs_rebuffer = false;
                paused = false;
            } else {
                paused = !paused;
            }
            if (!paused && hover_preview_needs_rebuffer) {
                begin_seek_preroll(&movie, &seek_preroll_active, &seek_preroll_started_ms, &seek_preroll_target_ready_count);
                hover_preview_needs_rebuffer = false;
            }
            if (!seek_preroll_active) {
                reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            }
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
            subtitle_placement = subtitle_cycle_placement(
                subtitle_placement,
                selected_subtitle_track_supports_auto_positioning(&movie)
            );
            snprintf(
                status_overlay_text,
                sizeof(status_overlay_text),
                "SUB POS %s",
                subtitle_placement_label(subtitle_placement)
            );
            status_overlay_until = now_ms + STATUS_OVERLAY_MS;
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
            subtitle_placement = subtitle_normalize_placement(
                subtitle_placement,
                selected_subtitle_track_supports_auto_positioning(&movie)
            );
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
            debug_set_metrics_collection(
                memory_overlay_mode != MEMORY_OVERLAY_OFF || debug_is_runtime_logging_enabled()
            );
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (debug_logging_edge) {
            if (debug_is_runtime_logging_enabled()) {
                debug_set_runtime_logging(false);
                snprintf(status_overlay_text, sizeof(status_overlay_text), "DEBUG LOG OFF");
            } else {
                debug_set_runtime_logging(true);
                snprintf(
                    status_overlay_text,
                    sizeof(status_overlay_text),
                    "%s",
                    debug_is_runtime_logging_enabled() ? "DEBUG LOG ON" : "DEBUG LOG NO RAM"
                );
                if (debug_is_runtime_logging_enabled()) {
                    debug_tracef(
                        "debug logging enabled frame=%lu chunk=%d",
                        (unsigned long) movie.current_frame,
                        movie.loaded_chunk
                    );
                    debug_log_sram_status();
                }
            }
            debug_set_metrics_collection(
                memory_overlay_mode != MEMORY_OVERLAY_OFF || debug_is_runtime_logging_enabled()
            );
            status_overlay_until = now_ms + STATUS_OVERLAY_MS;
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (tab_edge) {
            if (!paused) {
                paused = true;
            } else if (!step_movie_forward_one_frame(&movie, &hover_preview_needs_rebuffer)) {
                report_movie_decode_failure(&movie, path, "tab step");
                result = -1;
                break;
            }
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (tab_repeat_step && paused) {
            if (!step_movie_forward_one_frame(&movie, &hover_preview_needs_rebuffer)) {
                report_movie_decode_failure(&movie, path, "tab hold step");
                result = -1;
                break;
            }
            reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
            ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
        }
        if (pointer_click) {
            SDL_Rect bar = progress_bar_rect();
            if (show_ui && pointer.y >= SCREEN_H - UI_BAR_H && pointer.y < SCREEN_H) {
                int seek_x = clamp_int(pointer.x, bar.x, bar.x + bar.w);
                if (!seek_to_ratio(&movie, (uint32_t) (seek_x - bar.x), (uint32_t) bar.w)) {
                    report_movie_decode_failure(&movie, path, "pointer seek");
                    result = -1;
                    break;
                }
                begin_seek_preroll(&movie, &seek_preroll_active, &seek_preroll_started_ms, &seek_preroll_target_ready_count);
                hover_preview_needs_rebuffer = false;
                if (!seek_preroll_active) {
                    reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                }
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            } else {
                if (movie.current_frame + 1 >= movie.header.frame_count) {
                    if (!decode_to_frame(&movie, 0)) {
                        report_movie_decode_failure(&movie, path, "pointer restart");
                        result = -1;
                        break;
                    }
                    hover_preview_needs_rebuffer = false;
                    paused = false;
                } else {
                    paused = !paused;
                }
                if (!paused && hover_preview_needs_rebuffer) {
                    begin_seek_preroll(&movie, &seek_preroll_active, &seek_preroll_started_ms, &seek_preroll_target_ready_count);
                    hover_preview_needs_rebuffer = false;
                }
                if (!seek_preroll_active) {
                    reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                }
                ui_visible_until = now_ms + POINTER_UI_TIMEOUT_MS;
            }
        }
        maybe_defer_history_save(&movie, &last_history_saved_ms, &history_save_pending);
        if (seek_preroll_active) {
            size_t ready_frames = h264_frame_ring_contiguous_ready_count(&movie);
            if (ready_frames >= seek_preroll_target_ready_count ||
                (int32_t) (now_ms - seek_preroll_started_ms) >= (int32_t) SEEK_PREROLL_TIMEOUT_MS) {
                seek_preroll_active = false;
                seek_preroll_started_ms = 0;
                seek_preroll_target_ready_count = 0;
                if (!paused) {
                    reset_playback_timeline(&movie, playback_rate, &playback_anchor_ticks, &playback_anchor_frame, &next_frame_due_ticks);
                }
            }
        }
        if (!paused && frame_interval_ticks > 0 && !seek_preroll_active) {
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
                        if (debug_should_collect_metrics()) {
                            movie.diag_foreground_decode_count++;
                        }
                        record_h264_foreground_decode_time(&movie, decode_elapsed_ms);
                        if (debug_is_runtime_logging_enabled() &&
                            (decode_elapsed_ms >= DEBUG_TRACE_FOREGROUND_MS || lagged)) {
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
                            movie.h264_active_prefetch_backoff = H264_ACTIVE_PREFETCH_BACKOFF_LIGHT;
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
        {
            uint32_t render_now_ms = monotonic_clock_now_ms();
            bool subtitle_font_overlay_visible = render_now_ms <= subtitle_font_overlay_until;

            show_ui = paused || (render_now_ms <= ui_visible_until);
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
                &subtitle_cache,
                subtitle_font_index,
                subtitle_font_overlay_visible,
                subtitle_size,
                subtitle_placement,
                (render_now_ms <= status_overlay_until) ? status_overlay_text : NULL,
                &screenshot_preview,
                &seek_preview,
                render_now_ms,
                &pointer,
                pending_seek_ms
            );
            if (take_screenshot) {
                char saved_path[MAX_PATH_LEN];
                if (save_screenshot_bitmap(screen, path, saved_path, sizeof(saved_path))) {
                    prepare_screenshot_preview(&screenshot_preview, screen, saved_path);
                }
            }
        }
        if (paused || frame_interval_ticks == 0 || seek_preroll_active) {
            flush_deferred_history_save(
                path,
                &movie,
                &history_save_pending,
                "paused",
                false,
                scale_mode,
                playback_rate_index,
                subtitle_font_index,
                subtitle_size,
                subtitle_placement
            );
            prefetch_tick(&movie, true, 1000);
            if (debug_is_runtime_logging_enabled() &&
                now_ms - movie.diag_last_snapshot_ms >= DEBUG_SNAPSHOT_INTERVAL_MS) {
                movie.diag_last_snapshot_ms = now_ms;
                debug_trace_runtime_snapshot(&movie, true, 1000U, playback_rate, "paused");
            }
            msleep(16);
        } else {
            uint64_t after_render_ticks = monotonic_clock_now_ticks();
            uint64_t spare_ticks = next_frame_due_ticks > after_render_ticks ? (next_frame_due_ticks - after_render_ticks) : 0;
            uint32_t spare_ms = monotonic_clock_ticks_to_ms(spare_ticks);
            uint64_t wait_target_ticks = next_frame_due_ticks;
            if (debug_should_collect_metrics()) {
                if (spare_ms > movie.diag_max_spare_ms) {
                    movie.diag_max_spare_ms = spare_ms;
                }
                movie.diag_last_spare_ms = spare_ms;
            }
            prefetch_tick(&movie, false, spare_ms);
            if (debug_is_runtime_logging_enabled() &&
                now_ms - movie.diag_last_snapshot_ms >= DEBUG_SNAPSHOT_INTERVAL_MS) {
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

    flush_deferred_history_save(
        path,
        &movie,
        &history_save_pending,
        "exit",
        true,
        scale_mode,
        playback_rate_index,
        subtitle_font_index,
        subtitle_size,
        subtitle_placement
    );
    if (debug_is_runtime_logging_enabled() || result != 0) {
        char log_path[MAX_PATH_LEN];
        debug_log_path_for_movie(path, log_path, sizeof(log_path));
        debug_dump_session(log_path, &movie, result == 0 ? "normal-exit" : "aborted");
    }
    free_subtitle_surface_cache(&subtitle_cache);
    clear_screenshot_preview(&screenshot_preview);
    clear_seek_bar_preview(&seek_preview);
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
    sram_init();
    h264bsdInitSramTables();
    init_h264_color_tables();

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
    sram_shutdown();
    monotonic_clock_shutdown();
    return result == 0 ? 0 : 1;
}
