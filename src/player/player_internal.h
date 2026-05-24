#ifndef NDVIDEO_PLAYER_INTERNAL_H
#define NDVIDEO_PLAYER_INTERNAL_H

#include <stdbool.h>
#include <dirent.h>
#include <libndls.h>
#include <os.h>
#include <SDL/SDL.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "codecs/h264bsd/h264bsd_decoder.h"
#include "codecs/h264bsd/h264bsd_sram.h"
#include "codecs/h264bsd/h264bsd_util.h"
#include "codecs/mpeg4_xvid.h"
#include "movie/movie.h"
#include "sram.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define UI_CHROME_VISUAL_X_OFFSET 0
#define UI_CHROME_CENTER_X ((SCREEN_W / 2) + UI_CHROME_VISUAL_X_OFFSET)
#define UI_SOFT_PANEL_RIGHT_PERCEIVED_EDGE_INSET 1
#define UI_BAR_H 28
#define SEEK_STEP_MS 5000
#define SEEK_STACK_DELAY_MS 450U
#define SEEK_BADGE_ANIM_MS 120U
#define SEEK_BADGE_EXIT_ANIM_MS 105U
#define SEEK_BADGE_HIDE_PENDING UINT32_MAX
#define STATUS_BADGE_ANIM_MS 115U
#define STATUS_BADGE_EXIT_ANIM_MS 95U
#define TAB_HOLD_FRAME_REPEAT_DELAY_MS 250U
#define TAB_HOLD_FRAME_REPEAT_FALLBACK_INTERVAL_MS 80U
#define PICKER_MAX_FILES 128
#define PICKER_VISIBLE_ROWS 8
#define PICKER_TOOLTIP_DWELL_MS 450U
#define PICKER_SELECTION_ANIM_MS 155U
#define PICKER_DESELECTION_ANIM_MS 90U
#define PICKER_INTRO_ANIM_MS 360U
#define PICKER_INTRO_ROW_STAGGER_MS 28U
#define PICKER_INTRO_LAST_ROW_DELAY_MS (115U + ((uint32_t) (PICKER_VISIBLE_ROWS - 1U) * PICKER_INTRO_ROW_STAGGER_MS))
#define PICKER_INTRO_TOTAL_ANIM_MS (PICKER_INTRO_LAST_ROW_DELAY_MS + (PICKER_INTRO_ANIM_MS - 120U))
#define PICKER_EXIT_START_DELAY_DIVISOR 8U
#define PICKER_EXIT_TOTAL_ANIM_MS 430U
#define PICKER_EXIT_TO_LOADING_ANIM_MS PICKER_EXIT_TOTAL_ANIM_MS
#define PICKER_EXIT_LOADING_DELAY_MS PICKER_EXIT_TO_LOADING_ANIM_MS
#define PICKER_EXIT_INACTIVE UINT32_MAX
#define PICKER_PRESS_RELEASE_ANIM_MS 94U
#define PICKER_HOVER_SCROLL_EDGE_PX 18
#define PICKER_HOVER_SCROLL_REPEAT_MS 145U
#define PICKER_SCROLL_ANIM_MS 118U
#define PICKER_ROW_STEP_PX 20
#define PROMPT_BUTTON_ANIM_MS 90U
#define UI_CHROME_ANIM_MS 115U
#define UI_HOVER_ANIM_MS 105U
#define UI_TITLE_ANIM_MS 135U
#define UI_PRESS_ANIM_MS 120U
#define UI_PRESS_RELEASE_ANIM_MS 84U
#define UI_PRESS_PRIME_MIX 72U
#define UI_PRESS_RELEASE_VISIBLE_MIX 255U
#define UI_TOOLTIP_ANIM_MS 115U
#define UI_MENU_ANIM_MS 150U
#define UI_LOADING_ANIM_MS 150U
#define UI_LOADING_PROGRESS_FRAME_MS 90U
#define UI_LOADING_DIM_ALPHA 112
#define RESUME_PROMPT_DIM_ALPHA 112
#define UI_RETURN_COLLAPSE_ANIM_MS 210U
#define RESUME_PROMPT_OPEN_MORPH_ANIM_MS 260U
#define RESUME_PROMPT_CONTENT_START_MS 130U
#define RESUME_PROMPT_CONTENT_STAGGER_MS 48U
#define RESUME_PROMPT_CONTENT_ITEM_ANIM_MS 150U
#define RESUME_PROMPT_LOADING_TEXT_EXIT_MS 120U
#define RESUME_PROMPT_ANIM_MS 150U
#define RESUME_COMMIT_PROMPT_ANIM_MS 190U
#define RESUME_MORPH_ANIM_MS 150U
#define RESUME_COMMIT_MORPH_ANIM_MS 230U
#define SCALE_MORPH_ANIM_MS RESUME_MORPH_ANIM_MS
#define RESUME_INPUT_GUARD_MS 260U
#define UI_WAKE_MIN_MIX 28U
#define UI_PAUSE_QUIET_MS (UI_PRESS_ANIM_MS + UI_PRESS_RELEASE_ANIM_MS + 24U)
#define PLAYBACK_INPUT_PREFETCH_GRACE_MS 80U
#define MAX_PATH_LEN 512
#define MAX_SUBTITLE_LINES 3
#define MAX_SUBTITLE_LINE_LEN 96
#define APP_RAM_TARGET_BYTES (32U * 1024U * 1024U)
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
#define POINTER_HOVER_REARM_PIXELS 8
#define PLAYBACK_TITLE_TOP_EDGE_PX 1
#define PREFETCH_FILE_BLOCK_SIZE 32768U
#define PREFETCH_PAUSED_SLICE_MS 12U
#define PREFETCH_ACTIVE_H264_MIN_SPARE_MS 12U
#define PREFETCH_ACTIVE_H264_SLICE_MS 8U
#define H264_PREFETCH_NEXT_CHUNK_GUARD_FRAMES 12U
#define H264_PREFETCH_IO_PRIORITY_SLICE_MS 4U
#define H264_PREFETCH_NEXT_CHUNK_IO_CATCHUP_FRAMES 20U
#define H264_PREFETCH_SECOND_NEXT_CHUNK_WINDOW_FRAMES 32U
#define H264_FOREGROUND_DECODE_SOFT_MS 35U
#define H264_FOREGROUND_DECODE_HARD_MS 50U
#define FRAME_PACING_SPIN_MS 2U
#define MONOTONIC_TIMER_VALUE_ADDR 0x900C0004U
#define MONOTONIC_TIMER_CONTROL_ADDR 0x900C0008U
#define MONOTONIC_TIMER_CLOCK_SOURCE_ADDR 0x900C0080U
#define MONOTONIC_TIMER_CLOCK_SOURCE_32768HZ 0x0AU
#define MONOTONIC_TIMER_CONTROL_ENABLE_32BIT 0x82U
#define MONOTONIC_TIMER_MAX_DELTA_TICKS (TIMER_TICKS_PER_SEC * 10U)
#define LCD_BRIGHTNESS_CX2_ADDR ((volatile uint32_t *) 0x90130014U)
#define LCD_BRIGHTNESS_CX_ADDR ((volatile uint32_t *) 0x900F0020U)
#define LCD_BRIGHTNESS_MIN 0
#define LCD_BRIGHTNESS_MAX 255
/* CX uses 0x00..0xFF for visible backlight levels; 0x100 blanks it. */
#define LCD_BRIGHTNESS_CX_LEVEL_MIN 0U
#define LCD_BRIGHTNESS_CX_LEVEL_MAX 0xFFU
#define LCD_BRIGHTNESS_CX_BACKLIGHT_OFF 0x100U
#define LCD_BRIGHTNESS_LOWEST_NORMAL 252
#define LCD_BRIGHTNESS_STEP 25
#define LCD_BRIGHTNESS_FADE_MS 160U
#define DISPLAY_IDLE_DIM_START_MS 60000U
#define DISPLAY_IDLE_DIM_OFF_MS 300000U
#define DEBUG_RING_SIZE 8192
#define DEBUG_LINE_LEN 192
#define DEBUG_SNAPSHOT_INTERVAL_MS 1000U
#define DEBUG_TRACE_FOREGROUND_MS 12U
#define DEBUG_TRACE_PREFETCH_MS 10U
#define DEBUG_FPS_MIN_SAMPLE_MS 250U
#define DEBUG_FPS_WINDOW_MS 1000U
#define DEBUG_FPS_IDLE_RESET_MS 2000U
#define HISTORY_FILE_NAME "ndhistory.ts.tns"
#define HISTORY_MAX_ENTRIES 5
#define HISTORY_MAGIC_V1 "NDVH1"
#define HISTORY_MAGIC_V2 "NDVH2"
#define HISTORY_MAGIC_V3 "NDVH3"
#define HISTORY_MAGIC_V4 "NDVH4"
#define HISTORY_MAGIC_V5 "NDVH5"
#define HISTORY_MAGIC_V6 "NDVH6"
#define RESUME_MIN_MS 5000U
#define RESUME_CLEAR_TAIL_MS 3000U
#define STATUS_OVERLAY_MS 1200U
#define SCREENSHOT_PREVIEW_MS 1200U
#define SEEK_BAR_PREVIEW_IO_BLOCK_SIZE 8192U
#define SEEK_BAR_PREVIEW_SLICE_MS 8U
#define SEEK_BAR_PREVIEW_DEBOUNCE_MS 250U
#define H264_INCREMENTAL_DECODE_MIN_SPARE_MS 1U
#define H264_INCREMENTAL_DECODE_BUDGET_GUARD_MS 1U
#define H264_INCREMENTAL_DECODE_DEFAULT_MBS_PER_MS_Q8 640U
#define SCREENSHOT_PREVIEW_MAX_W 96
#define SCREENSHOT_PREVIEW_MAX_H 72
#define SEEK_BAR_PREVIEW_MAX_W 80
#define SEEK_BAR_PREVIEW_MAX_H 60
#define SUBTITLE_COORD_SCALE 10000U
#define H264_CLIP_OFFSET 384
#define H264_CLIP_TABLE_SIZE 1024
#define SRAM_MOVIE_CHUNK_BUFFER_BYTES (112U * 1024U)
#define MPEG4_XVID_SRAM_POOL_BYTES (48U * 1024U)

