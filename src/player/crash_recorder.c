#include "crash_recorder.h"
#include "app_task_io.h"
#include "storage_activity.h"
#include "journal_reader.h"

#include <limits.h>
#include <string.h>

#define CRASH_PATH_CAP 512U
#define CRASH_RECORD_MAGIC 0x5243564EU
#define CRASH_RECORD_VERSION 1U
enum { CRASH_FREE, CRASH_QUEUED, CRASH_WRITING };
enum { CRASH_IO_ERROR = -2200, CRASH_SEQUENCE_EXHAUSTED = -2201 };

typedef struct {
    unsigned state;
    uint64_t ticket;
    CrashRecorderSnapshot snapshot;
} CrashJob;

typedef struct {
    AppTaskIo native;
    JournalReader *reader;
    CrashJob jobs[CRASH_RECORDER_QUEUE_SLOTS];
    char paths[2][CRASH_PATH_CAP];
    uint64_t session_id, next_ticket, next_sequence;
    unsigned target_file, records_in_file;
    bool accepting, stopping;
    CrashRecorderStats stats;
} CrashRecorder;

static CrashRecorder recorder;

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static uint32_t record_checksum(const uint8_t *record)
{
    uint32_t hash = 2166136261U;
    for (unsigned i = 0; i < CRASH_RECORDER_RECORD_BYTES; ++i)
        if (i < 60U || i >= 64U)
            hash = (hash ^ record[i]) * 16777619U;
    return hash;
}

static bool valid_record(const uint8_t *record)
{
    if (get32(record) != CRASH_RECORD_MAGIC || get32(record + 4) != CRASH_RECORD_VERSION ||
        get32(record + 8) != CRASH_RECORDER_RECORD_BYTES ||
        get32(record + 36) != CRASH_RECORDER_SNAPSHOT_WORDS || !get64(record + 20) ||
        get32(record + 60) != record_checksum(record))
        return false;
    for (unsigned i = 10; i < 15; ++i)
        if (get32(record + 4U * i))
            return false;
    return true;
}

static void encode_record(uint8_t *record, const CrashRecorderSnapshot *snapshot, uint64_t sequence)
{
    memset(record, 0, CRASH_RECORDER_RECORD_BYTES);
    put32(record, CRASH_RECORD_MAGIC);
    put32(record + 4, CRASH_RECORD_VERSION);
    put32(record + 8, CRASH_RECORDER_RECORD_BYTES);
    put64(record + 12, recorder.session_id);
    put64(record + 20, sequence);
    put64(record + 28, snapshot->monotonic_ticks);
    put32(record + 36, CRASH_RECORDER_SNAPSHOT_WORDS);
    for (unsigned i = 0; i < CRASH_RECORDER_SNAPSHOT_WORDS; ++i)
        put32(record + 64U + i * 4U, snapshot->words[i]);
    put32(record + 60, record_checksum(record));
}

/* App-job startup inspection through the independent reader. Stop at the first incomplete/corrupt record
 * and never append to a pre-existing file; the newest valid prefix survives
 * in the other rotation slot. No guessed native EOF/flush/fsync API needed. */
static int scan_file(unsigned index, uint64_t *last_sequence)
{
    uint8_t record[CRASH_RECORDER_RECORD_BYTES];
    int error = 0;
    int exists = app_task_io_file_exists(&recorder.native, recorder.paths[index], &error);
    *last_sequence = 0;
    if (exists < 0)
        return error ? error : CRASH_IO_ERROR;
    if (!exists)
        return 0;
    void *file = app_task_io_file_open(&recorder.native, recorder.paths[index]);
    if (!file)
        return CRASH_IO_ERROR;
    if (!journal_reader_open(recorder.reader, file)) {
        app_task_io_file_close(&recorder.native, file);
        return CRASH_IO_ERROR;
    }
    for (unsigned i = 0; i < CRASH_RECORDER_RECORDS_PER_FILE; ++i) {
        int bytes = journal_reader_read(recorder.reader, &recorder.native,
                                        i * CRASH_RECORDER_RECORD_BYTES, record, sizeof(record));
        if (bytes < 0) {
            error = CRASH_IO_ERROR;
            break;
        }
        if ((unsigned)bytes != sizeof(record) || !valid_record(record))
            break;
        uint64_t sequence = get64(record + 20);
        if (sequence <= *last_sequence)
            break;
        *last_sequence = sequence;
        /* Yield between records as well as inside longer independent reads. */
        if ((i & 15U) == 15U)
            app_task_io_yield(&recorder.native);
    }
    journal_reader_end(recorder.reader);
    int closed = app_task_io_file_close(&recorder.native, file);
    return error ? error : closed;
}

