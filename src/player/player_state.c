#include "player_internal.h"

MonotonicClock g_clock;
char (*g_debug_ring)[DEBUG_LINE_LEN] = NULL;
size_t g_debug_ring_count = 0;
size_t g_debug_ring_next = 0;
char g_last_error_message[DEBUG_LINE_LEN];
bool g_debug_logging_enabled = false;
bool g_debug_metrics_enabled = false;
H264ColorTables g_h264_color_tables_storage;
H264ColorTables *g_h264_color_tables = &g_h264_color_tables_storage;
bool g_h264_color_tables_in_sram = false;
uint8_t *g_sram_movie_chunk_buffer = NULL;
size_t g_sram_movie_chunk_buffer_size = 0;
Movie *g_deferred_playback_movie = NULL;
MoviePickerCache g_picker_cache;
DeferredHistorySave g_pending_history_save;
char g_pending_theme_directory[MAX_PATH_LEN];
bool g_pending_theme_save = false;
const PlaybackRate g_playback_rates[PLAYBACK_RATE_COUNT] = {
    {1, 4, "0.25x"},
    {1, 2, "0.5x"},
    {3, 4, "0.75x"},
    {1, 1, "1.0x"},
    {5, 4, "1.25x"},
    {3, 2, "1.5x"},
    {7, 4, "1.75x"},
    {2, 1, "2.0x"},
};
const int g_subtitle_font_choices[SUBTITLE_FONT_CHOICE_COUNT] = {
    NSDL_FONT_TINYTYPE,
    NSDL_FONT_VGA,
    NSDL_FONT_THIN,
    NSDL_FONT_SPACE,
    NSDL_FONT_FANTASY,
};
const char *g_subtitle_font_names[SUBTITLE_FONT_CHOICE_COUNT] = {
    "Tinytype",
    "VGA",
    "Thin",
    "Space",
    "Fantasy",
};
