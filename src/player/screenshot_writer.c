#include "screenshot_writer.h"
#include "app_task_io.h"
#include "storage_activity.h"

#include <stdlib.h>
#include <limits.h>
#include <string.h>

enum { SHOT_FREE, SHOT_QUEUED, SHOT_WRITING, SHOT_DONE };
enum { SHOT_IO_ERROR = -2100, SHOT_NO_NAME = -2101 };
#define SHOT_WRITE_BLOCK_BYTES 32768U

typedef struct {
    unsigned state;
    uint8_t *bmp;
    size_t bytes;
    size_t prefix_bytes;
    ScreenshotWriteResult result;
} ScreenshotJob;

typedef struct {
    AppTaskIo native;
    ScreenshotJob jobs[SCREENSHOT_WRITER_QUEUE_SLOTS];
    uint32_t next_id;
    /* Foreground sets this only during shutdown, under the queue lock. The
     * worker drains already accepted jobs before exiting. */
    bool stopping;
} ScreenshotWriter;

static ScreenshotWriter writer;

static uint32_t screenshot_raw_counter(void)
{
#if defined(__arm__)
    return *(const volatile uint32_t *)0x900C0004U;
#else
    return 0;
#endif
}

static ScreenshotJob *oldest_job(unsigned state)
{
    ScreenshotJob *oldest = NULL;
    unsigned i;
    for (i = 0; i < SCREENSHOT_WRITER_QUEUE_SLOTS; ++i) {
        ScreenshotJob *job = &writer.jobs[i];
        if (job->state == state && (!oldest || (int32_t)(job->result.id - oldest->result.id) < 0))
            oldest = job;
    }
    return oldest;
}

/* Worker helpers perform no SDK calls, allocation, formatting or clock work.
 * Each path already has its foreground-copied directory and separator. */
static void write_job(ScreenshotJob *job)
{
    static const char suffix[] = "ndvideo-shot-0000.bmp.tns";
    AppTaskIo *native = &writer.native;
    char *name = job->result.path + job->prefix_bytes;
    unsigned number, i;
    void *file;
    size_t offset;
    int close_error, error = 0;
    for (i = 0; i < sizeof(suffix); ++i)
        name[i] = suffix[i];
    for (number = 1; number <= 9999U; ++number) {
        unsigned decimal = number;
        for (i = 17U; i > 13U; --i) {
            name[i - 1U] = (char)('0' + decimal % 10U);
            decimal /= 10U;
        }
        int lookup_error = 0;
        int exists = app_task_io_file_exists(native, job->result.path, &lookup_error);
        if (exists < 0) {
            job->result.native_error = lookup_error ? lookup_error : SHOT_IO_ERROR;
            return;
        }
        if (!exists)
            break;
        /* A directory with many existing screenshots must also yield. */
        app_task_io_yield(native);
    }
    if (number > 9999U) {
        job->result.native_error = SHOT_NO_NAME;
        return;
    }
    file = app_task_io_file_open_write(native, job->result.path);
    if (!file) {
        job->result.native_error = SHOT_IO_ERROR;
        return;
    }
    offset = 0;
    while (offset < job->bytes) {
        size_t count = job->bytes - offset;
        if (count > SHOT_WRITE_BLOCK_BYTES)
            count = SHOT_WRITE_BLOCK_BYTES;
        size_t written = app_task_io_file_write(native, file, job->bmp + offset, count);
        if (written <= count)
            job->result.bytes += (uint32_t)written;
        if (written != count) {
            error = SHOT_IO_ERROR;
            break;
        }
        offset += count;
        app_task_io_yield(native);
    }
    close_error = app_task_io_file_close(native, file);
    if (!error && close_error)
        error = close_error;
    if (error) {
        int remove_error = app_task_io_file_remove(native, job->result.path);
        job->result.native_error = remove_error ? remove_error : error;
        return;
    }
    job->result.success = true;
}

static void screenshot_writer_worker(void *argument)
{
    (void)argument;
    for (;;) {
        unsigned saved = app_task_io_critical_enter();
        ScreenshotJob *job = oldest_job(SHOT_QUEUED);
        if (!job) {
            bool stopping = writer.stopping;
            app_task_io_critical_leave(saved);
            if (stopping)
                return;
            unsigned received = 0;
            /* In-process signals cannot be lost. Sleep until a real submission
             * or stop; idle polling would repeatedly disrupt the raw reader. */
            int error = app_task_io_wait(&writer.native, 1U, true, UINT_MAX, &received);
            if (error && error != -36 && error != -50)
                app_task_io_sleep(&writer.native, 1U);
            continue;
        }
        job->state = SHOT_WRITING;
        app_task_io_critical_leave(saved);
        job->result.started_counter = screenshot_raw_counter();
        storage_native_begin();
        write_job(job);
        storage_native_end();
        job->result.finished_counter = screenshot_raw_counter();
        saved = app_task_io_critical_enter();
        job->state = SHOT_DONE;
        app_task_io_critical_leave(saved);
    }
}

bool screenshot_writer_has_capacity(void)
{
    unsigned saved = app_task_io_critical_enter();
    bool available = !writer.stopping && oldest_job(SHOT_FREE) != NULL;
    app_task_io_critical_leave(saved);
    return available;
}