static int recover_journal(void)
{
    uint64_t sequences[2];
    int error = scan_file(0, &sequences[0]);
    if (error)
        return error;
    error = scan_file(1, &sequences[1]);
    if (error)
        return error;
    unsigned newest = sequences[1] > sequences[0] ? 1U : 0U;
    uint64_t previous = sequences[newest];
    if (previous == UINT64_MAX)
        return CRASH_SEQUENCE_EXHAUSTED;
    recorder.target_file = previous ? (newest ^ 1U) : 0U;
    recorder.records_in_file = 0;
    recorder.next_sequence = previous + 1U;
    unsigned saved = app_task_io_critical_enter();
    recorder.stats.last_sequence = previous;
    recorder.stats.worker_ready = true;
    app_task_io_critical_leave(saved);
    return 0;
}

static CrashJob *oldest_queued(void)
{
    CrashJob *oldest = NULL;
    for (unsigned i = 0; i < CRASH_RECORDER_QUEUE_SLOTS; ++i) {
        CrashJob *job = &recorder.jobs[i];
        if (job->state == CRASH_QUEUED && (!oldest || job->ticket < oldest->ticket))
            oldest = job;
    }
    return oldest;
}

static void fail_worker(int error)
{
    unsigned saved = app_task_io_critical_enter();
    recorder.accepting = false;
    recorder.stats.failed = true;
    recorder.stats.native_error = error;
    ++recorder.stats.failures;
    for (unsigned i = 0; i < CRASH_RECORDER_QUEUE_SLOTS; ++i) {
        if (recorder.jobs[i].state != CRASH_FREE) {
            recorder.jobs[i].state = CRASH_FREE;
            ++recorder.stats.dropped;
        }
    }
    recorder.stats.pending = 0;
    app_task_io_critical_leave(saved);
}

static void observe_startup_failure(void)
{
    int error = app_task_io_error(&recorder.native);
    if (error && recorder.native.returned && !recorder.stats.failed)
        fail_worker(error);
}

static int write_snapshot(const CrashRecorderSnapshot *snapshot)
{
    uint8_t record[CRASH_RECORDER_RECORD_BYTES];
    uint64_t sequence = recorder.next_sequence;
    if (!sequence)
        return CRASH_SEQUENCE_EXHAUSTED;
    if (recorder.records_in_file == CRASH_RECORDER_RECORDS_PER_FILE) {
        recorder.target_file ^= 1U;
        recorder.records_in_file = 0;
    }
    encode_record(record, snapshot, sequence);
    void *file =
        recorder.records_in_file
            ? app_task_io_file_open_append(&recorder.native, recorder.paths[recorder.target_file])
            : app_task_io_file_open_write(&recorder.native, recorder.paths[recorder.target_file]);
    if (!file)
        return CRASH_IO_ERROR;
    size_t written = app_task_io_file_write(&recorder.native, file, record, sizeof(record));
    int closed = app_task_io_file_close(&recorder.native, file);
    if (written != sizeof(record))
        return CRASH_IO_ERROR;
    if (closed)
        return closed;
    ++recorder.records_in_file;
    recorder.next_sequence = sequence + 1U;
    unsigned saved = app_task_io_critical_enter();
    ++recorder.stats.written;
    recorder.stats.last_sequence = sequence;
    app_task_io_critical_leave(saved);
    return 0;
}

static void crash_worker(void *argument)
{
    (void)argument;
    storage_native_begin();
    int error = recover_journal();
    storage_native_end();
    if (error) {
        fail_worker(error);
        return;
    }
    for (;;) {
        unsigned saved = app_task_io_critical_enter();
        CrashJob *job = oldest_queued();
        if (!job) {
            bool stop = recorder.stopping;
            app_task_io_critical_leave(saved);
            if (stop)
                return;
            unsigned received = 0;
            /* No OS event or fallible signal delivery: wait for real work. */
            error = app_task_io_wait(&recorder.native, 1U, true, UINT_MAX, &received);
            if (error && error != -36 && error != -50)
                app_task_io_sleep(&recorder.native, 1U);
            continue;
        }
        job->state = CRASH_WRITING;
        app_task_io_critical_leave(saved);
        storage_native_begin();
        error = write_snapshot(&job->snapshot);
        storage_native_end();
        if (error) {
            fail_worker(error);
            return;
        }
        saved = app_task_io_critical_enter();
        job->state = CRASH_FREE;
        --recorder.stats.pending;
        app_task_io_critical_leave(saved);
    }
}

