#include "screenshot_encode.h"

#include <SDL/SDL.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

bool screenshot_encode_bmp(SDL_Surface *screen, unsigned char **data, size_t *size)
{
    size_t width, height, row_bytes, header_bytes = 54U, capacity;
    unsigned char *buffer;
    SDL_RWops *stream;
    int encoded_size, result;
    unsigned bytes_per_pixel;

    if (data)
        *data = NULL;
    if (size)
        *size = 0;
    if (!data || !size || !screen || !screen->format || !screen->pixels || screen->w <= 0 ||
        screen->h <= 0 || screen->w > UINT16_MAX || screen->h > UINT16_MAX)
        return false;

    width = (size_t)screen->w;
    height = (size_t)screen->h;
    bytes_per_pixel = screen->format->BytesPerPixel;
    if (screen->format->palette) {
        const SDL_Palette *palette = screen->format->palette;
        if (screen->format->BitsPerPixel != 8 || bytes_per_pixel != 1 || !palette->colors ||
            palette->ncolors <= 0 || palette->ncolors > 256)
            return false;
        header_bytes += (size_t)palette->ncolors * 4U;
        row_bytes = width;
    } else {
        unsigned bits = screen->format->BitsPerPixel;
        if (!((bits == 15 || bits == 16) && bytes_per_pixel == 2) &&
            !(bits == 24 && bytes_per_pixel == 3) && !(bits == 32 && bytes_per_pixel == 4))
            return false;
        /* SDL 1.2 expands every non-indexed BMP to 24-bit BGR. */
        if (width > (SIZE_MAX - 3U) / 3U)
            return false;
        row_bytes = width * 3U;
    }
    if (width > SIZE_MAX / bytes_per_pixel || (size_t)screen->pitch < width * bytes_per_pixel ||
        row_bytes > SIZE_MAX - 3U)
        return false;
    row_bytes = (row_bytes + 3U) & ~(size_t)3U;
    /* SDL 1.2 stores surface pitch in Uint16, including its temporary BGR
     * conversion surface. Reject dimensions that would truncate that pitch. */
    if (row_bytes > UINT16_MAX || row_bytes > SCREENSHOT_BMP_MAX_BYTES - header_bytes ||
        height > (SCREENSHOT_BMP_MAX_BYTES - header_bytes) / row_bytes)
        return false;
    capacity = header_bytes + height * row_bytes;
    if (capacity > INT_MAX)
        return false;

    buffer = (unsigned char *)malloc(capacity);
    if (!buffer)
        return false;
    stream = SDL_RWFromMem(buffer, (int)capacity);
    if (!stream) {
        free(buffer);
        return false;
    }
    result = SDL_SaveBMP_RW(screen, stream, 0);
    encoded_size = SDL_RWtell(stream);
    SDL_RWclose(stream); /* Frees the RWops, not its caller-owned buffer. */
    /* SDL 1.2 does not check every header/padding write individually. The
     * exact final position additionally catches a short or clamped result. */
    if (result != 0 || encoded_size < 0 || (size_t)encoded_size != capacity) {
        free(buffer);
        return false;
    }
    *data = buffer;
    *size = capacity;
    return true;
}
