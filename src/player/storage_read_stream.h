#ifndef NDVIDEO_STORAGE_READ_STREAM_H
#define NDVIDEO_STORAGE_READ_STREAM_H
#include <stdint.h>
#include <stdio.h>
typedef struct StorageReadStream StorageReadStream;
/* Foreground-only read handles. Native open/close resolve names and maintain
 * filesystem identity; every content byte uses the independent reader.
 * The caller owns the logical position and clamps EOF before read_at. */
StorageReadStream *storage_read_stream_open(const char *path, int *error);
/* Snapshot failure details before cleanup/native close can replace them. */
const char *storage_read_stream_open_stage(void);
int storage_read_stream_open_status(void);
void storage_read_stream_debug(FILE *);
uint32_t storage_read_stream_size(const StorageReadStream *);
int storage_read_stream_read_at(StorageReadStream *, uint32_t offset, void *destination,
                                uint32_t bytes, int *error);
/* Once native close is attempted this consumes the stream, even on error.
 * Preflight EBUSY leaves it alive; the descriptor registry must retain it for
 * a later foreground retry/shutdown. No private-job recursive I/O is allowed. */
int storage_read_stream_close(StorageReadStream *, int *error);
/* Free the lazy shared workspace only when no handle/call/controller owns it. */
void storage_read_stream_shutdown(void);
#endif
