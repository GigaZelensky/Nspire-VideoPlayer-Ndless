#include "player_internal.h"
#include "native_standby_guard.h"
#include "native_standby.h"
#include "native_screen_power.h"
#include "screenshot_writer.h"
#include "performance_clock.h"

/* Ownership is needed only for the explicit native standby/resume bridge.
 * Normal picker/playback does not run the OS scheduler or acquire these holds. */
static NativeStandbyGuard standby_guard;
static bool standby_entered;
static int standby_clock_status, standby_guard_status;
static int standby_screen_profile, standby_preflight, standby_mapping, standby_wake;
static unsigned standby_attempts, standby_native_calls;
static uint32_t standby_off_age_ms;
static void standby_release(void *unused)
{
    (void)unused;
    while (native_standby_guard_release(&standby_guard)) {
    }
}
void player_standby_shutdown(void)
{
    if (standby_guard.supported)
        (void)sram_with_native_mapping(standby_release, NULL);
}

static bool standby_acquire(void)
{
    if (standby_guard.active)
        return true;
    standby_guard_status = native_standby_guard_acquire(&standby_guard);
    if (standby_guard_status) {
        standby_release(NULL);
        return false;
    }
    return true;
}

static void standby_bridge(void *unused)
{
    (void)unused;
    /* The only interrupt-enabled OS spans execute with genuine SRAM mapped. */
    if (standby_acquire()) {
        if (!performance_clock_restore()) {
            standby_clock_status = performance_clock_status();
            return;
        }
        standby_entered = true;
        ++standby_native_calls;
        native_standby_run();
        if (!performance_clock_start())
            standby_clock_status = performance_clock_status();
    }
}

/* Export into the explicit D recording, never a separate unsolicited file. */
void player_standby_debug(FILE *file)
{
    if (!file || !standby_attempts) return;
    fprintf(file, "standby screen=%d preflight=%d mapping=%d entered=%u guard=%d guard_stage=%s clock=%d wake=%d attempts=%u native_calls=%u screen_off_age_ms=%lu\n",
        standby_screen_profile, standby_preflight, standby_mapping, standby_entered,
        standby_guard_status, native_standby_guard_stage(), standby_clock_status, standby_wake,
        standby_attempts, standby_native_calls, (unsigned long)standby_off_age_ms);
    native_standby_guard_debug(file);
    native_standby_debug(file);
}

bool player_standby(SDL_Surface *screen, Movie *movie, const char *path, bool is_directory,
                    ScreenshotPreviewState *preview)
{
    ++standby_attempts;
    standby_off_age_ms = monotonic_clock_now_ms() - g_display_power_state.off_started_ms;
    standby_entered = false;
    standby_guard_status = standby_clock_status = 0;
    standby_mapping = standby_wake = 0;
    standby_screen_profile = native_screen_power_supported();
    standby_preflight = native_standby_preflight_status();
    if (!standby_screen_profile || standby_preflight || !g_clock.using_hw_timer) {
        debug_tracef("native standby preflight failed screen=%d status=%d timer=%u",
            standby_screen_profile, standby_preflight, g_clock.using_hw_timer);
        return false;
    }
    bool had_reader = movie_async_enabled(movie);
    bool retained_reader = movie_async_suspend(movie);
    player_crash_trace_end(movie, PLAYER_CRASH_STANDBY);
    screenshot_writer_drain();
    screenshot_preview_tick(preview, monotonic_clock_now_ms());
    screenshot_writer_shutdown();
    flush_queued_history_save(&g_pending_history_save, "standby");
    flush_queued_theme_save("standby");

    (void)is_directory;
    /* Return the timer to its entry configuration for the native drivers.
     * Keep the monotonic accumulator and original shutdown snapshot intact. */
    monotonic_clock_now_ticks();
    *g_clock.control_reg = 0;
    *g_clock.speed_reg = g_clock.original_speed;
    *g_clock.control_reg = g_clock.original_control;
    bool mapped = false;
    standby_entered = false;
    standby_clock_status = 0;
    mapped = sram_with_native_mapping(standby_bridge, NULL);
    standby_mapping = mapped;
    *g_clock.control_reg = 0;
    *g_clock.speed_reg = MONOTONIC_TIMER_CLOCK_SOURCE_32768HZ;
    *g_clock.control_reg = MONOTONIC_TIMER_CONTROL_ENABLE_32BIT;
    g_clock.last_value = *g_clock.value_reg;

    int wake_result = native_screen_power_on(LCD_BRIGHTNESS_MAX);
    standby_wake = wake_result;
    /* Start only after both the physical pool and hardware timer are ready.
     * Existing decoded/prefetched data stays resident; no seek or re-decode. */
    if (retained_reader)
        movie_async_resume(movie);
    else if (had_reader)
        movie_async_start(movie, path);
    if (!wake_result)
        present_screen(screen);
    uint32_t now = monotonic_clock_now_ms();
    display_power_restore_animated(&g_display_power_state, now);
    if (g_display_power_state.off)
        g_display_power_state.off_started_ms = now;
    bool succeeded = mapped && standby_entered && !native_standby_status() && !wake_result;
    if (!succeeded || standby_clock_status) {
        debug_tracef("native standby refused/restore failed mapping=%u entered=%u status=%d wake=%d clock=%d", mapped, standby_entered,
                     standby_entered ? native_standby_status() : -1, wake_result, standby_clock_status);
    }
    /* A refused transition must reach the caller's normal OS standby path.
     * Returning success here would wake the screen and restart its idle timer
     * even though the calculator had never entered standby. */
    return succeeded;
}
