#ifndef NDVIDEO_NATIVE_FILE_IO_H
#define NDVIDEO_NATIVE_FILE_IO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Direct stdio for the fingerprinted CX II driver layout. Compatibility is
 * checked against code and task state, independently of the OS version label.
 * Handles are opaque; no native task/event is created. */
enum {
    NATIVE_FILE_IO_OK = 0,
    NATIVE_FILE_IO_UNSUPPORTED = -1000,
    NATIVE_FILE_IO_BAD_TASK = -1001,
    NATIVE_FILE_IO_BUSY = -1002,
    NATIVE_FILE_IO_BAD_STATE = -1003
};
typedef struct {
    bool supported;
    volatile unsigned io_phase;
} NativeFileIo;
int native_file_io_init(NativeFileIo *);
void *native_file_io_open(NativeFileIo *, const char *);
void *native_file_io_open_write(NativeFileIo *, const char *);
void *native_file_io_open_append(NativeFileIo *, const char *);
size_t native_file_io_write(NativeFileIo *, void *, const void *, size_t);
int native_file_io_close(NativeFileIo *, void *);
/* Exists: 1 present, 0 verified ENOENT, -1 other error. */
int native_file_io_exists(NativeFileIo *, const char *, int *);
int native_file_io_remove(NativeFileIo *, const char *);
#endif