typedef struct {
    char *name;
    char *detail;
    char *path;
    uint32_t resume_frame;
    uint32_t resume_ms;
    uint32_t duration_ms;
    bool has_resume;
    bool resume_time_known;
} MovieFile;

typedef struct {
    MovieFile *files;
    size_t count;
    size_t selected_index;
    size_t scroll_start;
    char directory[MAX_PATH_LEN];
    bool has_position;
    bool valid;
} MoviePickerCache;

typedef struct {
    char *path;
    uint32_t frame;
    bool has_resume;
    bool realtime_frame_skip;
    uint8_t scale_mode;
    uint8_t playback_rate_index;
    uint8_t playback_mode;
    uint8_t subtitle_font_index;
    int8_t subtitle_size;
    uint8_t subtitle_placement;
    uint16_t selected_subtitle_track;
    int8_t video_align_x;
    int8_t video_align_y;
} HistoryEntry;

typedef enum {
    UI_THEME_DORFIC = 0,
    UI_THEME_BLUE,
    UI_THEME_GREEN,
    UI_THEME_RED,
    UI_THEME_COUNT
} UiThemeId;

typedef struct {
    HistoryEntry entries[HISTORY_MAX_ENTRIES];
    size_t count;
    HistoryEntry default_settings;
    bool has_default_settings;
    UiThemeId theme_id;
} HistoryStore;

typedef struct {
    bool off;
    bool off_from_idle;
    bool idle_dim_active;
    bool resume_playback_on_wake;
    uint32_t saved_brightness;
    uint32_t idle_base_brightness;
    uint32_t last_activity_ms;
} DisplayPowerState;

typedef struct {
    nSDL_Font *white;
    nSDL_Font *outline;
    nSDL_Font *subtitle_white[NSP_NUMFONTS];
    nSDL_Font *subtitle_outline[NSP_NUMFONTS];
} Fonts;

typedef struct {
    SDL_Surface *screen;
    SDL_Surface *snapshot;
    const Fonts *fonts;
    const char *label;
    uint32_t started_ms;
    uint32_t last_draw_ms;
    bool dim_background;
} LoadingProgress;

typedef enum {
    SCALE_FIT = 0,
    SCALE_FILL = 1,
    SCALE_STRETCH = 2,
    SCALE_NATIVE = 3,
} ScaleMode;

typedef enum {
    VIDEO_ALIGN_NEGATIVE = -1,
    VIDEO_ALIGN_CENTER = 0,
    VIDEO_ALIGN_POSITIVE = 1,
} VideoAlign;

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
    PLAYBACK_MODE_ONCE = 0,
    PLAYBACK_MODE_REPEAT,
    PLAYBACK_MODE_AUTO_NEXT,
    PLAYBACK_MODE_COUNT,
} PlaybackMode;

typedef enum {
    MEMORY_OVERLAY_OFF = 0,
    MEMORY_OVERLAY_ALWAYS,
} MemoryOverlayMode;

typedef struct {
    char movie_path[MAX_PATH_LEN];
    MovieHeader header;
    uint32_t current_frame;
    uint16_t selected_subtitle_track;
    ScaleMode scale_mode;
    size_t playback_rate_index;
    PlaybackMode playback_mode;
    bool realtime_frame_skip;
    size_t subtitle_font_index;
    int subtitle_size;
    SubtitlePlacement subtitle_placement;
    VideoAlign video_align_x;
    VideoAlign video_align_y;
    bool pending;
} DeferredHistorySave;

typedef enum {
    PLAY_MOVIE_RESULT_ERROR = -1,
    PLAY_MOVIE_RESULT_EXIT = 0,
    PLAY_MOVIE_RESULT_AUTO_NEXT = 1,
    PLAY_MOVIE_RESULT_APP_EXIT = 2,
    PLAY_MOVIE_RESULT_HOME_EXIT = 3,
    PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT = 4,
    PLAY_MOVIE_RESULT_SWITCH_MOVIE = 5,
} PlayMovieResult;

enum {
    RESUME_PROMPT_RESULT_HOME_EXIT = -2,
    RESUME_PROMPT_RESULT_SCRATCHPAD_EXIT = -3,
};

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
    bool press_edge;
    bool release_edge;
    bool tracking;
    bool moved;
} PointerState;

typedef struct {
    bool esc;
    bool enter;
    bool space;
    bool tab;
    bool cat;
    bool scratchpad;
    bool keypad_1;
    bool keypad_2;
    bool keypad_3;
    bool keypad_4;
    bool keypad_5;
    bool keypad_6;
    bool keypad_7;
    bool keypad_8;
    bool keypad_9;
    bool left;
    bool right;
    bool up;
    bool down;
    bool divide;
    bool exp;
    bool tenx;
    bool lp;
    bool rp;
    bool lthan;
    bool gthan;
    bool plus;
    bool minus;
    bool f;
    bool t;
    bool m;
    bool d;
    bool s;
    bool c;
    bool p;
    bool r;
    bool on;
} PlaybackKeySnapshot;

typedef struct {
    bool locked;
    int anchor_x;
    int anchor_y;
} PointerHoverGuard;

typedef struct {
    int row_index;
    uint32_t started_ms;
    bool armed;
} PickerTooltipHoverState;

typedef struct {
    size_t from_start;
    size_t to_start;
    uint32_t started_ms;
} PickerScrollAnim;

typedef struct {
    bool initialized;
    bool target_active;
    uint8_t start_mix;
    uint8_t current_mix;
    uint32_t started_ms;
} UiTransition;

typedef enum {
    PLAYBACK_PRESS_NONE = 0,
    PLAYBACK_PRESS_PLAY,
    PLAYBACK_PRESS_SCALE,
    PLAYBACK_PRESS_SPEED,
} PlaybackPressTarget;

typedef struct {
    UiTransition chrome;
    UiTransition playback_badge;
    UiTransition playback_press;
    UiTransition scale_badge;
    UiTransition scale_press;
    UiTransition speed_badge;
    UiTransition speed_press;
    UiTransition seek_preview;
    UiTransition title_strip;
    UiTransition help_menu;
} PlaybackUiTransitions;

typedef struct {
    uint8_t chrome;
    uint8_t playback_badge;
    uint8_t playback_press;
    uint8_t scale_badge;
    uint8_t scale_press;
    uint8_t speed_badge;
    uint8_t speed_press;
    uint8_t seek_preview;
    uint8_t title_strip;
    uint8_t help_menu;
} PlaybackUiMixes;

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
    uint8_t overlay_mix;
} SubtitleLayoutSpec;

typedef struct {
    SDL_Surface *surface;
    char label[96];
    uint32_t until_ms;
} ScreenshotPreviewState;

typedef struct {
    storage_t *decoder;
    bool decoder_initialized;
    bool active;
    bool complete;
    int chunk_index;
    uint32_t target_frame;
    uint32_t next_frame;
    uint8_t *chunk_storage;
    size_t chunk_storage_size;
    size_t read_offset;
    uint32_t *frame_offsets;
    uint8_t *chunk_bytes;
    size_t chunk_size;
    size_t consumed_bytes;
    unsigned zero_advance_retries;
    uint16_t *pixels;
    uint16_t avg_mbs_per_ms_q8;
} SeekPreviewDecodeJob;

typedef struct {
    SDL_Surface *surface;
    int decoded_chunk_index;
    uint32_t decoded_frame_index;
    int marker_x;
    uint32_t hover_ms;
    uint32_t last_move_ms;
    uint32_t surface_started_ms;
    int last_pointer_x;
    int suppress_marker_x;
    uint32_t suppress_frame_index;
    bool surface_fade_pending;
    bool surface_render_pending;
    bool suppress_until_pointer_moves;
    bool tracking;
    bool over_bar;
    SeekPreviewDecodeJob decode_job;
} SeekBarPreviewState;

