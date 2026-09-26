#include "player_internal.h"
#include "crash_recorder.h"
#include "native_runtime_stats.h"
#include "native_screen_power.h"
#include "app_task_io.h"
#include <time.h>

/* D enables this bounded diagnostic journal alongside its RAM capture.
 * No player pointer crosses into the writer. Expensive observations happen
 * once per second, not at every phase hook or video presentation. */
static struct {
    const Movie *movie;
    bool active, attempted;
    uint64_t next_sample, last_present, max_gap;
    uint32_t presents, build_id, media_id, minimum_free, minimum_stack;
    const PlaybackRate *rate;
    bool paused;
    CrashRecorderSnapshot last;
} crash_trace;

static uint32_t crash_trace_word(uintptr_t address)
{
    return *(const volatile uint32_t *)address;
}

static uint32_t crash_trace_hash(const char *text)
{
    uint32_t hash = 2166136261U;
    while (*text)
        hash = (hash ^ (uint8_t)*text++) * 16777619U;
    return hash;
}

static void crash_trace_snapshot(const Movie *movie, unsigned phase, uint64_t now)
{
    NativeRuntimeStats runtime;
    AppTaskIoStats io;
    NativeScreenPowerSnapshot screen;
    CrashRecorderStats journal;
    MemoryStats memory = query_memory_stats(movie);
    uint32_t *w = crash_trace.last.words;
    native_runtime_stats_snapshot(&runtime);
    app_task_io_stats(&io);
    native_screen_power_snapshot(&screen);
    crash_recorder_stats(&journal);
    if ((runtime.valid_flags & NATIVE_RUNTIME_STATS_DYNAMIC_POOL) &&
        runtime.dynamic_pool_available_bytes < crash_trace.minimum_free)
        crash_trace.minimum_free = runtime.dynamic_pool_available_bytes;
    if (io.minimum_stack_remaining < crash_trace.minimum_stack)
        crash_trace.minimum_stack = io.minimum_stack_remaining;
    memset(&crash_trace.last, 0, sizeof(crash_trace.last));
    crash_trace.last.monotonic_ticks = now;
    w[0] = 4U;
    w[1] = crash_trace.build_id;
    /* A successful recorder start established the exact native platform gate. */
    w[2] = crash_trace_word(0x90090000U);
    w[3] = phase;
    w[4] = movie->current_frame;
    /* Decoder lookahead may have loaded a later chunk than the visible frame. */
    w[5] = (uint32_t)movie_chunk_for_frame(movie, movie->current_frame);
    w[6] = movie->header.fps_num;
    w[7] = movie->header.fps_den;
    w[8] = ((uint32_t)movie->header.video_width << 16) | movie->header.video_height;
    w[9] = (uint32_t)movie->codec;
    if (crash_trace.rate)
        w[10] = ((uint32_t)crash_trace.rate->numerator << 16) | crash_trace.rate->denominator;
    w[11] = (crash_trace.paused ? 1U : 0U) | (g_display_power_state.off ? 2U : 0U) |
            (sram_is_enabled() ? 4U : 0U) | (movie_async_enabled(movie) ? 8U : 0U) |
            (playback_capture_active(movie) ? 16U : 0U) | (g_clock.using_hw_timer ? 32U : 0U) |
            (screen.restore_required ? 64U : 0U) | (memory.valid ? 128U : 0U);
    w[12] = io.contexts;
    w[13] = (uint32_t)memory.used_bytes;
    w[14] = (uint32_t)memory.prefetched_bytes;
    w[15] = (uint32_t)movie->chunk_storage_size;
    w[16] = (uint32_t)movie_async_buffer_bytes(movie);
    w[17] = (uint32_t)screenshot_writer_pending_bytes();
    w[18] = movie->diag_async_reads;
    w[19] = movie->diag_async_bytes;
    w[20] = movie->diag_async_waits;
    w[21] = movie->diag_async_failures;
    w[22] = (uint32_t)movie->diag_async_native_error;
    w[23] = movie->diag_async_cancels;
    w[24] = runtime.valid_flags | (io.minimum_stack_remaining != UINT32_MAX ? 32U : 0U);
    w[25] = runtime.dynamic_pool_total_bytes;
    w[26] = runtime.dynamic_pool_available_bytes;
    w[28] = io.minimum_stack_remaining;
    w[29] = io.io_phases;
    /* Payload v4 leaves retired OS scheduler/cursor/watchdog observations
     * reserved. Writer counters describe the player's cooperative contexts. */
    w[34] = screen.lcd_control;
    w[35] = screen.pwm_duty;
    w[36] = screen.pwm_period;
    w[37] = screen.pwm_control;
    w[38] = io.resumes;
    w[39] = io.max_step_ticks;
    w[40] = io.spi_yields;
    w[41] = io.max_callback_ticks;
    w[42] = io.errors;
    w[43] = journal.written;
    w[44] = journal.dropped;
    w[45] = journal.pending;
    w[46] = journal.failures;
    w[47] = (uint32_t)journal.native_error;
    w[48] = (uint32_t)crash_trace.last_present;
    w[49] = (uint32_t)(crash_trace.last_present >> 32);
    w[50] = (uint32_t)crash_trace.max_gap;
    w[51] = crash_trace.presents;
    w[52] = monotonic_clock_ticks_per_second();
    w[53] = crash_trace.minimum_free;
    w[54] = crash_trace.minimum_stack;
    w[55] = movie->diag_async_max_ticks;
    w[56] = (uint32_t)screen.last_status;
    w[57] = crash_trace_word(0x900C0008U);
    w[58] = (uint32_t)sram_bytes_used();
    w[59] = sram_expected_ttbr();
    w[60] = crash_trace.media_id;
    w[61] = movie->header.frame_count;
    w[62] = crash_trace_word(0x900C0080U);
    w[63] = sram_active_ttbr();
}

