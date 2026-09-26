/* Newlib owns stdio buffering; these descriptors supply independent reads
 * underneath it, including fgets and buffered seeks. Native writable files
 * retain the SDK descriptor table and the player's existing write path. */
#include "storage_read_stream.h"
#include "storage_descriptors.h"
#include "raw_player_io.h"
#include "native_interrupts.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* Newlib stores FILE._file in a signed short. Stay clear of the SDK's small
 * native descriptor table without exceeding that representation. */
#define READ_DESCRIPTOR_BASE 0x4000
#define READ_DESCRIPTOR_COUNT 16
typedef struct {
    StorageReadStream *stream;
    uint32_t position;
} ReadDescriptor;
static ReadDescriptor descriptors[READ_DESCRIPTOR_COUNT];

extern int __real__open(const char *, int);
extern int __real__write(int, char *, int);
extern int __real__lseek(int, int, int);
extern int __real__close(int);

static ReadDescriptor *descriptor(int fd)
{
    unsigned index = (unsigned)fd - READ_DESCRIPTOR_BASE;
    return index < READ_DESCRIPTOR_COUNT ? &descriptors[index] : NULL;
}
static int error_result(int error)
{
    errno = error ? error : EIO;
    return -1;
}

int __wrap__open(const char *path, int flags, ...)
{
    if (!path)
        return error_result(EINVAL);
    int mode = flags & O_ACCMODE;
    if (mode == O_WRONLY) {
        unsigned mask = native_interrupt_mask();
        raw_player_before_native();
        int result = __real__open(path, flags);
        native_critical_leave(mask);
        return result;
    }
    /* An update stream needs a single cursor shared by native writes and raw
     * reads. The player uses separate rb/wb/ab streams, so reject that mode
     * explicitly instead of silently handing its reads back to the OS. */
    if (mode != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND)))
        return error_result(EINVAL);
    unsigned slot;
    for (slot = 0; slot < READ_DESCRIPTOR_COUNT; ++slot)
        if (!descriptors[slot].stream)
            break;
    if (slot == READ_DESCRIPTOR_COUNT)
        return error_result(EMFILE);
    int error = 0;
    StorageReadStream *stream = storage_read_stream_open(path, &error);
    if (!stream)
        return error_result(error);
    if (storage_read_stream_size(stream) > INT_MAX) {
        storage_read_stream_close(stream, &error);
        return error_result(EOVERFLOW);
    }
    descriptors[slot] = (ReadDescriptor){stream, 0};
    return READ_DESCRIPTOR_BASE + (int)slot;
}

int __wrap__read(int fd, char *destination, int bytes)
{
    ReadDescriptor *entry = descriptor(fd);
    if (!entry || !entry->stream)
        return error_result(EBADF);
    if (bytes < 0 || (!destination && bytes))
        return error_result(EINVAL);
    if (!bytes)
        return 0;
    uint32_t size = storage_read_stream_size(entry->stream);
    if (entry->position >= size)
        return 0;
    uint32_t count = (uint32_t)bytes;
    if (count > size - entry->position)
        count = size - entry->position;
    int error = 0;
    int result =
        storage_read_stream_read_at(entry->stream, entry->position, destination, count, &error);
    if (result < 0)
        return error_result(error);
    if ((uint32_t)result > count)
        return error_result(EIO);
    entry->position += (uint32_t)result;
    return result;
}

int __wrap__lseek(int fd, int offset, int whence)
{
    ReadDescriptor *entry = descriptor(fd);
    if (!entry) {
        unsigned mask = native_interrupt_mask();
        raw_player_before_native();
        int result = __real__lseek(fd, offset, whence);
        native_critical_leave(mask);
        return result;
    }
    if (!entry->stream)
        return error_result(EBADF);
    int64_t base;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = entry->position;
    else if (whence == SEEK_END)
        base = storage_read_stream_size(entry->stream);
    else
        return error_result(EINVAL);
    int64_t position = base + offset;
    if (position < 0)
        return error_result(EINVAL);
    if (position > INT_MAX)
        return error_result(EOVERFLOW);
    entry->position = (uint32_t)position;
    return (int)position;
}

int __wrap__close(int fd)
{
    ReadDescriptor *entry = descriptor(fd);
    if (!entry) {
        unsigned mask = native_interrupt_mask();
        raw_player_before_native();
        int result = __real__close(fd);
        native_critical_leave(mask);
        return result;
    }
    if (!entry->stream)
        return error_result(EBADF);
    int error = 0;
    int result = storage_read_stream_close(entry->stream, &error);
    /* An ownership conflict is detected before close touches the handle. */
    if (!result || error != EBUSY)
        memset(entry, 0, sizeof(*entry));
    return result ? error_result(error) : 0;
}

void storage_descriptors_shutdown(void)
{
    raw_player_before_native();
    for (unsigned i = 0; i < READ_DESCRIPTOR_COUNT; ++i)
        if (descriptors[i].stream)
            __wrap__close(READ_DESCRIPTOR_BASE + (int)i);
}

int __wrap__write(int fd, char *source, int bytes)
{
    if (descriptor(fd))
        return error_result(EBADF);
    if (bytes < 0 || (!source && bytes))
        return error_result(EINVAL);
    unsigned mask = native_interrupt_mask();
    raw_player_before_native();
    int result = __real__write(fd, source, bytes);
    native_critical_leave(mask);
    return result;
}
