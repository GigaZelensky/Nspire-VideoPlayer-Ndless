#include "player_internal.h"
#include "storage_descriptors.h"
#include "storage_read_stream.h"
#include "performance_clock.h"
#include "movie_crypto_session.h"
#if NDVIDEO_CODEC_MODULES
#include "codecs/codec_module_api.h"
#include "codec_module_manifest.h"
#endif

int main(int argc, char **argv)
{
    SDL_Surface *screen;
    Fonts fonts;
    char movie_path[MAX_PATH_LEN];
    char queued_movie_path[MAX_PATH_LEN] = {0};
    char directory[MAX_PATH_LEN];
    int result = 0;
    bool have_queued_movie = false;
    bool resume_without_prompt = false;
    bool picker_opened_loading = false;
    bool return_home_after_exit = false;
    bool open_scratchpad_after_exit = false;
    bool suspend_after_exit = false;

    if (argc < 1) {
        report_app_failure(NULL, "startup-failure", "Ndless did not provide argv[0].");
        return 1;
    }

    enable_relative_paths(argv);
#if NDVIDEO_CODEC_MODULES
    if (!codec_modules_configure(argv[0], codec_modules_default_host_api()) ||
        !codec_modules_set_expected_hashes(CODEC_MODULE_EXPECTED_HASHES, CODEC_MODULE_EXPECTED_MASK)) {
        report_app_failure(argv[0], "startup-failure", codec_module_error());
        return 1;
    }
#endif
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        report_app_failure(argv[0], "startup-failure", "Failed to initialize SDL.");
        return 1;
    }
    monotonic_clock_init();
    bool keep_clock;
    unsigned saved_clock = history_load_clock_preference(argv[0], &keep_clock);
    performance_clock_load(saved_clock, keep_clock);
    performance_clock_startup_begin();
    screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 16, SDL_SWSURFACE);
    if (!screen) {
        SDL_Quit();
        performance_clock_restore();
        monotonic_clock_shutdown();
        report_app_failure(argv[0], "startup-failure", "Failed to create the screen surface.");
        return 1;
    }
    if (!lcd_init(screen_lcd_type())) {
        SDL_Quit();
        performance_clock_restore();
        monotonic_clock_shutdown();
        report_app_failure(argv[0], "startup-failure", "Failed to initialize the LCD.");
        return 1;
    }
    patch_cx2_lcd_edge_timing();
    display_power_init(&g_display_power_state, monotonic_clock_now_ms());
    if (!init_fonts(&fonts)) {
        lcd_init(SCR_TYPE_INVALID);
        SDL_Quit();
        performance_clock_restore();
        monotonic_clock_shutdown();
        report_app_failure(argv[0], "startup-failure", "Failed to load fonts.");
        return 1;
    }
    night_mode_init(&fonts);

    strncpy(directory, argv[0], sizeof(directory) - 1);
    directory[sizeof(directory) - 1] = '\0';
    strip_filename(directory);
    ui_load_theme_for_directory(directory);
    /* Native startup may reject an early transition or change the clock after
     * it. Reconcile once here, with genuine SRAM and before app I/O begins. */
    performance_clock_startup_complete();

    while (1) {
        resume_without_prompt = false;
        picker_opened_loading = false;
        if (have_queued_movie) {
            strncpy(movie_path, queued_movie_path, sizeof(movie_path) - 1);
            movie_path[sizeof(movie_path) - 1] = '\0';
            have_queued_movie = false;
        } else if (argc > 1) {
            strncpy(movie_path, argv[1], sizeof(movie_path) - 1);
            movie_path[sizeof(movie_path) - 1] = '\0';
        } else {
            int picker_result = pick_movie(
                    screen,
                    &fonts,
                    directory,
                    movie_path,
                    sizeof(movie_path),
                    &resume_without_prompt);

            if (picker_result == PLAY_MOVIE_RESULT_HOME_EXIT ||
                picker_result == PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT ||
                picker_result == PLAY_MOVIE_RESULT_SUSPEND_EXIT) {
                result = picker_result;
                return_home_after_exit = true;
                open_scratchpad_after_exit = picker_result == PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT;
                suspend_after_exit = picker_result == PLAY_MOVIE_RESULT_SUSPEND_EXIT;
                break;
            }
            if (picker_result != 0) {
                result = PLAY_MOVIE_RESULT_APP_EXIT;
                break;
            }
            picker_opened_loading = true;
        }
        if (!picker_opened_loading) {
            int unlocked = unlock_movie_prompt(screen, &fonts, movie_path, NULL, NULL);
            if (unlocked != 1) {
                movie_crypto_clear();
                if (!unlocked) { argc = 1; continue; }
                result = unlocked;
                return_home_after_exit = true;
                open_scratchpad_after_exit = result == PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT;
                suspend_after_exit = result == PLAY_MOVIE_RESULT_SUSPEND_EXIT;
                break;
            }
        }
        result = play_movie(
            screen,
            &fonts,
            movie_path,
            queued_movie_path,
            sizeof(queued_movie_path),
            resume_without_prompt,
            picker_opened_loading
        );
        /* D records one movie. Its saved log and journal must survive an
         * automatic switch to the next file in the directory. */
        debug_set_runtime_logging(false);
        /* All movie and preview read handles have closed, including on error
         * and on return to the picker. Never carry a key into the next video. */
        movie_crypto_clear();
        argc = 1;
        if (result == PLAY_MOVIE_RESULT_ERROR && player_message_pending()) {
            /* Movie cleanup has finished. Let the picker show the error over
             * its own scene, without retrying a direct launch or auto-next. */
            have_queued_movie = false;
            queued_movie_path[0] = '\0';
            continue;
        }
        if (result == PLAY_MOVIE_RESULT_AUTO_NEXT ||
            result == PLAY_MOVIE_RESULT_SWITCH_MOVIE) {
            have_queued_movie = true;
            continue;
        }
        if (result == PLAY_MOVIE_RESULT_HOME_EXIT ||
            result == PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT ||
            result == PLAY_MOVIE_RESULT_SUSPEND_EXIT) {
            return_home_after_exit = true;
            open_scratchpad_after_exit = result == PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT;
            suspend_after_exit = result == PLAY_MOVIE_RESULT_SUSPEND_EXIT;
            break;
        }
        if (result == PLAY_MOVIE_RESULT_APP_EXIT) {
            break;
        }
        if (result != PLAY_MOVIE_RESULT_EXIT) {
            break;
        }
    }

    player_crash_trace_end(NULL, PLAYER_CRASH_EXIT);
    screenshot_writer_shutdown();
    flush_queued_history_save(&g_pending_history_save, "shutdown");
    flush_queued_theme_save("shutdown");
    if (g_display_power_state.off || g_display_power_state.off_fade_active) {
        /* Retain video for normal wake, but do not flash it while exiting to
         * the OS from an off screen. The SDL surface is still owned here. */
        SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
    }
    display_power_restore(&g_display_power_state, monotonic_clock_now_ms());
    player_standby_shutdown();
    cleanup_deferred_playback_movie();
    clear_movie_picker_cache(&g_picker_cache);
