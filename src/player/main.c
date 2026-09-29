#include "player_internal.h"
#include "storage_descriptors.h"
#include "storage_read_stream.h"
#include "performance_clock.h"
#include "movie_crypto_session.h"

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
        show_msgbox("ND Video Player", "Ndless did not provide argv[0].");
        return 1;
    }

    enable_relative_paths(argv);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        show_msgbox("ND Video Player", "Failed to initialize SDL.");
        return 1;
    }
    monotonic_clock_init();
    performance_clock_start();
    screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 16, SDL_SWSURFACE);
    if (!screen) {
        show_msgbox("ND Video Player", "Failed to create the screen surface.");
        SDL_Quit();
        performance_clock_restore();
        monotonic_clock_shutdown();
        return 1;
    }
    if (!lcd_init(screen_lcd_type())) {
        show_msgbox("ND Video Player", "Failed to initialize the LCD.");
        SDL_Quit();
        performance_clock_restore();
        monotonic_clock_shutdown();
        return 1;
    }
    patch_cx2_lcd_edge_timing();
    display_power_init(&g_display_power_state, monotonic_clock_now_ms());
    if (!init_fonts(&fonts)) {
        show_msgbox("ND Video Player", "Failed to load fonts.");
        lcd_init(SCR_TYPE_INVALID);
        SDL_Quit();
        performance_clock_restore();
        monotonic_clock_shutdown();
        return 1;
    }
    night_mode_init(&fonts);

    strncpy(directory, argv[0], sizeof(directory) - 1);
    directory[sizeof(directory) - 1] = '\0';
    strip_filename(directory);
    ui_load_theme_for_directory(directory);

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
        /* All movie and preview read handles have closed, including on error
         * and on return to the picker. Never carry a key into the next video. */
        movie_crypto_clear();
        argc = 1;
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
    performance_clock_restore();
    monotonic_clock_shutdown();
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