typedef bool (*H264FramePublishPredicate)(Movie *movie, uint32_t frame_index, void *userdata);
typedef bool (*H264DecodedFrameHook)(Movie *movie, uint32_t frame_index, void *userdata);

typedef struct {
    bool active;
    SDL_Rect from_src;
    SDL_Rect from_dst;
    SDL_Rect to_src;
    SDL_Rect to_dst;
    uint32_t started_ms;
} ScaleMorphState;

typedef struct {
    SDL_Surface *screen;
    const Fonts *fonts;
    bool paused;
    bool show_ui;
    ScaleMode scale_mode;
    ScaleMorphState *scale_morph;
    VideoAlign video_align_x;
    VideoAlign video_align_y;
    const PlaybackRate *playback_rate;
    MemoryOverlayMode memory_overlay_mode;
    SubtitleSurfaceCache *subtitle_cache;
    size_t subtitle_font_index;
    bool subtitle_font_overlay_visible;
    int subtitle_size;
    SubtitlePlacement subtitle_placement;
    const char *movie_title_text;
    const char *movie_detail_text;
    const char *status_overlay_text;
    uint32_t status_overlay_started_ms;
    uint32_t status_overlay_until_ms;
    const ScreenshotPreviewState *screenshot_preview;
    SeekBarPreviewState *seek_preview;
    PointerState *pointer;
    int32_t pending_seek_ms;
    int32_t seek_badge_ms;
    uint32_t seek_badge_started_ms;
    uint32_t seek_badge_hide_elapsed_ms;
    PlaybackUiTransitions *ui_transitions;
    PlaybackUiMixes *ui_mixes;
    PlaybackPressTarget playback_press_target;
    bool playback_press_active;
    bool scale_press_active;
    bool speed_press_active;
    bool title_strip_active;
    PlaybackKeySnapshot abort_key_snapshot;
    bool abort_on_input;
    bool abort_requested;
    uint32_t target_frame;
} CommittedSeekRenderContext;

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


#define UI_RGB565(r, g, b) ((Uint16) (((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))))

enum {
    UI_COLOR_BLACK = UI_RGB565(0, 0, 0),
    UI_COLOR_WHITE = UI_RGB565(255, 255, 255),
    UI_CURSOR_KEY = UI_RGB565(255, 0, 255)
};

typedef struct {
    const char *name;
    Uint16 accent_hot;
    Uint16 accent_mid;
    Uint16 accent_deep;
    Uint16 bg_top;
    Uint16 bg_bottom;
    Uint16 carbon;
    Uint16 gunmetal;
    Uint16 warm_white;
    Uint16 shortcut_base;
    Uint16 shortcut_glint;
    Uint16 tooltip_base;
    Uint16 resume_hover;
    Uint16 row_selected;
    Uint16 row_divider;
    Uint16 footer_panel;
    Uint16 footer_accent_top;
    Uint16 scroll_track_top;
    Uint16 scroll_track_bottom;
    Uint16 scroll_thumb;
    Uint16 help_key;
    Uint16 help_key_cut;
    Uint16 menu_border;
    Uint16 menu_panel;
    Uint16 modal_panel;
    Uint16 modal_cut;
    Uint16 modal_title_panel;
    Uint16 progress_track_top;
    Uint16 progress_track_bottom;
    Uint16 progress_cap_top;
    Uint16 progress_cap_bottom;
    Uint16 progress_edge_top;
    Uint16 progress_edge_bottom;
    Uint16 buffer_top;
    Uint16 buffer_bottom;
    Uint16 buffer_separator;
    Uint16 progress_fill_top;
    Uint16 progress_fill_bottom;
    Uint16 progress_fill_glow;
    Uint16 progress_fill_cap;
    Uint16 progress_overlay_base;
    Uint16 preview_outer;
    Uint16 surface_outer;
    Uint16 cursor_pale;
    Uint16 cursor_mid;
    Uint16 cursor_deep;
    Uint16 cursor_dark;
    Uint16 cursor_shadow;
} UiThemePalette;

#define PLAYBACK_RATE_DEFAULT_INDEX 3U
#define PLAYBACK_RATE_COUNT 8U
#define SUBTITLE_FONT_DEFAULT_INDEX 2U
#define SUBTITLE_FONT_CHOICE_COUNT 5U
#define SUBTITLE_FONT_OVERLAY_MS 1200U

extern MonotonicClock g_clock;
extern char (*g_debug_ring)[DEBUG_LINE_LEN];
extern size_t g_debug_ring_count;
extern size_t g_debug_ring_next;
extern char g_last_error_message[DEBUG_LINE_LEN];
extern bool g_debug_logging_enabled;
extern bool g_debug_metrics_enabled;
extern H264ColorTables g_h264_color_tables_storage;
extern H264ColorTables *g_h264_color_tables;
extern bool g_h264_color_tables_in_sram;
extern uint8_t *g_sram_movie_chunk_buffer;
extern size_t g_sram_movie_chunk_buffer_size;
extern Movie *g_deferred_playback_movie;
extern MoviePickerCache g_picker_cache;
extern DeferredHistorySave g_pending_history_save;
extern DisplayPowerState g_display_power_state;
extern char g_pending_theme_directory[MAX_PATH_LEN];
extern bool g_pending_theme_save;
extern const PlaybackRate g_playback_rates[PLAYBACK_RATE_COUNT];
extern const int g_subtitle_font_choices[SUBTITLE_FONT_CHOICE_COUNT];
extern const char *g_subtitle_font_names[SUBTITLE_FONT_CHOICE_COUNT];
extern UiThemeId g_ui_theme_id;