static bool make_paths(const char *directory)
{
    static const char *names[2] = {CRASH_RECORDER_FILE_A, CRASH_RECORDER_FILE_B};
    size_t length = 0;
    if (!directory || !directory[0])
        directory = ".";
    while (length < CRASH_PATH_CAP && directory[length])
        ++length;
    if (length == CRASH_PATH_CAP)
        return false;
    bool separator = length && directory[length - 1U] != '/' && directory[length - 1U] != '\\';
    for (unsigned i = 0; i < 2; ++i) {
        size_t name_length = strlen(names[i]);
        if (length + (separator ? 1U : 0U) + name_length + 1U > CRASH_PATH_CAP)
            return false;
        memcpy(recorder.paths[i], directory, length);
        size_t position = length;
        if (separator)
            recorder.paths[i][position++] = '/';
        memcpy(recorder.paths[i] + position, names[i], name_length + 1U);
    }
    return true;
}

bool crash_recorder_start(const char *directory, uint64_t session_id)
{
    if (recorder.native.supported || recorder.native.initialized || recorder.native.started)
        return false;
    memset(&recorder, 0, sizeof(recorder));
    if (!make_paths(directory))
        return false;
    recorder.session_id = session_id;
    int error = app_task_io_init(&recorder.native);
    if (!error) {
        recorder.reader = journal_reader_create();
        if (!recorder.reader)
            error = CRASH_IO_ERROR;
    }
    if (error) {
        recorder.stats.failed = true;
        recorder.stats.failures = 1;
        recorder.stats.native_error = error;
        /* A failed ownership cleanup is retained for explicit shutdown. */
        app_task_io_destroy(&recorder.native);
        journal_reader_destroy(recorder.reader);
        recorder.reader = NULL;
        return false;
    }
    recorder.accepting = true;
    recorder.stats.active = true;
    error = app_task_io_start(&recorder.native, crash_worker, NULL);
    if (error) {
        recorder.accepting = false;
        recorder.stats.active = false;
        recorder.stats.failed = true;
        recorder.stats.failures = 1;
        recorder.stats.native_error = error;
        app_task_io_destroy(&recorder.native);
        journal_reader_destroy(recorder.reader);
        recorder.reader = NULL;
        return false;
    }
    return true;
}

bool crash_recorder_submit(const CrashRecorderSnapshot *snapshot)
{
    observe_startup_failure();
    unsigned saved = app_task_io_critical_enter();
    if (!snapshot || !recorder.accepting || recorder.stopping) {
        app_task_io_critical_leave(saved);
        return false;
    }
    CrashJob *job = NULL;
    for (unsigned i = 0; i < CRASH_RECORDER_QUEUE_SLOTS; ++i)
        if (recorder.jobs[i].state == CRASH_FREE) {
            job = &recorder.jobs[i];
            break;
        }
    if (!job || recorder.next_ticket == UINT64_MAX) {
        ++recorder.stats.dropped;
        app_task_io_critical_leave(saved);
        return false;
    }
    job->snapshot = *snapshot;
    job->ticket = ++recorder.next_ticket;
    job->state = CRASH_QUEUED;
    ++recorder.stats.pending;
    ++recorder.stats.submitted;
    app_task_io_critical_leave(saved);
    /* Direct in-process signal; no periodic idle wakeups. */
    app_task_io_signal(&recorder.native, 1U);
    return true;
}

void crash_recorder_stats(CrashRecorderStats *stats)
{
    if (!stats)
        return;
    observe_startup_failure();
    unsigned saved = app_task_io_critical_enter();
    *stats = recorder.stats;
    app_task_io_critical_leave(saved);
}

void crash_recorder_debug(FILE *file)
{
    if (!file)
        return;
    CrashRecorderStats stats;
    crash_recorder_stats(&stats);
    fprintf(
        file,
        "crash_recorder active=%u ready=%u failed=%u submitted=%lu written=%lu pending=%lu dropped=%lu failures=%lu native_error=%d\n",
        stats.active ? 1U : 0U, stats.worker_ready ? 1U : 0U, stats.failed ? 1U : 0U,
        (unsigned long)stats.submitted, (unsigned long)stats.written, (unsigned long)stats.pending,
        (unsigned long)stats.dropped, (unsigned long)stats.failures, stats.native_error);
}

void crash_recorder_shutdown(void)
{
    unsigned saved = app_task_io_critical_enter();
    recorder.accepting = false;
    recorder.stopping = true;
    app_task_io_critical_leave(saved);
    if (recorder.native.started)
        app_task_io_signal(&recorder.native, 1U);
    while (recorder.native.started) {
        if (app_task_io_join(&recorder.native) == 0)
            break;
        app_task_io_yield(&recorder.native);
    }
    /* A first service inside join can reject deferred initialization. Capture
     * that error before destroy clears the backend, even without a stats poll. */
    observe_startup_failure();
    while (app_task_io_destroy(&recorder.native))
        app_task_io_yield(&recorder.native);
    journal_reader_destroy(recorder.reader);
    recorder.reader = NULL;
    recorder.stats.active = false;
    recorder.stopping = false;
}
