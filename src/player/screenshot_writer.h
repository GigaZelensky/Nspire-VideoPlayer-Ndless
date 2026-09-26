#ifndef NDVIDEO_SCREENSHOT_WRITER_H
#define NDVIDEO_SCREENSHOT_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCREENSHOT_WRITER_PATH_CAP 512U
#define SCREENSHOT_WRITER_QUEUE_SLOTS 2U
#define SCREENSHOT_WRITER_MAX_BMP_BYTES (256U * 1024U)

typedef enum {
    SCREENSHOT_WRITE_ACCEPTED = 0,
    SCREENSHOT_WRITE_BUSY,
    SCREENSHOT_WRITE_UNSUPPORTED,
    SCREENSHOT_WRITE_INVALID,
    SCREENSHOT_WRITE_FAILED
} ScreenshotWriteStatus;

typedef struct {
    uint32_t id;
    bool success;
    int native_error;
    /* Raw 32768 Hz hardware downcounter around the whole filesystem job;
     * includes filename lookup, IO and time descheduled. bytes is the actual
     * counted write amount, including a valid partial write on failure. */
    uint32_t started_counter, finished_counter, bytes;
    char path[SCREENSHOT_WRITER_PATH_CAP];
} ScreenshotWriteResult;

/* Foreground-only singleton API. ACCEPTED transfers ownership of an immutable
 * malloc-allocated BMP; every other return leaves ownership with the caller.
 * Directory is copied before return. A queued/writing/completed-but-unpolled
 * job occupies one of two slots. No synchronous file fallback is performed.
 * One app-owned context starts lazily and sleeps until signaled when empty.
 * No OS worker is created. Submit/poll never join it;
 * only explicit shutdown may wait for native task exit. */
ScreenshotWriteStatus screenshot_writer_submit(const char *directory, uint8_t *bmp, size_t bytes,
                                               uint32_t *id);
/* Check before allocating/encoding a new BMP. Only the foreground submits,
 * so capacity cannot disappear while it encodes. */
bool screenshot_writer_has_capacity(void);
/* Poll each foreground UI iteration. Completion publishes only after close
 * (and attempted partial-file removal on failure); polling frees the BMP.
 * A false return never modifies result. Result does not own any allocation. */
bool screenshot_writer_poll(ScreenshotWriteResult *result);
size_t screenshot_writer_pending_bytes(void);
/* Drain accepted jobs and retry join/destruction before freeing any storage.
 * Call before OS dialogs/transitions, raw timer/LCD/SRAM teardown or unload.
 * Outstanding completion records are discarded. Safe to call repeatedly. */
void screenshot_writer_shutdown(void);
/* Join and close while retaining completion records for a final UI poll.
 * Follow with shutdown after polling; submissions stay disabled meanwhile. */
void screenshot_writer_drain(void);

#endif