#if NDVIDEO_CODEC_MODULES
    char module_shutdown_error[192] = {0};
    if (!codec_modules_shutdown())
        snprintf(module_shutdown_error, sizeof(module_shutdown_error), "%s", codec_module_error());
#endif
    night_mode_shutdown();
    playback_capture_release();
    release_debug_ring_storage();
    free_fonts(&fonts);
    storage_descriptors_shutdown();
    storage_read_stream_shutdown();
    movie_crypto_clear();
    lcd_init(SCR_TYPE_INVALID);
    SDL_Quit();
    sram_shutdown();
#if NDVIDEO_CODEC_MODULES
    if (module_shutdown_error[0])
        report_app_failure(argv[0], "codec-cleanup-failure", module_shutdown_error);
#endif
    bool normal_exit = !suspend_after_exit &&
        (result == PLAY_MOVIE_RESULT_EXIT || result == PLAY_MOVIE_RESULT_APP_EXIT ||
         result == PLAY_MOVIE_RESULT_HOME_EXIT || result == PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT);
    bool clock_finished = performance_clock_finish(normal_exit);
    if (clock_finished && normal_exit) {
        if (!history_save_clock_preference(argv[0], performance_clock_selection(), performance_clock_keep_after_exit()))
            report_app_failure(argv[0], "settings-save-failure", "Could not save the clock preference.");
    }
    monotonic_clock_shutdown();
    show_pending_movie_error();
    /* Explicit Home/Scratchpad/standby navigation also belongs after cleanup. */
    if (return_home_after_exit) {
        if (open_scratchpad_after_exit)
            yes_teacher_im_mathing();
        else
            return_to_os_home_menu();
        if (suspend_after_exit)
            queue_os_suspend_shortcut();
    } else {
        queue_os_redraw();
    }
    return (result == PLAY_MOVIE_RESULT_EXIT ||
        result == PLAY_MOVIE_RESULT_AUTO_NEXT ||
        result == PLAY_MOVIE_RESULT_SWITCH_MOVIE ||
        result == PLAY_MOVIE_RESULT_APP_EXIT ||
        result == PLAY_MOVIE_RESULT_HOME_EXIT ||
        result == PLAY_MOVIE_RESULT_SCRATCHPAD_EXIT ||
        result == PLAY_MOVIE_RESULT_SUSPEND_EXIT) ? 0 : 1;
}
