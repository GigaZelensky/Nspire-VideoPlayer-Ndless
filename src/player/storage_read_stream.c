#include "storage_read_stream.h"
#include "raw_player_io.h"
#include "app_task_io.h"
#include "native_interrupts.h"
#include "storage_activity.h"
#include "storage_mutation.h"
#include "movie_crypto_session.h"
#include "../platform/portable_reader_platform.h"
#include "../storage/raw_file_reader.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#define STREAM_CHUNK_BYTES 32768U
#define STREAM_CALL_SECONDS 120U
#define STREAM_STEP_LIMIT 16000000U
#define STREAM_PAGE_SECONDS 3U
struct StorageReadStream {
    void *native;
    uint32_t bytes, inode;
    NveReader *crypt;
};
typedef struct {
    PortableStorageSnapshot snapshot;
    RawFileReader reader;
    RawFileOverlay dirty[PORTABLE_STORAGE_DIRTY_MAX], clean[PORTABLE_STORAGE_CLEAN_MAX];
    StorageReadStream *owner;
    uint32_t epoch, native_revision;
} ReadScratch;
static PortableReaderPlatform platform;
static ReadScratch *scratch;
static unsigned handles;
static bool active;
static void set_error(int *out, int value)
{
    if (out)
        *out = value;
}
static uint32_t seconds(void) { return *(const volatile uint32_t *)0x90090000U; }
static void *allocate(size_t bytes)
{
    unsigned saved = native_interrupt_mask();
    void *p = calloc(1, bytes);
    native_critical_leave(saved);
    return p;
}
static void dispose(void *p)
{
    unsigned saved = native_interrupt_mask();
    free(p);
    native_critical_leave(saved);
}
static int native_error(void)
{
    /* Native errno values for these common filesystem failures match the SDK
     * libc contract. Do not leak private platform/stage numbers as errno. */
    switch (portable_reader_platform_errno(&platform)) {
    case 2:
        return ENOENT;
    case 5:
        return EIO;
    case 9:
        return EBADF;
    case 12:
        return ENOMEM;
    case 13:
        return EACCES;
    case 17:
        return EEXIST;
    case 20:
        return ENOTDIR;
    case 21:
        return EISDIR;
    case 22:
        return EINVAL;
    case 24:
        return EMFILE;
    case 28:
        return ENOSPC;
    default:
        return EIO;
    }
}
static bool claim(bool native, int *error)
{
    if (active || app_task_io_in_job()) {
        set_error(error, EBUSY);
        return false;
    }
    active = true;
    /* This parks/drains the optimized primary reader and app-owned writes.
     * No callback below may resume those jobs while our scratch view is live. */
    unsigned saved = native_interrupt_mask();
    if (native)
        raw_player_before_native();
    else
        raw_player_before_read();
    native_critical_leave(saved);
    return true;
}
static bool workspace(int *error)
{
    if (!scratch)
        scratch = allocate(sizeof(*scratch));
    if (!scratch) {
        set_error(error, ENOMEM);
        return false;
    }
    return true;
}
static void release(unsigned mask)
{
    platform.table_valid = false;
    native_critical_leave(mask);
    active = false;
}
static void drain_reader(void)
{
    RawFileReader *r = &scratch->reader;
    if (r->status == RAW_FILE_PENDING)
        raw_file_cancel(r);
    /* A timed-out chip retains ownership until ready. Returning early and
     * allowing native filesystem I/O would corrupt controller ownership. */
    while (r->status == RAW_FILE_PENDING || (r->nand.initialized && !r->nand.quiescent))
        raw_file_step(r, seconds(), STREAM_PAGE_SECONDS);
}
StorageReadStream *storage_read_stream_open(const char *path, int *error)
{
    unsigned entry_mask = native_interrupt_mask();
    set_error(error, 0);
    if (!path || !*path) {
        set_error(error, EINVAL);
        return NULL;
    }
    if (!claim(true, error))
        return NULL;
    if (!workspace(error)) {
        active = false;
        return NULL;
    }
    StorageReadStream *stream = allocate(sizeof(*stream));
    if (!stream) {
        active = false;
        set_error(error, ENOMEM);
        return NULL;
    }
    unsigned mask = native_critical_enter();
    int status = portable_reader_platform_init(&platform);
    native_critical_leave(mask);
    if (status) {
        dispose(stream);
        active = false;
        set_error(error, ENOSYS);
        return NULL;
    }
    stream->native = portable_reader_platform_open(&platform, path);
    native_critical_leave(entry_mask);
    if (!stream->native) {
        int failure = native_error();
        dispose(stream);
        active = false;
        set_error(error, failure);
        return NULL;
    }
    mask = native_critical_enter();
    scratch->owner = NULL;
    status = portable_reader_platform_capture(&platform, stream->native, 0, &scratch->snapshot);
    if (!status) {
        stream->bytes = scratch->snapshot.file_bytes;
        stream->inode = scratch->snapshot.inode;
    }
    platform.table_valid = false;
    native_critical_leave(mask);
    if (status) {
        /* No raw command was issued. Native close is safe after releasing the
         * read-only RAM observation, and consumes its native stream. */
        portable_reader_platform_close(&platform, stream->native);
        native_critical_leave(entry_mask);
        dispose(stream);
        active = false;
        set_error(error, EIO);
        return NULL;
    }
    const NveKeys *keys = movie_crypto_keys(path);
    if (keys) {
        if (keys->header.physical_bytes != stream->bytes ||
            !(stream->crypt = allocate(sizeof(*stream->crypt)))) {
            portable_reader_platform_close(&platform, stream->native);
            dispose(stream);
            active = false;
            native_critical_leave(entry_mask);
            set_error(error, EIO);
            return NULL;
        }
        nve_reader_init(stream->crypt, keys);
    }
    ++handles;
    active = false;
    native_critical_leave(entry_mask);
    return stream;
}
uint32_t storage_read_stream_size(const StorageReadStream *stream)
{
    return stream ? (stream->crypt ? stream->crypt->keys->header.plain_bytes : stream->bytes) : 0;
}
static int read_raw_at(StorageReadStream *stream, uint32_t offset, void *destination,
                                uint32_t bytes, int *error)
{
    unsigned entry_mask = native_interrupt_mask();
    set_error(error, 0);
    if (!stream || !stream->native) {
        set_error(error, EBADF);
        return -1;
    }
    if (bytes > (uint32_t)INT_MAX) {
        set_error(error, EOVERFLOW);
        return -1;
    }
    if (offset > stream->bytes || bytes > stream->bytes - offset ||
        (bytes && (!destination || (uintptr_t)destination > UINTPTR_MAX - bytes))) {
        set_error(error, EINVAL);
        return -1;
    }
    if (!bytes)
        return 0;
    if (!claim(false, error))
        return -1;
    if (!workspace(error)) {
        active = false;
        return -1;
    }
    native_critical_enter();
    int failure = EIO;
    uint32_t copied = 0, steps = 0, started = seconds();
    bool timed_out = false;
    PortableStorageSnapshot *s = &scratch->snapshot;
    RawFileReader *r = &scratch->reader;
    uint32_t epoch = storage_mutation_epoch(), revision = storage_native_revision();
    /* Stdio can refill the same file hundreds of times during one load.
     * Retain its metadata and reconstructed maps until native filesystem work
     * actually changes the view. All preceding requests finished quiescent. */
    if (scratch->owner == stream && epoch && revision && scratch->epoch == epoch &&
        scratch->native_revision == revision)
        goto read;
    scratch->owner = NULL;
    if (portable_reader_platform_capture(&platform, stream->native, 0, s) ||
        s->inode != stream->inode || s->file_bytes != stream->bytes)
        goto finished;
    for (unsigned i = 0; i < s->dirty_count; ++i)
        scratch->dirty[i] =
            (RawFileOverlay){s->dirty[i].number, (const uint8_t *)(uintptr_t)s->dirty[i].address};
    for (unsigned i = 0; i < s->clean_count; ++i)
        scratch->clean[i] =
            (RawFileOverlay){s->clean[i].number, (const uint8_t *)(uintptr_t)s->clean[i].address};
    NandPageConfig config = {platform.kind, portable_reader_platform_bus(), s->spare};
    if (!raw_file_init(r, s->state_data, sizeof(s->state_data), s->state, s->block_count,
                       s->index_block, s->inode, config, s->layout, scratch->dirty,
                       s->dirty_count) ||
        !raw_file_metadata_cache(r, scratch->clean, s->clean_count))
        goto finished;
    for (unsigned i = 0; i < 3; ++i)
        if (s->region[i].active) {
            r->map.region[i] = (FfxMapRegion){(const uint8_t *)(uintptr_t)s->region[i].address,
                                              FFX_MAP_REGION_BYTES};
            r->ages[i] = i + 1U;
        }
    r->age = 3;
    scratch->owner = stream;
    scratch->epoch = epoch;
    scratch->native_revision = revision;
read:
    while (copied < bytes) {
        uint32_t count = bytes - copied;
        if (count > STREAM_CHUNK_BYTES)
            count = STREAM_CHUNK_BYTES;
        if (!raw_file_begin(r, (uint64_t)offset + copied, (uint8_t *)destination + copied, count))
            goto finished;
        while (r->status == RAW_FILE_PENDING || (r->nand.initialized && !r->nand.quiescent)) {
            uint32_t now = seconds();
            if (++steps >= STREAM_STEP_LIMIT || (uint32_t)(now - started) >= STREAM_CALL_SECONDS) {
                timed_out = true;
                raw_file_cancel(r);
            }
            raw_file_step(r, now, STREAM_PAGE_SECONDS);
        }
        if (r->status != RAW_FILE_DONE)
            goto finished;
        copied += count;
    }
    failure = 0;
finished:
    drain_reader();
    release(entry_mask);
    if (failure) {
        scratch->owner = NULL;
        set_error(error, timed_out ? ETIMEDOUT : failure);
        return -1;
    }
    return (int)copied;
}
int storage_read_stream_read_at(StorageReadStream *stream, uint32_t offset, void *destination,
                                uint32_t bytes, int *error)
{
    if (!stream || !stream->crypt) return read_raw_at(stream, offset, destination, bytes, error);
    set_error(error, 0);
    if (!stream->native) { set_error(error, EBADF); return -1; }
    if (active || app_task_io_in_job()) { set_error(error, EBUSY); return -1; }
    if (offset > storage_read_stream_size(stream)) { set_error(error, EINVAL); return -1; }
    if (!bytes) return 0;
    NveReader *r = stream->crypt;
    if (!nve_reader_begin(r, offset, destination, bytes)) {
        set_error(error, EINVAL);
        return -1;
    }
    while (r->phase != NVE_READ_DONE && r->phase != NVE_READ_ERROR) {
        if (r->phase == NVE_READ_FETCH) {
            uint32_t count = r->unit_bytes + NVE_TAG_BYTES;
            int result = read_raw_at(stream, nve_reader_physical_offset(r), r->block, count, error);
            nve_reader_supplied(r, result == (int)count);
        } else nve_reader_step(r);
    }
    if (r->phase == NVE_READ_ERROR) { set_error(error, EIO); return -1; }
    return (int)bytes;
}
int storage_read_stream_close(StorageReadStream *stream, int *error)
{
    unsigned entry_mask = native_interrupt_mask();
    set_error(error, 0);
    if (!stream || !stream->native) {
        set_error(error, EBADF);
        return -1;
    }
    if (!claim(true, error))
        return -1;
    if (scratch && scratch->owner == stream)
        scratch->owner = NULL;
    int status = portable_reader_platform_close(&platform, stream->native);
    native_critical_leave(entry_mask);
    int failure = status ? native_error() : 0;
    stream->native = NULL;
    if (stream->crypt) {
        nve_wipe(stream->crypt, sizeof(*stream->crypt));
        dispose(stream->crypt);
    }
    if (handles)
        --handles;
    dispose(stream);
    active = false;
    if (status) {
        set_error(error, failure);
        return -1;
    }
    return 0;
}
void storage_read_stream_shutdown(void)
{
    if (active || handles || app_task_io_in_job() ||
        (scratch && scratch->reader.nand.initialized && !scratch->reader.nand.quiescent))
        return;
    dispose(scratch);
    scratch = NULL;
    platform.table_valid = false;
}
