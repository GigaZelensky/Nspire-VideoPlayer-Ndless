#include "journal_reader.h"
#include "native_interrupts.h"
#include "storage_activity.h"
#include "../platform/portable_reader_platform.h"
#include "../storage/raw_file_reader.h"
#include <limits.h>
#include <stdlib.h>

struct JournalReader {
    PortableReaderPlatform platform;
    PortableStorageSnapshot snapshot;
    RawFileReader reader;
    RawFileOverlay dirty[PORTABLE_STORAGE_DIRTY_MAX];
    RawFileOverlay clean[PORTABLE_STORAGE_CLEAN_MAX];
    bool open;
};

static uint32_t ticks(void)
{
    return 0U - *(const volatile uint32_t *)0x900c0004U;
}
static bool quiescent(const JournalReader *reader)
{
    return !reader->reader.nand.initialized || reader->reader.nand.quiescent;
}
JournalReader *journal_reader_create(void)
{
    if (app_task_io_in_job())
        return NULL;
    unsigned mask = native_interrupt_mask();
    JournalReader *reader = calloc(1, sizeof(*reader));
    native_critical_leave(mask);
    if (reader && portable_reader_platform_init(&reader->platform)) {
        free(reader);
        native_critical_leave(mask);
        return NULL;
    }
    return reader;
}
void journal_reader_destroy(JournalReader *reader)
{
    if (!reader || app_task_io_in_job() || !quiescent(reader))
        return;
    unsigned mask = native_interrupt_mask();
    free(reader);
    native_critical_leave(mask);
}
bool journal_reader_open(JournalReader *reader, void *stream)
{
    if (!reader || !stream || !app_task_io_in_job() || !storage_native_active() ||
        native_interrupt_mask() != 0xc0U || !quiescent(reader))
        return false;
    reader->open = false;
    PortableStorageSnapshot *s = &reader->snapshot;
    if (portable_reader_platform_capture(&reader->platform, stream, 0, s))
        return false;
    for (unsigned i = 0; i < s->dirty_count; ++i)
        reader->dirty[i] =
            (RawFileOverlay){s->dirty[i].number, (const uint8_t *)(uintptr_t)s->dirty[i].address};
    for (unsigned i = 0; i < s->clean_count; ++i)
        reader->clean[i] =
            (RawFileOverlay){s->clean[i].number, (const uint8_t *)(uintptr_t)s->clean[i].address};
    NandPageConfig config = {s->kind, portable_reader_platform_bus(), s->spare};
    RawFileReader *raw = &reader->reader;
    if (!raw_file_init(raw, s->state_data, sizeof(s->state_data), s->state, s->block_count,
                       s->index_block, s->inode, config, s->layout, reader->dirty,
                       s->dirty_count) ||
        !raw_file_metadata_cache(raw, reader->clean, s->clean_count))
        return false;
    for (unsigned i = 0; i < 3; ++i) {
        if (s->region[i].active) {
            raw->map.region[i] = (FfxMapRegion){(const uint8_t *)(uintptr_t)s->region[i].address,
                                                FFX_MAP_REGION_BYTES};
            raw->ages[i] = i + 1;
        }
    }
    raw->age = 3;
    reader->open = true;
    return true;
}
int journal_reader_read(JournalReader *reader, AppTaskIo *job, uint32_t offset, void *destination,
                        uint32_t bytes)
{
    if (!reader || !reader->open || !job || !app_task_io_in_job() || !storage_native_active() ||
        native_interrupt_mask() != 0xc0U || bytes > INT_MAX || (bytes && !destination))
        return -1;
    if (!bytes || offset >= reader->snapshot.file_bytes)
        return 0;
    if (bytes > reader->snapshot.file_bytes - offset)
        bytes = reader->snapshot.file_bytes - offset;
    RawFileReader *raw = &reader->reader;
    if (!raw_file_begin(raw, offset, destination, bytes))
        return -1;
    uint32_t started = ticks(), slice = started;
    do {
        uint32_t now = ticks();
        if ((uint32_t)(now - started) >= 10U * 32768U)
            raw_file_cancel(raw);
        raw_file_step(raw, now, 32768U);
        /* The app job retains its exclusive storage lease across a yield.
         * The private context can switch safely at completed NAND commands;
         * the foreground may render/decode while this view remains borrowed. */
        if (quiescent(reader) && (uint32_t)(ticks() - slice) >= 8U) {
            app_task_io_yield(job);
            slice = ticks();
        }
    } while (raw->status == RAW_FILE_PENDING || !quiescent(reader));
    return raw->status == RAW_FILE_DONE ? (int)bytes : -1;
}
void journal_reader_end(JournalReader *reader)
{
    if (reader && quiescent(reader)) {
        reader->open = false;
        reader->platform.table_valid = false;
    }
}