ScreenshotWriteStatus screenshot_writer_submit(const char *directory, uint8_t *bmp, size_t bytes,
                                               uint32_t *id)
{
    char prefix[SCREENSHOT_WRITER_PATH_CAP];
    size_t length = 0;
    unsigned saved;
    int error;
    ScreenshotJob *job;
    bool needs_start;
    if (id)
        *id = 0;
    if (!bmp || !bytes || bytes > SCREENSHOT_WRITER_MAX_BMP_BYTES || !id)
        return SCREENSHOT_WRITE_INVALID;
    if (!directory || !directory[0])
        directory = ".";
    while (directory[length] && length < sizeof(prefix) - 1U) {
        prefix[length] = directory[length];
        ++length;
    }
    if (directory[length])
        return SCREENSHOT_WRITE_INVALID;
    if (!length || prefix[length - 1U] != '/')
        prefix[length++] = '/';
    /* The suffix includes its terminating zero. */
    if (length + sizeof("ndvideo-shot-0000.bmp.tns") > sizeof(prefix))
        return SCREENSHOT_WRITE_INVALID;
    prefix[length] = '\0';
    saved = app_task_io_critical_enter();
    job = writer.stopping ? NULL : oldest_job(SHOT_FREE);
    app_task_io_critical_leave(saved);
    if (!job)
        return SCREENSHOT_WRITE_BUSY;
    if (!writer.native.supported || !writer.native.initialized) {
        error = app_task_io_init(&writer.native);
        if (error) {
            /* Retain any registration if destruction fails; shutdown retries. */
            app_task_io_destroy(&writer.native);
            return error == NATIVE_FILE_IO_UNSUPPORTED ? SCREENSHOT_WRITE_UNSUPPORTED
                                                       : SCREENSHOT_WRITE_FAILED;
        }
    }
    saved = app_task_io_critical_enter();
    needs_start = !writer.native.started;
    if (++writer.next_id == 0)
        ++writer.next_id;
    memset(&job->result, 0, sizeof(job->result));
    memcpy(job->result.path, prefix, length + 1U);
    job->result.id = writer.next_id;
    job->bmp = bmp;
    job->bytes = bytes;
    job->prefix_bytes = length;
    job->state = SHOT_QUEUED;
    app_task_io_critical_leave(saved);
    if (needs_start) {
        error = app_task_io_start(&writer.native, screenshot_writer_worker, NULL);
        if (error) {
            saved = app_task_io_critical_enter();
            job->state = SHOT_FREE;
            job->bmp = NULL;
            job->bytes = 0;
            app_task_io_critical_leave(saved);
            return SCREENSHOT_WRITE_FAILED;
        }
    }
    /* The copied job stays owned until the private writer closes the file. */
    app_task_io_signal(&writer.native, 1U);
    *id = job->result.id;
    return SCREENSHOT_WRITE_ACCEPTED;
}

bool screenshot_writer_poll(ScreenshotWriteResult *result)
{
    ScreenshotJob *job;
    uint8_t *bmp;
    unsigned saved;
    if (!result)
        return false;
    int startup_error = app_task_io_error(&writer.native);
    if (startup_error && writer.native.returned) {
        for (unsigned i = 0; i < SCREENSHOT_WRITER_QUEUE_SLOTS; ++i)
            if (writer.jobs[i].state == SHOT_QUEUED) {
                writer.jobs[i].result.native_error = startup_error;
                writer.jobs[i].state = SHOT_DONE;
            }
    }
    saved = app_task_io_critical_enter();
    job = oldest_job(SHOT_DONE);
    if (!job) {
        app_task_io_critical_leave(saved);
        return false;
    }
    *result = job->result;
    bmp = job->bmp;
    job->bmp = NULL;
    job->bytes = 0;
    job->state = SHOT_FREE;
    app_task_io_critical_leave(saved);
    free(bmp);
    return true;
}

size_t screenshot_writer_pending_bytes(void)
{
    size_t bytes = 0;
    unsigned i, saved = app_task_io_critical_enter();
    for (i = 0; i < SCREENSHOT_WRITER_QUEUE_SLOTS; ++i)
        bytes += writer.jobs[i].bytes;
    app_task_io_critical_leave(saved);
    return bytes;
}

void screenshot_writer_drain(void)
{
    unsigned saved = app_task_io_critical_enter();
    writer.stopping = true;
    app_task_io_critical_leave(saved);
    if (writer.native.started)
        app_task_io_signal(&writer.native, 1U);
    /* Foreground is the only submitter; the worker drains before observing stop. */
    while (writer.native.started) {
        if (app_task_io_join(&writer.native) == 0)
            break;
        app_task_io_yield(&writer.native);
    }
    while (app_task_io_destroy(&writer.native))
        app_task_io_yield(&writer.native);
}

void screenshot_writer_shutdown(void)
{
    screenshot_writer_drain();
    unsigned i;
    for (i = 0; i < SCREENSHOT_WRITER_QUEUE_SLOTS; ++i)
        free(writer.jobs[i].bmp);
    memset(&writer, 0, sizeof(writer));
}