static inline uint8_t h264_clip_byte(int32_t value)
{
    /* The YUV->RGB fixed-point path keeps this in [-258, 534]. */
    return g_h264_color_tables->clip[value + H264_CLIP_OFFSET];
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

/* Cross-module declarations for the player translation units. */
/* movie_open_scan.c */
void return_to_os_home_menu(void);
void yes_teacher_im_mathing(void);
bool key_pressed_edge(t_key key, bool *previous_state);
bool on_key_pressed_edge(bool *previous_state);
bool load_movie(const char *path, Movie *movie, LoadingProgress *loading_progress);
void ensure_movie_picker_cache(MoviePickerCache *cache, const char *directory);
bool find_next_movie_path(const char *current_path, char *next_path, size_t next_path_size);
bool find_previous_movie_path(const char *current_path, char *previous_path, size_t previous_path_size);
const SubtitleCue *active_subtitle_cue(const Movie *movie, uint32_t now_ms);

/* codec_streaming.c */
const PrefetchedChunk *find_prefetched_chunk_const(const Movie *movie, int chunk_index);
uint32_t h264_frame_size_from_offsets(const uint32_t *frame_offsets, uint32_t frame_count, size_t chunk_size, uint32_t local_index);
uint32_t h264_frame_size_from_chunk_storage(const Movie *movie, const ChunkIndexEntry *entry, const uint8_t *chunk_storage, size_t chunk_storage_size, uint32_t local_index);
uint32_t estimate_h264_chunk_average_frame_bytes(const Movie *movie, const ChunkIndexEntry *entry);
uint32_t estimate_h264_frame_bytes(const Movie *movie, uint32_t frame_index);
bool reset_h264_storage_decoder(storage_t *decoder, bool *initialized);
bool read_h264_picture_params( const Movie *movie, storage_t *decoder, uint32_t *full_width, uint32_t *full_height, uint32_t *crop_left, uint32_t *crop_top, uint32_t *crop_width, uint32_t *crop_height );
void store_h264_picture_params( Movie *movie, uint32_t full_width, uint32_t full_height, uint32_t crop_left, uint32_t crop_top, uint32_t crop_width, uint32_t crop_height );
bool sync_h264_picture_params(Movie *movie, storage_t *decoder, bool force_commit);
bool reset_h264_decoder(Movie *movie);
bool reset_mpeg4_decoder(Movie *movie);
bool blit_h264_planes_to_rgb565_target_with_crop( const Movie *movie, const uint8_t *restrict y_plane, const uint8_t *restrict u_plane, const uint8_t *restrict v_plane, size_t luma_stride, size_t chroma_stride, uint16_t *restrict dst_pixels, size_t dst_pitch_pixels, size_t crop_width, size_t crop_height );
bool blit_h264_planes_to_rgb565_target( const Movie *movie, const uint8_t *restrict y_plane, const uint8_t *restrict u_plane, const uint8_t *restrict v_plane, size_t luma_stride, size_t chroma_stride, uint16_t *restrict dst_pixels, size_t dst_pitch_pixels );
bool blit_h264_picture_to_target( Movie *movie, const uint8_t *picture, uint16_t *dst_pixels, size_t dst_pitch_pixels );
uint8_t *take_h264_output_picture(storage_t *decoder, const char *context);
bool pump_h264_access_unit( Movie *movie, storage_t *decoder, uint8_t *frame_data, size_t frame_size, size_t *inout_consumed, unsigned *inout_zero_advance_retries, uint32_t macroblock_budget, bool force_commit_picture_params, const char *context, bool *out_picture_ready, bool *out_pending, uint8_t **out_picture );
bool decode_h264_access_unit_to_target( Movie *movie, uint8_t *frame_data, size_t frame_size, uint16_t *dst_pixels, size_t dst_pitch_pixels );
bool decode_h264_access_unit( Movie *movie, uint8_t *frame_data, size_t frame_size, bool blit_output );
bool configure_chunk_view_from_storage( const Movie *movie, int chunk_index, const uint8_t *chunk_storage, size_t chunk_storage_size, uint32_t **out_frame_offsets, uint8_t **out_chunk_bytes, size_t *out_chunk_size );
bool configure_chunk_view(Movie *movie, int chunk_index);
bool prefetch_deadline_reached(uint32_t deadline_ms);
void reset_prefetched_chunk(PrefetchedChunk *chunk);
void clear_prefetched_chunk(PrefetchedChunk *chunk);
PrefetchedChunk *find_prefetched_chunk(Movie *movie, int chunk_index);
PrefetchedChunk *find_prefetch_work_chunk(Movie *movie, int current_chunk, int max_distance);
bool prefetch_read_step(Movie *movie, PrefetchedChunk *chunk, bool respect_deadline, uint32_t deadline_ms);
bool prefetch_process_chunk( Movie *movie, PrefetchedChunk *chunk, uint32_t deadline_ms, bool respect_deadline, const PointerState *abort_pointer );
bool prefetch_finish_chunk(Movie *movie, PrefetchedChunk *chunk);
bool load_chunk_from_file(Movie *movie, int chunk_index, bool allow_prefetch_retry);
bool load_chunk(Movie *movie, int chunk_index);
bool seek_bar_preview_decode_active(const SeekBarPreviewState *preview);
bool begin_seek_bar_preview_decode(Movie *movie, SeekBarPreviewState *preview, int chunk_index, uint32_t target_frame);
bool read_seek_bar_preview_chunk_step(Movie *movie, SeekPreviewDecodeJob *job, uint32_t deadline_ms);
bool publish_seek_bar_preview_picture(Movie *movie, SeekBarPreviewState *preview, uint32_t frame_index, const uint8_t *picture);
uint32_t h264_incremental_total_mbs(const Movie *movie, const storage_t *decoder);
void update_h264_incremental_rate(uint16_t *avg_mbs_per_ms_q8, uint32_t elapsed_ms, uint32_t decoded_mbs);
uint32_t h264_incremental_budget( const Movie *movie, const storage_t *decoder, uint16_t avg_mbs_per_ms_q8, uint32_t spare_ms );
uint32_t seek_bar_preview_macroblock_budget( const Movie *movie, const storage_t *decoder, uint16_t avg_mbs_per_ms_q8, uint32_t spare_ms );
void step_seek_bar_preview_decode(Movie *movie, SeekBarPreviewState *preview, uint32_t deadline_ms);
bool finish_seek_bar_preview_pending_frame(Movie *movie, SeekBarPreviewState *preview);
bool prefetch_chunk(Movie *movie, int chunk_index);
void prefetch_ahead(Movie *movie, int current_chunk, int max_new_chunks, int max_new_distance);
void prefetch_do_work( Movie *movie, int current_chunk, int max_work_distance, uint32_t deadline_ms, bool single_step, const PointerState *abort_pointer );
int prefetch_budget_for_state(const Movie *movie, bool paused, uint32_t spare_ms);
int prefetch_target_chunk(const Movie *movie);
bool next_chunk_needs_prefetch(const Movie *movie, int current_chunk);
bool next_chunk_prefetched_ready(const Movie *movie, int current_chunk);
uint32_t next_chunk_prefetch_guard_frames(const Movie *movie, int current_chunk);
bool should_accelerate_next_chunk_io(Movie *movie, int current_chunk);
uint32_t second_next_chunk_prefetch_window_frames(const Movie *movie, int current_chunk);
bool should_prefetch_second_next_chunk(Movie *movie, int current_chunk);
bool should_prioritize_next_chunk_io(const Movie *movie, int current_chunk);
void prefetch_tick(Movie *movie, bool paused, uint32_t spare_ms, const PointerState *abort_pointer);
int movie_chunk_for_frame(const Movie *movie, uint32_t frame_index);
bool decode_h264_frame_with_progress( Movie *movie, uint32_t frame_index, bool blit_output, H264FramePublishPredicate predicate, H264DecodedFrameHook hook, void *userdata );
bool decode_h264_frame( Movie *movie, uint32_t frame_index, bool blit_output );
bool decode_mpeg4_frame( Movie *movie, uint32_t frame_index, bool blit_output );
void invalidate_loaded_chunk_state(Movie *movie);
bool recover_failed_h264_playback_state(Movie *movie);
bool decode_to_frame(Movie *movie, uint32_t frame_index);
bool decode_to_frame_with_progress( Movie *movie, uint32_t frame_index, H264FramePublishPredicate predicate, H264DecodedFrameHook hook, void *userdata, bool *abort_requested );

/* history_screenshots.c */
void strip_filename(char *path);
void history_path_for_directory(const char *directory, char *history_path, size_t history_path_size);
void history_path_for_movie(const char *movie_path, char *history_path, size_t history_path_size);
void history_entry_init_defaults(HistoryEntry *entry);
void apply_history_entry_subtitle_track(const HistoryEntry *entry, Movie *movie);
void apply_history_entry_settings( const HistoryEntry *entry, Movie *movie, ScaleMode *scale_mode, size_t *playback_rate_index, PlaybackMode *playback_mode, bool *realtime_frame_skip, size_t *subtitle_font_index, int *subtitle_size, SubtitlePlacement *subtitle_placement, VideoAlign *video_align_x, VideoAlign *video_align_y );
bool load_history_store_from_path(const char *history_path, HistoryStore *history);
bool load_history_store(const char *movie_path, HistoryStore *history);
bool save_history_store_to_path(const char *history_path, const HistoryStore *history);
bool save_history_store(const char *movie_path, const HistoryStore *history);
void ui_load_theme_for_directory(const char *directory);
void ui_write_theme_for_directory(const char *directory);
void ui_save_theme_for_directory(const char *directory);
void ui_save_theme_for_movie(const char *movie_path);
void flush_queued_theme_save(const char *reason);
int history_find_entry_index(const HistoryStore *history, const char *movie_path);
void history_remove_entry(HistoryStore *history, const char *movie_path);
void history_upsert_entry( HistoryStore *history, const char *movie_path, const Movie *movie, bool has_resume, uint32_t frame, ScaleMode scale_mode, size_t playback_rate_index, PlaybackMode playback_mode, bool realtime_frame_skip, size_t subtitle_font_index, int subtitle_size, SubtitlePlacement subtitle_placement, VideoAlign video_align_x, VideoAlign video_align_y );
bool history_resume_frame_for_movie(const char *movie_path, uint32_t *out_frame);
bool should_save_history_snapshot(const MovieHeader *header, uint32_t frame);
void update_movie_file_resume_from_snapshot(MovieFile *file, const DeferredHistorySave *request);
void update_movie_picker_cache_resume_from_snapshot( MoviePickerCache *cache, const DeferredHistorySave *request );
void queue_history_save_from_movie( DeferredHistorySave *request, MoviePickerCache *cache, const char *movie_path, const Movie *movie, ScaleMode scale_mode, size_t playback_rate_index, PlaybackMode playback_mode, bool realtime_frame_skip, size_t subtitle_font_index, int subtitle_size, SubtitlePlacement subtitle_placement, VideoAlign video_align_x, VideoAlign video_align_y );
void flush_queued_history_save(DeferredHistorySave *request, const char *reason);
bool save_screenshot_bitmap_in_directory(SDL_Surface *screen, const char *directory, char *saved_path, size_t saved_path_size);
bool save_screenshot_bitmap(SDL_Surface *screen, const char *movie_path, char *saved_path, size_t saved_path_size);
void prepare_screenshot_preview(ScreenshotPreviewState *preview, SDL_Surface *screen, const char *saved_path);
bool update_seek_bar_preview(Movie *movie, SeekBarPreviewState *preview, const PointerState *pointer, bool show_ui, uint32_t now_ms);

/* input_timing_memory.c */
void pointer_init(PointerState *pointer);
bool pointer_update(PointerState *pointer);
void pointer_hover_guard_reset(PointerHoverGuard *guard);
void pointer_hover_guard_lock(PointerHoverGuard *guard, const PointerState *pointer);
bool pointer_hover_guard_allows(PointerHoverGuard *guard, const PointerState *pointer);
void picker_tooltip_hover_reset(PickerTooltipHoverState *state);
int picker_tooltip_hover_update(PickerTooltipHoverState *state, int hovered_index, const PointerState *pointer, bool pointer_click, uint32_t now_ms);
char *append_uint_decimal_raw(char *out, uint32_t value);
char *append_two_digits_raw(char *out, uint32_t value);
void copy_truncated(char *buffer, size_t buffer_size, const char *text);
char *append_text_bounded(char *out, char *end, const char *text);
void format_clock(uint32_t total_ms, char *buffer, size_t buffer_size);
MemoryStats query_memory_stats(const Movie *movie);
void format_memory_compact(size_t bytes, char *buffer, size_t buffer_size);
uint64_t movie_frame_interval_ticks(const Movie *movie);
uint32_t tab_hold_frame_repeat_interval_ms(const Movie *movie);
const PlaybackRate *playback_rate_for_index(size_t rate_index);
uint32_t movie_header_frame_time_ms(const MovieHeader *header, uint32_t frame_index);
uint32_t movie_frame_time_ms(const Movie *movie, uint32_t frame_index);
uint64_t movie_frame_time_scaled_ticks(const Movie *movie, uint32_t frame_index, const PlaybackRate *rate);
uint32_t movie_frames_from_ms(const Movie *movie, uint32_t total_ms);
uint32_t movie_frames_from_scaled_ticks(const Movie *movie, uint64_t total_ticks, const PlaybackRate *rate);
uint32_t movie_duration_ms(const Movie *movie);
void reset_playback_timeline(const Movie *movie, const PlaybackRate *playback_rate, uint64_t *anchor_ticks, uint32_t *anchor_frame, uint64_t *next_frame_due_ticks);
bool step_movie_forward_one_frame(Movie *movie, bool *hover_preview_needs_rebuffer);
uint16_t rolling_u16_average(uint16_t current, uint32_t sample_ms);
void record_h264_foreground_decode_time(Movie *movie, uint32_t elapsed_ms);
void record_debug_displayed_frame(Movie *movie, uint32_t now_ms);
bool playback_wait_key_pending(void);
void playback_key_snapshot_init(PlaybackKeySnapshot *snapshot);
bool playback_key_snapshot_new_press(PlaybackKeySnapshot *snapshot);
bool playback_wait_touchpad_pending(const PointerState *pointer);
bool playback_wait_touchpad_click_pending(const PointerState *pointer);
bool playback_wait_input_pending(const PointerState *pointer);
bool prefetch_abort_requested(const PointerState *pointer);
void wait_until_ticks_playback(uint64_t target_ticks, const PointerState *pointer);
void free_movie_files(MovieFile *files, size_t count);
void clear_movie_picker_cache(MoviePickerCache *cache);
void free_history_store(HistoryStore *history);

/* movie_resources.c */
bool sram_movie_chunk_buffer_can_hold(size_t size);
void release_movie_chunk_storage(Movie *movie);
bool allocate_movie_chunk_storage(Movie *movie, size_t size);
bool adopt_movie_chunk_storage(Movie *movie, uint8_t **storage, size_t size);
void destroy_movie(Movie *movie);
void defer_playback_movie_cleanup(Movie *movie);
void cleanup_deferred_playback_movie(void);
bool init_fonts(Fonts *fonts);
void free_fonts(Fonts *fonts);
bool movie_uses_h264(const Movie *movie);
bool init_mpeg4_decoder_global(void);
bool init_h264_color_tables(void);
void init_sram_movie_chunk_buffer(void);
uint32_t h264_prefetch_io_min_spare_ms(const Movie *movie);
void debug_trace_runtime_snapshot( Movie *movie, bool paused, uint32_t spare_ms, const PlaybackRate *playback_rate, const char *tag );
void debug_dump_session(const char *path, const Movie *movie, const char *reason);
void debug_log_sram_status(void);

/* picker_loop.c */
int pick_movie( SDL_Surface *screen, const Fonts *fonts, const char *directory, char *selected_path, size_t selected_size, bool *resume_without_prompt );
bool seek_delta_target_frame(const Movie *movie, int32_t delta_ms, uint32_t *out_target_frame);

/* picker_ui.c */
SDL_Rect picker_row_rect_for_y(int row_y);
SDL_Rect picker_row_divider_rect(const SDL_Rect *row);
size_t picker_scroll_start_centered(size_t count, size_t selected);
size_t picker_scroll_start_clamped(size_t count, size_t start_index);
size_t picker_scroll_start_for_selection(size_t count, size_t selected, size_t start_index);
void movie_picker_cache_remember_position( MoviePickerCache *cache, size_t count, size_t selected, size_t scroll_start );
void picker_scroll_anim_view( PickerScrollAnim *anim, size_t scroll_start, uint32_t now_ms, size_t *display_start, int *offset_y );
void picker_scroll_to( size_t count, size_t *scroll_start, PickerScrollAnim *anim, size_t next_start, uint32_t now_ms );
int picker_hover_scroll_direction(size_t count, size_t start_index, const PointerState *pointer);
int picker_row_index_at(size_t count, size_t start_index, int row_offset_y, int x, int y);
bool picker_resume_badge_rect(const Fonts *fonts, const MovieFile *file, int row_y, SDL_Rect *rect);
int picker_resume_badge_index_at( const Fonts *fonts, MovieFile *files, size_t count, size_t start_index, int row_offset_y, int x, int y );
void draw_loading_overlay_mix( SDL_Surface *screen, const Fonts *fonts, const char *label, int phase, uint8_t mix, bool opening );
void draw_loading_overlay_label_mix( SDL_Surface *screen, const Fonts *fonts, const SDL_Rect *panel, const char *label, int phase, uint8_t mix );
SDL_Surface *capture_screen_surface(SDL_Surface *screen);
SDL_Surface *capture_rect_surface(SDL_Surface *screen, const SDL_Rect *source);
SDL_Surface *begin_faded_region_draw( SDL_Surface *screen, const SDL_Rect *bounds, uint8_t mix, SDL_Surface **target, SDL_Rect *old_clip, int *dx, int *dy );
void end_faded_region_draw( SDL_Surface *screen, SDL_Surface *surface, const SDL_Rect *bounds, const SDL_Rect *old_clip, uint8_t mix );
SDL_Surface *create_black_screen_snapshot(SDL_Surface *screen);
void draw_loading_transition_frame( SDL_Surface *screen, SDL_Surface *snapshot, const Fonts *fonts, const char *label, uint8_t mix, int phase, bool opening, bool dim_background );
void loading_progress_init( LoadingProgress *progress, SDL_Surface *screen, SDL_Surface *snapshot, const Fonts *fonts, const char *label, bool dim_background );
void loading_progress_tick(LoadingProgress *progress, bool force);
void animate_loading_transition_ex( SDL_Surface *screen, SDL_Surface *snapshot, const Fonts *fonts, const char *label, bool opening, bool dim_background );
void animate_loading_transition( SDL_Surface *screen, SDL_Surface *snapshot, const Fonts *fonts, const char *label, bool opening );
void finish_loading_transition_ex( SDL_Surface *screen, SDL_Surface **snapshot, const Fonts *fonts, const char *label, bool dim_background );
void finish_loading_transition( SDL_Surface *screen, SDL_Surface **snapshot, const Fonts *fonts, const char *label );
SDL_Rect movie_vertical_morph_dst(const SDL_Rect *base_dst, uint8_t visible_mix);
void animate_movie_collapse_to_black( SDL_Surface *screen, Movie *movie, ScaleMode scale_mode, VideoAlign video_align_x, VideoAlign video_align_y, SDL_Surface **overlay_surface );
void copy_fitted_text(nSDL_Font *font, const char *text, char *buffer, size_t buffer_size, int max_width);
void draw_movie_hover_tooltip( SDL_Surface *screen, const Fonts *fonts, const MovieFile *file, const PointerState *pointer, uint8_t tooltip_mix );
void draw_resume_badge( SDL_Surface *screen, const Fonts *fonts, const SDL_Rect *badge, uint8_t hover_mix, uint8_t press_mix, uint8_t visible_mix, Uint16 background_color );
void draw_resume_hover_tooltip( SDL_Surface *screen, const Fonts *fonts, const MovieFile *file, const PointerState *pointer, uint8_t tooltip_mix );
uint8_t picker_selection_ease(uint32_t elapsed_ms, uint32_t duration_ms);
uint8_t picker_row_selection_mix( size_t index, size_t selected, size_t previous_selected, uint32_t selection_anim_started_ms, uint32_t now_ms, uint8_t selected_start_mix, uint8_t previous_start_mix );
void picker_set_selected_row( size_t *selected, size_t *previous_selected, uint32_t *selection_anim_started_ms, uint8_t *selected_start_mix, uint8_t *previous_start_mix, size_t next_selected, uint32_t now_ms );
uint8_t prompt_button_selection_mix( size_t index, size_t selected, size_t previous_selected, uint32_t selection_anim_started_ms, uint32_t now_ms );
uint8_t picker_intro_mix(uint32_t intro_started_ms, uint32_t now_ms, uint32_t delay_ms, uint32_t duration_ms);
uint32_t picker_exit_timeline_ms(uint32_t exit_elapsed_ms);
uint8_t picker_intro_mix_for_transition( uint32_t intro_started_ms, uint32_t now_ms, uint32_t delay_ms, uint32_t duration_ms, uint32_t exit_elapsed_ms );
int picker_intro_offset(uint8_t mix, int distance);
void picker_set_selected( size_t *selected, size_t *previous_selected, uint32_t *selection_anim_started_ms, size_t next_selected, uint32_t now_ms );
size_t picker_adjacent_selection(size_t count, size_t selected, int direction);
void render_picker( SDL_Surface *screen, const Fonts *fonts, MovieFile *files, size_t count, size_t scroll_start, int scroll_offset_y, size_t selected, size_t previous_selected, uint32_t selection_anim_started_ms, uint8_t selected_start_mix, uint8_t previous_start_mix, int movie_tooltip_index, uint8_t movie_tooltip_mix, int resume_badge_hover_index, uint8_t resume_badge_hover_mix, int resume_tooltip_index, uint8_t resume_tooltip_mix, int pressed_row_index, int pressed_resume_badge_index, uint8_t press_mix, const PointerState *pointer, const ScreenshotPreviewState *screenshot_preview, uint32_t now_ms, uint32_t intro_started_ms, uint32_t exit_elapsed_ms, uint8_t loading_mix, const char *loading_label, int loading_phase );

/* platform_debug.c */
bool ensure_debug_ring_storage(void);
void release_debug_ring_storage(void);
bool debug_is_runtime_logging_enabled(void);
bool debug_should_collect_metrics(void);
scr_type_t screen_buffer_type(void);
scr_type_t screen_lcd_type(void);
void patch_cx2_lcd_edge_timing(void);
void present_screen(SDL_Surface *screen);
void present_black_screen(SDL_Surface *screen);
void debug_set_metrics_collection(bool enabled);
void debug_set_runtime_logging(bool enabled);
void debug_tracevf(bool force, const char *fmt, va_list args);
void debug_tracef(const char *fmt, ...);
void debug_tracef_force(const char *fmt, ...);
void debug_clear_last_error(void);
const char *debug_last_error(void);
void debug_failf(const char *fmt, ...);
size_t total_prefetched_chunk_bytes(const Movie *movie);
void clear_all_prefetched_chunks(Movie *movie);
PrefetchedChunk *find_farthest_prefetched_chunk(Movie *movie);
bool ensure_prefetch_budget(Movie *movie, int requested_chunk, size_t required_bytes);
void debug_log_path_for_movie(const char *movie_path, char *log_path, size_t log_path_size);
void report_movie_decode_failure(const Movie *movie, const char *movie_path, const char *reason);
void report_movie_open_failure(const char *movie_path);
uint16_t read_le16(const uint8_t *src);
uint32_t read_le32(const uint8_t *src);
char *dup_string(const char *src);
bool has_suffix(const char *value, const char *suffix);
char *display_name_for_movie(const char *filename);
void normalize_display_spacing(char *text);
void append_movie_filename_detail(char *detail, size_t detail_size, const char *begin, size_t length);
bool movie_display_fields_for_filename(const char *filename, char **out_name, char **out_detail);
int clamp_int(int value, int min_value, int max_value);
Sint16 chrome_centered_x_for_width(int width);
Sint16 chrome_left_x_for_margin(int margin);
Sint16 chrome_right_x_for_margin(int margin);
Sint16 chrome_soft_panel_right_x_for_margin(int margin);
bool rect_contains_point(const SDL_Rect *rect, int x, int y);
bool pointer_over_rect(const PointerState *pointer, const SDL_Rect *rect);
VideoAlign clamp_video_align(int value);
void apply_video_align_preset( VideoAlign *horizontal, VideoAlign *vertical, VideoAlign target_horizontal, VideoAlign target_vertical );
int aligned_axis_position(int container_size, int content_size, VideoAlign align);
void format_video_align_status( VideoAlign horizontal, VideoAlign vertical, char *buffer, size_t buffer_size );
uint32_t cx_level_to_brightness_raw(uint32_t level);
uint32_t brightness_raw_to_cx_level(uint32_t raw_value);
uint32_t current_lcd_brightness(void);
uint32_t set_lcd_brightness(int value);
void set_lcd_dark_for_power_off(void);
uint32_t lcd_brightness_step_target_from(uint32_t raw_value, int delta);
uint32_t adjust_lcd_brightness(int delta);
unsigned lcd_brightness_percent(uint32_t raw_value);
void display_power_init(DisplayPowerState *state, uint32_t now_ms);
void display_power_note_activity(DisplayPowerState *state, uint32_t now_ms);
bool display_power_tick_idle(DisplayPowerState *state, SDL_Surface *screen, uint32_t now_ms, bool allow_idle_dim, bool was_paused);
void display_power_off(DisplayPowerState *state, bool was_paused);
void display_power_off_with_saved_brightness(DisplayPowerState *state, SDL_Surface *screen, uint32_t saved_brightness, bool was_paused);
void display_power_off_for_exit(DisplayPowerState *state, SDL_Surface *screen, bool was_paused);
void display_power_on(DisplayPowerState *state);
void display_power_restore(DisplayPowerState *state, uint32_t now_ms);
const char *filename_from_path(const char *path);
SDL_Surface *create_rgb565_surface(int width, int height);
SDL_Surface *create_scaled_surface_from_surface(SDL_Surface *source, int max_width, int max_height);
void invalidate_subtitle_surface_cache(SubtitleSurfaceCache *cache);
void free_subtitle_surface_cache(SubtitleSurfaceCache *cache);
void clear_screenshot_preview(ScreenshotPreviewState *preview);
void clear_seek_bar_preview_decode_job(SeekBarPreviewState *preview);
void finish_seek_bar_preview_decode_job(SeekBarPreviewState *preview);
void clear_seek_bar_preview_surface(SeekBarPreviewState *preview);
void clear_seek_bar_preview(SeekBarPreviewState *preview);
void suppress_seek_bar_preview_rebuild(SeekBarPreviewState *preview, int marker_x, uint32_t frame_index);
bool monotonic_clock_try_init_hw_timer(void);
void monotonic_clock_init(void);
uint32_t monotonic_clock_ticks_per_second(void);
uint64_t monotonic_clock_now_ticks(void);
uint32_t monotonic_clock_ticks_to_ms(uint64_t ticks);
uint32_t monotonic_clock_now_ms(void);
void monotonic_clock_shutdown(void);

/* playback_loop.c */
int play_movie( SDL_Surface *screen, const Fonts *fonts, const char *path, char *next_path, size_t next_path_size, bool resume_without_prompt, bool loading_already_open );

/* playback_ui.c */
const char *scale_mode_text(ScaleMode scale_mode);
const char *playback_mode_text(PlaybackMode playback_mode);
int top_overlay_y_for_rect(const SDL_Rect *video_rect, int overlay_h);
uint8_t ui_ease_out_cubic(uint32_t elapsed_ms, uint32_t duration_ms);
uint8_t ui_ease_in_cubic(uint32_t elapsed_ms, uint32_t duration_ms);
uint8_t ui_ease_smoothstep(uint32_t elapsed_ms, uint32_t duration_ms);
bool rects_equal(const SDL_Rect *a, const SDL_Rect *b);
SDL_Rect mix_sdl_rects(const SDL_Rect *from, const SDL_Rect *to, uint8_t mix);
SDL_Rect offset_sdl_rect(const SDL_Rect *rect, int dx, int dy);
uint8_t staggered_content_mix(uint32_t elapsed_ms, uint32_t start_ms, uint32_t stagger_ms, uint32_t index, uint32_t duration_ms);
void scale_morph_current_rects( const Movie *movie, ScaleMorphState *morph, ScaleMode scale_mode, VideoAlign video_align_x, VideoAlign video_align_y, uint32_t now_ms, SDL_Rect *src, SDL_Rect *dst );
bool scale_morph_animating(const ScaleMorphState *morph, uint32_t now_ms);
void begin_scale_mode_morph( Movie *movie, ScaleMorphState *morph, ScaleMode current_mode, ScaleMode next_mode, VideoAlign video_align_x, VideoAlign video_align_y, uint32_t now_ms );
void cycle_scale_mode_with_morph( Movie *movie, ScaleMorphState *morph, ScaleMode *scale_mode, VideoAlign video_align_x, VideoAlign video_align_y, uint32_t now_ms );
void ui_transition_init(UiTransition *transition, bool active);
uint8_t ui_transition_value_with_ease( const UiTransition *transition, uint32_t now_ms, uint32_t duration_ms, uint8_t (*ease_fn)(uint32_t, uint32_t) );
uint8_t ui_transition_value(const UiTransition *transition, uint32_t now_ms, uint32_t duration_ms);
uint8_t ui_transition_update_ex( UiTransition *transition, bool active, uint32_t now_ms, uint32_t duration_ms, uint8_t active_min_mix );
uint8_t ui_transition_update( UiTransition *transition, bool active, uint32_t now_ms, uint32_t duration_ms );
uint8_t ui_transition_update_press_ex( UiTransition *transition, bool active, uint32_t now_ms, uint32_t press_duration_ms, uint32_t release_duration_ms );
uint8_t ui_transition_update_press( UiTransition *transition, bool active, uint32_t now_ms );
void ui_transition_prime_press(UiTransition *transition, uint32_t now_ms);
void ui_transition_begin_press_release(UiTransition *transition, uint32_t now_ms);
bool ui_mix_animating(uint8_t mix);
bool playback_ui_mixes_animating(const PlaybackUiMixes *mixes);
void trigger_playback_badge_press( PlaybackUiTransitions *transitions, uint32_t *press_until_ms, uint32_t now_ms );
void trigger_badge_press(UiTransition *transition, uint32_t *press_until_ms, uint32_t now_ms);
bool ui_time_before(uint32_t now_ms, uint32_t until_ms);
void status_overlay_show(uint32_t now_ms, bool restart_animation, uint32_t *started_ms, uint32_t *until_ms);
bool seek_preview_surface_animating(const SeekBarPreviewState *preview, uint32_t now_ms);
void note_pause_transition( bool was_paused, bool now_paused, uint32_t now_ms, uint32_t *quiet_until_ms );
Uint16 animated_control_color(Uint16 idle_color, Uint16 active_color, uint8_t active_mix);
SDL_Rect text_badge_rect(const Fonts *fonts, int right_x, int y, const char *label);
int pressed_control_offset_y(uint8_t press_mix);
int pressed_control_offset_x(uint8_t press_mix);
uint8_t max_u8(uint8_t a, uint8_t b);
Uint16 pressed_control_base(Uint16 base, uint8_t press_mix);
void draw_pressed_control_reflection(SDL_Surface *screen, const SDL_Rect *rect, uint8_t press_mix);
int draw_text_badge( SDL_Surface *screen, const Fonts *fonts, int right_x, int y, const char *label, uint8_t hover_mix, uint8_t press_mix );
int draw_left_text_badge(SDL_Surface *screen, const Fonts *fonts, int left_x, int y, const char *label);
int draw_left_text_badge_animated( SDL_Surface *screen, const Fonts *fonts, int left_x, int y, const char *label, uint8_t mix, int offset_x );
void draw_seek_delta_badge( SDL_Surface *screen, const Fonts *fonts, int32_t seek_ms, uint32_t started_ms, uint32_t hide_elapsed_ms, uint32_t now_ms );
void draw_status_overlay_badge( SDL_Surface *screen, const Fonts *fonts, int left_x, int y, const char *label, uint32_t started_ms, uint32_t until_ms, uint32_t now_ms, uint8_t chrome_mix );
void draw_playback_title_strip( SDL_Surface *screen, const Fonts *fonts, const SDL_Rect *video_rect, ScaleMode scale_mode, const PlaybackRate *playback_rate, const char *title, const char *detail, uint8_t mix );
void draw_centered_text_badge(SDL_Surface *screen, const Fonts *fonts, int center_x, int y, const char *label);
int header_shortcut_badge_width(const Fonts *fonts, const char *key, const char *action);
void draw_header_shortcut_badge( SDL_Surface *screen, const Fonts *fonts, int x, int y, const char *key, const char *action, uint8_t mix );
void draw_header_shortcuts(SDL_Surface *screen, const Fonts *fonts, int y, uint8_t mix);
void draw_surface_panel(SDL_Surface *screen, SDL_Surface *surface, int x, int y);
void draw_seek_preview_panel(SDL_Surface *screen, SDL_Surface *surface, int x, int y, int marker_x, uint8_t panel_mix);
void draw_screenshot_preview_osd( SDL_Surface *screen, const Fonts *fonts, const ScreenshotPreviewState *preview, uint32_t now_ms );
void format_seek_delta(int32_t delta_ms, char *buffer, size_t buffer_size);
void status_badge_rects( const Fonts *fonts, const SDL_Rect *video_rect, ScaleMode scale_mode, const PlaybackRate *playback_rate, SDL_Rect *scale_badge, SDL_Rect *speed_badge );
int draw_status_badges( SDL_Surface *screen, const Fonts *fonts, const SDL_Rect *video_rect, ScaleMode scale_mode, const PlaybackRate *playback_rate, const PlaybackUiMixes *ui_mixes, uint8_t chrome_mix );
void update_playback_ui_mixes( PlaybackUiTransitions *transitions, PlaybackUiMixes *mixes, const Fonts *fonts, const Movie *movie, ScaleMode scale_mode, ScaleMorphState *scale_morph, VideoAlign video_align_x, VideoAlign video_align_y, const PlaybackRate *playback_rate, bool show_ui, bool help_menu_open, PlaybackPressTarget pressed_target, bool force_playback_press, bool force_scale_press, bool force_speed_press, bool show_title_strip, const PointerState *pointer, uint32_t now_ms );
SDL_Rect playback_badge_rect(const SDL_Rect *video_rect);
void draw_playback_badge(SDL_Surface *screen, const SDL_Rect *video_rect, bool paused, uint8_t hover_mix, uint8_t press_mix, uint8_t chrome_mix);
void draw_memory_badge( SDL_Surface *screen, const Fonts *fonts, const Movie *movie, const SDL_Rect *video_rect, int right_limit, bool playback_badge_visible, uint8_t chrome_mix );
void draw_help_row( SDL_Surface *screen, const Fonts *fonts, int shortcut_x, int shortcut_w, int description_x, int y, const char *shortcut, const char *description );
void draw_help_menu(SDL_Surface *screen, const Fonts *fonts, uint8_t menu_mix);
bool chunk_list_contains(const int *chunks, size_t count, int chunk_index);
void chunk_list_add_unique(int *chunks, size_t *count, size_t capacity, int chunk_index);
void movie_update_ui_buffer_chunks(Movie *movie, int *chunks_to_draw, size_t *num_chunks_to_draw);
Uint16 progress_overlay_fill_color_at_y(const SDL_Rect *overlay, int y);
void draw_progress_track(SDL_Surface *screen, const SDL_Rect *bar_back, const SDL_Rect *overlay);
void draw_progress_buffer_range(SDL_Surface *screen, const SDL_Rect *rect);
void draw_progress_overlay(SDL_Surface *screen, const SDL_Rect *overlay);
void draw_progress( SDL_Surface *screen, const Fonts *fonts, Movie *movie, uint32_t current_ms, bool paused, const PlaybackRate *playback_rate, uint32_t now_ms, const PointerState *pointer, int32_t pending_seek_ms, int32_t seek_badge_ms, uint32_t seek_badge_started_ms, uint32_t seek_badge_hide_elapsed_ms, SeekBarPreviewState *seek_preview, uint8_t preview_mix, uint8_t chrome_mix );
void render_movie( SDL_Surface *screen, const Fonts *fonts, Movie *movie, bool paused, bool show_ui, bool help_menu_open, ScaleMode scale_mode, ScaleMorphState *scale_morph, VideoAlign video_align_x, VideoAlign video_align_y, const PlaybackRate *playback_rate, MemoryOverlayMode memory_overlay_mode, SubtitleSurfaceCache *subtitle_cache, size_t subtitle_font_index, bool subtitle_font_overlay_visible, int subtitle_size, SubtitlePlacement subtitle_placement, const char *movie_title_text, const char *movie_detail_text, const char *status_overlay_text, uint32_t status_overlay_started_ms, uint32_t status_overlay_until_ms, const ScreenshotPreviewState *screenshot_preview, SeekBarPreviewState *seek_preview, uint32_t now_ms, const PointerState *pointer, int32_t pending_seek_ms, int32_t seek_badge_ms, uint32_t seek_badge_started_ms, uint32_t seek_badge_hide_elapsed_ms, const PlaybackUiMixes *ui_mixes );
bool should_publish_committed_seek_frame(Movie *movie, uint32_t frame_index, void *userdata);
bool render_committed_seek_frame(Movie *movie, uint32_t frame_index, void *userdata);
bool commit_seek_bar_preview_to_movie(Movie *movie, SeekBarPreviewState *preview, uint32_t target_frame);
void draw_movie_frame_background( SDL_Surface *screen, Movie *movie, ScaleMode scale_mode, VideoAlign video_align_x, VideoAlign video_align_y, SDL_Rect *out_src, SDL_Rect *out_dst );

/* render_primitives.c */
SDL_Rect progress_bar_rect(void);
uint32_t progress_bar_denominator(const SDL_Rect *bar);
int progress_bar_marker_x_from_pointer(const SDL_Rect *bar, int pointer_x);
uint32_t progress_bar_ms_for_marker(const Movie *movie, const SDL_Rect *bar, int marker_x);
bool progress_bar_target_frame_for_marker(const Movie *movie, const SDL_Rect *bar, int marker_x, uint32_t *out_target_frame, uint32_t *out_target_ms);
UiThemeId ui_theme_clamp(int theme_id);
const UiThemePalette *ui_theme(void);
const char *ui_theme_name(UiThemeId theme_id);
void ui_set_theme(UiThemeId theme_id);
UiThemeId ui_cycle_theme(void);
bool ui_theme_transition_active(void);
bool surface_is_rgb565(const SDL_Surface *surface);
void rgb565_to_rgb888(Uint16 color, int *r, int *g, int *b);
Uint32 map_rgb565(SDL_Surface *screen, Uint16 color);
Uint16 rgb565_lerp(Uint16 from, Uint16 to, int step, int steps);
Uint16 blend_rgb565(Uint16 base, Uint16 overlay, int alpha);
void fill_rect_rgb565(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color);
void fill_rect_rgb565_mix(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color, uint8_t mix);
void blit_surface_rgb565_mix(SDL_Surface *screen, SDL_Surface *surface, const SDL_Rect *dst_rect, uint8_t mix);
void dim_rect_rgb565(SDL_Surface *screen, const SDL_Rect *rect, int alpha);
void draw_vertical_gradient(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color_top, Uint16 color_bottom);
void draw_rect_outline_rgb565(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color);
void cut_rect_corners(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color);
Uint16 picker_background_color_at_y(int y);
Uint16 picker_background_color_at_y_mix(int y, uint8_t mix);
int ui_mix_int(int from, int to, uint8_t mix);
Sint16 ui_mix_sint16(int from, int to, uint8_t mix);
Uint16 ui_mix_uint16(int from, int to, uint8_t mix);
void fill_picker_selection_line( SDL_Surface *screen, const SDL_Rect *line, Uint16 target_color, uint8_t mix );
Uint16 control_outline_color(Uint16 base_color, uint8_t selected_mix);
void draw_picker_selection_panel_mix( SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, uint8_t selection_mix );
void draw_glass_panel_mix(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, uint8_t selected_mix);
void draw_glass_panel(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, bool is_selected);
void draw_glass_panel_faded(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, bool is_selected, uint8_t mix);
int soft_panel_inset_for_row(int row, int height);
int soft_panel_top_inset_for_row(int row, int height);
void fill_soft_panel_backplate(SDL_Surface *screen, const SDL_Rect *rect, Uint16 color);
void draw_soft_glass_panel_mix(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, uint8_t selected_mix);
void draw_soft_glass_panel_top_mix(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, uint8_t selected_mix);
void draw_soft_glass_panel_body_from_y( SDL_Surface *screen, const SDL_Rect *panel, int body_y, Uint16 base_color );
void draw_soft_glass_panel_rim(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, uint8_t selected_mix);
void draw_soft_glass_panel(SDL_Surface *screen, const SDL_Rect *rect, Uint16 base_color, bool is_selected);
void draw_ui_label(SDL_Surface *screen, const Fonts *fonts, int x, int y, const char *label);
bool surface_pixel_has_ink(SDL_Surface *surface, int x, int y);
bool font_char_ink_bounds(nSDL_Font *font, unsigned char ch, int *out_min_x, int *out_max_x);
int font_char_advance_width(nSDL_Font *font, unsigned char ch);
bool font_string_ink_bounds(nSDL_Font *font, const char *text, int *out_min_x, int *out_max_x);
void draw_ui_label_ink_left(SDL_Surface *screen, const Fonts *fonts, int ink_left_x, int y, const char *label);
void draw_ui_label_ink_right(SDL_Surface *screen, const Fonts *fonts, int ink_right_x, int y, const char *label);
void draw_overlay_backdrop_dim(SDL_Surface *screen, uint8_t dim_mix);
void draw_cursor(SDL_Surface *screen, int x, int y);
void compute_video_rects( const Movie *movie, ScaleMode scale_mode, VideoAlign video_align_x, VideoAlign video_align_y, SDL_Rect *src, SDL_Rect *dst );
bool clip_scaled_rects_to_screen( const SDL_Rect *src, const SDL_Rect *dst, int source_w, int source_h, SDL_Rect *clipped_src, SDL_Rect *clipped_dst );
void draw_surface_frame_scaled_clipped( SDL_Surface *screen, SDL_Surface *surface, const SDL_Rect *src, const SDL_Rect *dst );
void draw_surface_frame_scaled_clipped_mix( SDL_Surface *screen, SDL_Surface *surface, const SDL_Rect *src, const SDL_Rect *dst, uint8_t mix );
void draw_movie_frame_scaled_clipped( SDL_Surface *screen, Movie *movie, const SDL_Rect *src, const SDL_Rect *dst );
void draw_movie_frame_background_rects( SDL_Surface *screen, Movie *movie, const SDL_Rect *src, const SDL_Rect *dst );

/* resume_prompt.c */
void draw_prompt_button( SDL_Surface *screen, const Fonts *fonts, const SDL_Rect *button, const char *label, uint8_t selection_mix, uint8_t press_mix );
int prompt_resume_position( SDL_Surface *screen, const Fonts *fonts, Movie *movie, const char *path, uint32_t resume_frame, ScaleMode scale_mode, VideoAlign video_align_x, VideoAlign video_align_y, SDL_Surface **loading_snapshot );

/* subtitles.c */
const char *active_subtitle_track_name(const Movie *movie);
void draw_outlined_text(SDL_Surface *surface, nSDL_Font *white_font, nSDL_Font *outline_font, int x, int y, const char *text);
int wrap_subtitle(nSDL_Font *font, const char *text, int max_width, char lines[MAX_SUBTITLE_LINES][MAX_SUBTITLE_LINE_LEN]);
int subtitle_scale_num(int subtitle_size);
int subtitle_scale_den(int subtitle_size);
int subtitle_font_id_for_index(size_t subtitle_font_index);
const char *subtitle_font_name_for_index(size_t subtitle_font_index);
SubtitlePlacement subtitle_opposite_placement(SubtitlePlacement placement);
const char *subtitle_placement_label(SubtitlePlacement placement);
bool subtitle_track_supports_auto_positioning(const Movie *movie, uint16_t track_index);
bool selected_subtitle_track_supports_auto_positioning(const Movie *movie);
SubtitlePlacement subtitle_normalize_placement(SubtitlePlacement placement, bool auto_supported);
SubtitlePlacement subtitle_cycle_placement(SubtitlePlacement placement, bool auto_supported);
SubtitlePlacement subtitle_effective_manual_placement(SubtitlePlacement placement, SubtitlePlacement fallback);
int subtitle_align_column(uint8_t align);
int subtitle_align_row(uint8_t align);
int subtitle_scale_coord(uint16_t value, int extent);
bool subtitle_resolve_layout_spec( const SDL_Rect *video_rect, uint8_t overlay_mix, SubtitlePlacement placement, SubtitlePlacement manual_fallback, const SubtitleCue *cue, SubtitleLayoutSpec *layout );
int ui_bar_hidden_offset_for_mix(uint8_t chrome_mix);
int ui_bar_visible_height_for_mix(uint8_t chrome_mix);
int subtitle_visible_bottom_limit(const SubtitleLayoutSpec *layout, int bottom_margin);
int subtitle_visible_max_y(const SubtitleLayoutSpec *layout, int surface_h);
void subtitle_layout_dst_rect( const SubtitleLayoutSpec *layout, int surface_w, int surface_h, SDL_Rect *dst );
void subtitle_fonts_for_style(const Fonts *fonts, size_t subtitle_font_index, nSDL_Font **white_font, nSDL_Font **outline_font);
void draw_scaled_outlined_text( SDL_Surface *screen, nSDL_Font *white_font, nSDL_Font *outline_font, int x, int y, const char *text, int scale_num, int scale_den );
bool ensure_subtitle_surface_cache( SubtitleSurfaceCache *cache, SDL_Surface *screen, const Fonts *fonts, const char *text, size_t subtitle_font_index, int subtitle_size, int wrap_width );
void draw_subtitle_cached( SDL_Surface *screen, const Fonts *fonts, SubtitleSurfaceCache *cache, const char *text, size_t subtitle_font_index, int subtitle_size, const SubtitleLayoutSpec *layout );
void draw_subtitle( SDL_Surface *screen, const Fonts *fonts, const char *text, size_t subtitle_font_index, int subtitle_size, const SubtitleLayoutSpec *layout );

#define UI_COLOR_ACCENT (ui_theme()->accent_mid)
#define UI_COLOR_ACCENT_DEEP (ui_theme()->accent_deep)
#define UI_COLOR_ACCENT_HOT (ui_theme()->accent_hot)
#define UI_COLOR_CARBON (ui_theme()->carbon)
#define UI_COLOR_GUNMETAL (ui_theme()->gunmetal)
#define UI_COLOR_WARM_WHITE (ui_theme()->warm_white)
#define UI_COLOR_BG_TOP (ui_theme()->bg_top)
#define UI_COLOR_BG_BOTTOM (ui_theme()->bg_bottom)

#endif
