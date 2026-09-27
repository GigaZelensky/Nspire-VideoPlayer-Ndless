#include "player_internal.h"
#include "screenshot_encode.h"
#include "screenshot_writer.h"

/* Follow the displayed content, including cached transition backgrounds,
 * rather than just the lifetime of the decryption key. */
static bool protected_content_visible;

void screenshot_set_protected_content(bool protected_content)
{
    protected_content_visible = protected_content;
}

static void screenshot_notice(ScreenshotPreviewState *preview, const char *message, uint32_t now)
{
    if (!preview)
        return;
    uint32_t started = preview->label[0] ? preview->started_ms : 0;
    if (!preview->request_id)
        clear_screenshot_preview(preview);
    /* Repeated requests extend the current notice without replaying entry. */
    preview->started_ms = started ? started : (now ? now : 1U);
    snprintf(preview->label, sizeof(preview->label), "%s", message);
    if (!preview->request_id)
        preview->until_ms = now + SCREENSHOT_PREVIEW_MS;
    ++preview->revision;
}

bool screenshot_preview_animating(const ScreenshotPreviewState *preview, uint32_t now_ms)
{
    if (!preview || preview->surface || !preview->label[0] || !preview->started_ms)
        return false;
    return (uint32_t)(now_ms - preview->started_ms) < STATUS_BADGE_ANIM_MS ||
        (!preview->request_id && (uint32_t)(now_ms - preview->until_ms) < STATUS_BADGE_EXIT_ANIM_MS);
}

bool screenshot_preview_tick(ScreenshotPreviewState *preview, uint32_t now_ms)
{
    ScreenshotWriteResult result;
    bool changed = false;
    while (screenshot_writer_poll(&result)) {
        if (playback_capture_active(NULL) && g_clock.using_hw_timer) {
            uint64_t now_ticks = monotonic_clock_now_ticks();
            uint32_t raw_now = *(volatile uint32_t *)MONOTONIC_TIMER_VALUE_ADDR;
            uint32_t age = result.finished_counter - raw_now;
            uint32_t elapsed = result.started_counter - result.finished_counter;
            if (now_ticks >= (uint64_t)age + elapsed)
                playback_capture_io(NULL, CAPTURE_IO_ASYNC_SCREENSHOT, -1, result.bytes,
                                    now_ticks - age - elapsed, now_ticks - age);
        }
        debug_tracef("screenshot %s id=%lu native_error=%d path=%s",
                     result.success ? "saved" : "failed", (unsigned long)result.id,
                     result.native_error, result.path);
        if (preview && preview->request_id == result.id) {
            preview->request_id = 0;
            if (result.success)
                snprintf(preview->label, sizeof(preview->label), "Saved %.72s",
                         filename_from_path(result.path));
            else
                snprintf(preview->label, sizeof(preview->label), "Screenshot failed");
            preview->until_ms = now_ms + SCREENSHOT_PREVIEW_MS;
            ++preview->revision;
            changed = true;
        }
    }
    if (preview && preview->label[0] && !preview->request_id &&
        (int32_t)(now_ms - (preview->until_ms + (preview->surface ? 0U : STATUS_BADGE_EXIT_ANIM_MS))) > 0) {
        clear_screenshot_preview(preview);
        changed = true;
    }
    return changed;
}

void request_screenshot_in_directory(SDL_Surface *screen, const char *directory,
                                     ScreenshotPreviewState *preview)
{
    unsigned char *bmp = NULL;
    size_t bytes = 0;
    uint32_t id = 0;
    uint32_t now = monotonic_clock_now_ms();
    ScreenshotWriteStatus status = SCREENSHOT_WRITE_UNSUPPORTED;
    char saved_path[MAX_PATH_LEN];
    screenshot_preview_tick(preview, now);
    if (!screen || !preview)
        return;
    if (protected_content_visible) {
        if (preview->request_id) clear_screenshot_preview(preview);
        screenshot_notice(preview, "Screenshots disabled for locked videos", now);
        preview->until_ms = now + 2000U;
        return;
    }
    if (!screenshot_writer_has_capacity()) {
        screenshot_notice(
            preview, preview->request_id ? "Saving (queue full)" : "Screenshot queue full", now);
        return;
    }
    if (g_clock.using_hw_timer) {
        bool recording = playback_capture_active(NULL);
        uint64_t started = recording ? monotonic_clock_now_ticks() : 0;
        bool encoded = screenshot_encode_bmp(screen, &bmp, &bytes);
        if (recording)
            playback_capture_stage(NULL, CAPTURE_SCREENSHOT, started, monotonic_clock_now_ticks());
        if (!encoded) {
            screenshot_notice(
                preview, preview->request_id ? "Saving; new capture failed" : "Screenshot failed",
                now);
            return;
        }
        status = screenshot_writer_submit(directory, bmp, bytes, &id);
        if (status != SCREENSHOT_WRITE_ACCEPTED)
            free(bmp);
    }
    if (status == SCREENSHOT_WRITE_ACCEPTED) {
        /* Only the encoded byte buffer belongs to the worker. SDL objects
         * remain foreground-owned and can be cleared when views change. */
        clear_screenshot_preview(preview);
        preview->surface = create_scaled_surface_from_surface(screen, SCREENSHOT_PREVIEW_MAX_W,
                                                              SCREENSHOT_PREVIEW_MAX_H);
        preview->request_id = id;
        screenshot_notice(preview, "Saving screenshot...", now);
        return;
    }
    if (status == SCREENSHOT_WRITE_UNSUPPORTED) {
        /* Preserve screenshots on firmware without the verified native ABI.
         * Supported-platform OOM/queue/write errors never block via fallback. */
        if (save_screenshot_bitmap_in_directory(screen, directory, saved_path,
                                                sizeof(saved_path))) {
            prepare_screenshot_preview(preview, screen, saved_path);
            return;
        }
    }
    screenshot_notice(
        preview, preview->request_id ? "Saving; new capture failed" : "Screenshot failed", now);
}

void request_screenshot(SDL_Surface *screen, const char *movie_path,
                        ScreenshotPreviewState *preview)
{
    char directory[MAX_PATH_LEN];
    if (movie_path && movie_path[0]) {
        snprintf(directory, sizeof(directory), "%s", movie_path);
        strip_filename(directory);
    } else {
        snprintf(directory, sizeof(directory), ".");
    }
    request_screenshot_in_directory(screen, directory, preview);
}