void player_crash_trace_begin(const Movie *movie, const char *path, bool paused,
                              const PlaybackRate *rate)
{
    char directory[MAX_PATH_LEN];
    uint64_t now;
    if (!debug_is_runtime_logging_enabled() || !movie || !path || !g_clock.using_hw_timer)
        return;
    if (crash_trace.active) {
        if (crash_trace.movie != movie)
            return;
        /* D can resume recording without joining a suspended file writer. */
        now = monotonic_clock_now_ticks();
        crash_trace.paused = paused;
        crash_trace.rate = rate;
        crash_trace.last_present = 0;
        crash_trace_snapshot(movie, PLAYER_CRASH_START, now);
        crash_recorder_submit(&crash_trace.last);
        crash_trace.next_sample = now + monotonic_clock_ticks_per_second();
        return;
    }
    player_crash_trace_end(NULL, PLAYER_CRASH_EXIT);
    debug_log_path_for_movie(path, directory, sizeof(directory));
    strip_filename(directory);
    now = monotonic_clock_now_ticks();
    uint64_t session = ((uint64_t)(uint32_t)time(NULL) << 32) | (uint32_t)now;
    memset(&crash_trace, 0, sizeof(crash_trace));
    crash_trace.attempted = true;
    if (!crash_recorder_start(directory, session))
        return;
    crash_trace.active = true;
    crash_trace.movie = movie;
    crash_trace.rate = rate;
    crash_trace.paused = paused;
    crash_trace.build_id = crash_trace_hash(__DATE__ " " __TIME__);
    crash_trace.media_id = crash_trace_hash(filename_from_path(path));
    crash_trace.minimum_free = crash_trace.minimum_stack = UINT32_MAX;
    crash_trace_snapshot(movie, PLAYER_CRASH_START, now);
    crash_recorder_submit(&crash_trace.last);
    crash_trace.next_sample = now + monotonic_clock_ticks_per_second();
}

void player_crash_trace_tick(const Movie *movie, unsigned phase, bool paused,
                             const PlaybackRate *rate)
{
    if (!crash_trace.active || crash_trace.movie != movie || !debug_is_runtime_logging_enabled())
        return;
    uint64_t now = monotonic_clock_now_ticks();
    crash_trace.paused = paused;
    crash_trace.rate = rate;
    if (paused || g_display_power_state.off || phase == PLAYER_CRASH_SEEK)
        crash_trace.last_present = 0;
    if (now < crash_trace.next_sample)
        return;
    crash_trace.next_sample = now + monotonic_clock_ticks_per_second();
    crash_trace_snapshot(movie, phase, now);
    crash_recorder_submit(&crash_trace.last);
}

void player_crash_trace_presented(const Movie *movie)
{
    if (crash_trace.active && crash_trace.movie == movie && debug_is_runtime_logging_enabled()) {
        uint64_t now = monotonic_clock_now_ticks();
        if (crash_trace.last_present && now - crash_trace.last_present > crash_trace.max_gap)
            crash_trace.max_gap = now - crash_trace.last_present;
        crash_trace.last_present = now;
        ++crash_trace.presents;
    }
}

void player_crash_trace_power_event(unsigned phase)
{
    if (!crash_trace.active || !crash_trace.movie || !debug_is_runtime_logging_enabled())
        return;
    if (phase != PLAYER_CRASH_OFF_BEGIN && phase != PLAYER_CRASH_OFF_END &&
        phase != PLAYER_CRASH_WAKE_BEGIN && phase != PLAYER_CRASH_WAKE_END)
        return;
    /* Transition checkpoints are independent of the periodic deadline. The
     * movie is still owned by the foreground until end() disables this trace;
     * submit copies the value and never sends that pointer to the writer.
     * A full queue drops this checkpoint rather than blocking a power change. */
    crash_trace.last_present = 0;
    crash_trace_snapshot(crash_trace.movie, phase, monotonic_clock_now_ticks());
    crash_recorder_submit(&crash_trace.last);
}

void player_crash_trace_end(const Movie *movie, unsigned phase)
{
    if (!crash_trace.attempted)
        return;
    if (crash_trace.active && movie && movie != crash_trace.movie)
        return;
    if (crash_trace.active && (!movie || movie == crash_trace.movie) &&
        debug_is_runtime_logging_enabled()) {
        if (movie)
            crash_trace_snapshot(movie, phase, monotonic_clock_now_ticks());
        else {
            crash_trace.last.words[3] = phase;
            crash_trace.last.monotonic_ticks = monotonic_clock_now_ticks();
        }
        /* Exit is already a drain/join boundary. Wait for a free copied slot
         * so a normal exit marker is not dropped behind older checkpoints. */
        CrashRecorderStats stats;
        do {
            crash_recorder_stats(&stats);
            if (!stats.active || stats.failed || stats.pending < CRASH_RECORDER_QUEUE_SLOTS)
                break;
            app_task_io_yield(NULL);
        } while (true);
        if (stats.active && !stats.failed)
            crash_recorder_submit(&crash_trace.last);
    }
    crash_trace.active = false;
    crash_trace.movie = NULL;
    crash_recorder_shutdown();
    crash_trace.attempted = false;
}
