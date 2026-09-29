#include "player_internal.h"
#include "movie_async_io.h"
#include "raw_player_io.h"
#include "app_task_io.h"
#include "player_idle.h"

typedef struct MovieAsyncIo {
    RawPlayerIo *raw;
} MovieAsyncIo;

bool movie_async_enabled(const Movie *movie)
{
    return movie && movie->async_io;
}
size_t movie_async_buffer_bytes(const Movie *movie)
{
    return movie_async_enabled(movie) ? raw_player_memory_bytes() : 0;
}
void player_delay_ms(unsigned milliseconds)
{
    if (!raw_player_idle(milliseconds))
        player_idle_sleep(milliseconds);
}
void movie_async_service(Movie *movie, unsigned budget_ticks)
{
    if (movie_async_enabled(movie))
        raw_player_service(budget_ticks);
}
bool movie_async_crypto_step(Movie *movie, uint32_t spare_ticks)
{
    return movie_async_enabled(movie) && raw_player_crypto_step(spare_ticks);
}
bool movie_async_start(Movie *movie, const char *path)
{
    if (!movie || movie->async_io || !g_clock.using_hw_timer || !path)
        return false;
    MovieAsyncIo *state = calloc(1, sizeof(*state));
    if (!state)
        return false;
    state->raw = raw_player_create(path);
    if (!state->raw) {
        debug_tracef(
            "independent reader unavailable reason=%d; foreground independent reads only",
            raw_player_error(NULL));
        free(state);
        return false;
    }
    movie->async_io = state;
    movie->diag_async_used = true;
    debug_tracef("independent reader active; player-owned storage scheduling");
    return true;
}
bool movie_async_suspend(Movie *movie)
{
    if (!movie_async_enabled(movie))
        return false;
    raw_player_before_native();
    return true;
}
void movie_async_resume(Movie *movie)
{
    MovieAsyncIo *state = movie ? movie->async_io : NULL;
    if (state)
        raw_player_after_clock_reset(state->raw);
}
void movie_async_cancel(Movie *movie)
{
    MovieAsyncIo *state = movie ? movie->async_io : NULL;
    if (state) {
        raw_player_cancel(state->raw);
        ++movie->diag_async_cancels;
    }
}
void movie_async_stop(Movie *movie)
{
    MovieAsyncIo *state = movie ? movie->async_io : NULL;
    if (!state)
        return;
    raw_player_destroy(state->raw);
    movie->async_io = NULL;
    free(state);
}

static void collect_async_read(Movie *movie, MovieAsyncIo *state, int chunk, size_t bytes)
{
    uint32_t started, ended, actual_bytes;
    ++movie->diag_async_reads;
    movie->diag_async_bytes += (uint32_t)bytes;
    if (!raw_player_last_read(state->raw, &started, &ended, &actual_bytes))
        return;
    uint32_t elapsed = started - ended; /* Hardware downcounter; one read <36h. */
    movie->last_read_bytes = actual_bytes;
    movie->last_read_time_ms = (uint32_t)((uint64_t)elapsed * 1000U / TIMER_TICKS_PER_SEC);
    if (elapsed > movie->diag_async_max_ticks)
        movie->diag_async_max_ticks = elapsed;
    if (playback_capture_active(movie)) {
        uint64_t now = monotonic_clock_now_ticks();
        uint32_t raw_now = *(volatile uint32_t *)MONOTONIC_TIMER_VALUE_ADDR;
        uint32_t age = ended - raw_now;
        if (now >= (uint64_t)age + elapsed) {
            /* Async wall time is not foreground IO/decode work. */
            playback_capture_io(movie, CAPTURE_IO_ASYNC, chunk, actual_bytes, now - age - elapsed,
                                now - age);
        }
    }
}

int movie_async_read(Movie *movie, uint64_t offset, void *destination, size_t bytes,
                     int chunk_index, bool wait)
{
    MovieAsyncIo *state = movie ? movie->async_io : NULL;
    if (!state)
        return MOVIE_ASYNC_DISABLED;
    uint64_t started = wait && playback_capture_active(movie) ? monotonic_clock_now_ticks() : 0;
    int result = raw_player_read(state->raw, offset, destination, bytes, false);
    if (result == MOVIE_ASYNC_PENDING && wait) {
        ++movie->diag_async_waits;
        result = raw_player_read(state->raw, offset, destination, bytes, true);
        RawPlayerWaitStats cost;
        if (debug_is_runtime_logging_enabled() && raw_player_last_wait(state->raw, &cost) &&
            cost.wall_ticks >= TIMER_TICKS_PER_SEC / 200U) {
            debug_tracef("read_wait chunk=%d n=%lu wall=%lu read=%lu crypt=%lu writer=%lu pages=%lu regions=%lu views=%lu phase=%lu units=us",
                chunk_index, (unsigned long)bytes,
                (unsigned long)((uint64_t)cost.wall_ticks * 1000000U / TIMER_TICKS_PER_SEC),
                (unsigned long)((uint64_t)cost.reader_ticks * 1000000U / TIMER_TICKS_PER_SEC),
                (unsigned long)((uint64_t)cost.crypto_ticks * 1000000U / TIMER_TICKS_PER_SEC),
                (unsigned long)((uint64_t)cost.writer_service_ticks * 1000000U / TIMER_TICKS_PER_SEC),
                (unsigned long)cost.physical_reads, (unsigned long)cost.region_rebuilds,
                (unsigned long)cost.view_captures, (unsigned long)cost.start_phase);
        }
    }
    if (result == MOVIE_ASYNC_READY)
        collect_async_read(movie, state, chunk_index, bytes);
    if (started)
        playback_capture_stage(movie, CAPTURE_IO, started, monotonic_clock_now_ticks());
    if (result < 0) {
        ++movie->diag_async_failures;
        movie->diag_async_native_error = raw_player_error(state->raw);
        debug_tracef("independent reader error=%d; switching to foreground independent reads",
                     movie->diag_async_native_error);
        movie_async_stop(movie);
    }
    return result;
}
void movie_async_debug(FILE *file, const Movie *movie)
{
    MovieAsyncIo *state = movie ? movie->async_io : NULL;
    if (state || (movie && movie->diag_async_used))
        raw_player_debug(file, state ? state->raw : NULL);
    app_task_io_debug(file);
    player_idle_debug(file);
}
