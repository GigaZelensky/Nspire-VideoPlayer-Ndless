#ifndef NDVIDEO_CODEC_H
#define NDVIDEO_CODEC_H

#include <stdbool.h>
#include <stdint.h>

#include "movie/nvp_format.h"

struct Movie;

typedef struct {
    MovieCodec codec;
    const char *name;
    bool (*global_init)(void);
    bool (*open)(struct Movie *movie);
    void (*destroy)(struct Movie *movie);
    bool (*reset)(struct Movie *movie);
    bool (*decode_frame)(struct Movie *movie, uint32_t frame_index, bool blit_output);
    bool (*supports_incremental_seek_preview)(const struct Movie *movie);
} MovieCodecOps;

const MovieCodecOps *movie_codec_ops(MovieCodec codec);

#endif
