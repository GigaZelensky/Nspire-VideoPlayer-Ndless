#ifndef NDVIDEO_CRASH_RECORDER_H
#define NDVIDEO_CRASH_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CRASH_RECORDER_SNAPSHOT_WORDS 64U
#define CRASH_RECORDER_QUEUE_SLOTS 4U
#define CRASH_RECORDER_RECORD_BYTES 320U
#define CRASH_RECORDER_RECORDS_PER_FILE 256U
#define CRASH_RECORDER_FILE_A "ndvideo-crash-a.bin"
#define CRASH_RECORDER_FILE_B "ndvideo-crash-b.bin"

/* The foreground adapter owns the meaning of these 64 words. The recorder
 * copies all values before submit returns and never retains player pointers.
 * A one-second submission cadence is recommended; submit itself never waits. */
typedef struct {
    uint64_t monotonic_ticks;
    uint32_t words[CRASH_RECORDER_SNAPSHOT_WORDS];
} CrashRecorderSnapshot;

typedef struct {
    bool active;
    bool worker_ready;
    bool failed;
    uint32_t pending;
    uint32_t submitted;
    uint32_t written;
    uint32_t dropped;
    uint32_t failures;
    uint64_t last_sequence;
    int native_error;
} CrashRecorderStats;

/* Foreground only; initializes fixed storage and an app-owned context, but
 * performs no foreground filesystem operations. session_id is supplied by
 * the adapter (for example RTC seconds plus a monotonic boot-time value).
 * A new start uses the other rotation file, retaining the newest verified
 * complete prefix from the prior process. */
bool crash_recorder_start(const char *directory, uint64_t session_id);
/* Returns false when disabled/full/failed; never allocates or performs I/O. */
bool crash_recorder_submit(const CrashRecorderSnapshot *snapshot);
void crash_recorder_stats(CrashRecorderStats *stats);
/* Foreground log export; retained counters include startup failures even
 * after shutdown has destroyed the app-owned context. */
void crash_recorder_debug(FILE *file);
/* Stops accepting work, drains accepted copies, joins before freeing. Only
 * use at safe movie/OS-dialog/shutdown boundaries, never on D toggling. */
void crash_recorder_shutdown(void);

/* On-disk format v1: 80 little-endian uint32 words (320 bytes).
 * [0] magic 0x5243564E ("NVCR"), [1] version=1, [2] bytes=320,
 * [3..4] session_id, [5..6] sequence (continues across starts),
 * [7..8] monotonic_ticks, [9] payload_words=64, [10..14] reserved=0,
 * [15] FNV-1a32 of bytes [0..59] followed by bytes [64..319],
 * [16..79] copied snapshot words. 64-bit quantities are low word first.
 * The worker open/appends/writes/closes each record. Two files rotate at
 * 256 records each (163840 bytes total). Every new start begins in the other
 * file after a bounded prefix scan; no existing/torn tail is appended to.
 * Native close is verified; no unverified fflush/fsync entry is called. */

#endif
