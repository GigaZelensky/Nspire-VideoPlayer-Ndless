#ifndef NDVIDEO_SCREENSHOT_ENCODE_H
#define NDVIDEO_SCREENSHOT_ENCODE_H

#include <stdbool.h>
#include <stddef.h>

struct SDL_Surface;
#define SCREENSHOT_BMP_MAX_BYTES (256U * 1024U)

/* Foreground-only SDL work, using memory RWops: no filesystem operations.
 * Success returns malloc-owned bytes; transfer ownership only when a writer
 * accepts the job, otherwise free them. Failure leaves *data=NULL, *size=0.
 * Output is identical to SDL_SaveBMP, including 8-bit indexed palettes.
 * RGB565 320x240 needs 230454 bytes; indexed 320x240 needs at most 77878. */
bool screenshot_encode_bmp(struct SDL_Surface *screen, unsigned char **data, size_t *size);

#endif
