#ifndef NDVIDEO_MOVIE_ASYNC_IO_H
#define NDVIDEO_MOVIE_ASYNC_IO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct Movie;
enum { MOVIE_ASYNC_DISABLED = -1, MOVIE_ASYNC_PENDING = 0, MOVIE_ASYNC_READY = 1 };
#define MOVIE_ASYNC_BLOCK_BYTES 32768U

bool movie_async_start(struct Movie *movie, const char *path);
void movie_async_stop(struct Movie *movie);
void movie_async_cancel(struct Movie *movie);
/* Raw readers retain their open stream through standby after quiescing NAND. */
bool movie_async_suspend(struct Movie *);
void movie_async_resume(struct Movie *);
bool movie_async_enabled(const struct Movie *movie);
size_t movie_async_buffer_bytes(const struct Movie *movie);
/* Destination is touched only when READY. Pending reads use private RAM.
 * DISABLED uses foreground independent reads after controller ownership settles. */
int movie_async_read(struct Movie *movie, uint64_t offset, void *destination, size_t bytes,
                     int chunk_index, bool wait);
void player_delay_ms(unsigned milliseconds);
void movie_async_debug(FILE *, const struct Movie *);
void movie_async_service(struct Movie *, unsigned budget_ticks);
bool movie_async_crypto_step(struct Movie *, uint32_t spare_ticks);
#endif
