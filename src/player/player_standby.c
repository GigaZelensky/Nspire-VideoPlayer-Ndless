#include "player_internal.h"
#include "native_standby_guard.h"
#include "native_standby.h"
#include "native_screen_power.h"
#include "screenshot_writer.h"

/* Ownership is needed only for the explicit native standby/resume bridge.
 * Normal picker/playback does not run the OS scheduler or acquire these holds. */
static NativeStandbyGuard standby_guard;
static bool standby_entered;
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
    if (native_standby_guard_acquire(&standby_guard)) {
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
        standby_entered = true;
        native_standby_run();
    }
}

bool player_standby(SDL_Surface *screen, Movie *movie, const char *path, bool is_directory,
                    ScreenshotPreviewState *preview)
{
    if (!native_screen_power_supported() || !native_standby_supported() || !g_clock.using_hw_timer)
        return false;
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
    mapped = sram_with_native_mapping(standby_bridge, NULL) && standby_entered;
    *g_clock.control_reg = 0;
    *g_clock.speed_reg = MONOTONIC_TIMER_CLOCK_SOURCE_32768HZ;
    *g_clock.control_reg = MONOTONIC_TIMER_CONTROL_ENABLE_32BIT;
    g_clock.last_value = *g_clock.value_reg;

    int wake_result = native_screen_power_on(LCD_BRIGHTNESS_MAX);
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
    bool succeeded = mapped && !native_standby_status() && !wake_result;
    if (!succeeded)
        debug_tracef("native standby refused/restore failed mapping=%u status=%d wake=%d", mapped,
                     mapped ? native_standby_status() : -1, wake_result);
    /* A refused transition must reach the caller's normal OS standby path.
     * Returning success here would wake the screen and restart its idle timer
     * even though the calculator had never entered standby. */
    return succeeded;
}
