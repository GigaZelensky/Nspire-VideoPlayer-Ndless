#ifndef NDVIDEO_NVP_FORMAT_H
#define NDVIDEO_NVP_FORMAT_H

#include <stdint.h>

#define MOVIE_VERSION_H264 9
#define MOVIE_VERSION_POSITIONED_SUBS 10
#define MOVIE_VERSION_CODEC_TAGGED 11

#define MOVIE_CODEC_FLAG_MASK 0x000FU
#define MOVIE_CODEC_FLAG_H264 0x0000U
#define MOVIE_CODEC_FLAG_MPEG4 0x0001U

#pragma pack(push, 1)
typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t flags;
    uint16_t canvas_width;
    uint16_t canvas_height;
    uint16_t video_x;
    uint16_t video_y;
    uint16_t video_width;
    uint16_t video_height;
    uint16_t fps_num;
    uint16_t fps_den;
    uint16_t block_size;
    uint16_t chunk_frames;
    uint32_t frame_count;
    uint32_t chunk_count;
    uint32_t subtitle_count;
    uint32_t index_offset;
    uint32_t subtitle_offset;
} MovieHeader;

typedef struct {
    uint32_t offset;
    uint32_t packed_size;
    uint32_t unpacked_size;
    uint32_t first_frame;
    uint32_t frame_count;
    uint32_t frame_table_offset;
} ChunkIndexEntry;
#pragma pack(pop)

typedef enum {
    MOVIE_CODEC_UNKNOWN = -1,
    MOVIE_CODEC_H264 = 0,
    MOVIE_CODEC_MPEG4 = 1,
} MovieCodec;

static inline MovieCodec movie_codec_from_header(const MovieHeader *header)
{
    if (!header) {
        return MOVIE_CODEC_UNKNOWN;
    }
    if (header->version == MOVIE_VERSION_H264 ||
        header->version == MOVIE_VERSION_POSITIONED_SUBS) {
        return MOVIE_CODEC_H264;
    }
    if (header->version == MOVIE_VERSION_CODEC_TAGGED) {
        switch (header->flags & MOVIE_CODEC_FLAG_MASK) {
        case MOVIE_CODEC_FLAG_H264:
            return MOVIE_CODEC_H264;
        case MOVIE_CODEC_FLAG_MPEG4:
            return MOVIE_CODEC_MPEG4;
        default:
            return MOVIE_CODEC_UNKNOWN;
        }
    }
    return MOVIE_CODEC_UNKNOWN;
}

static inline const char *movie_codec_name(MovieCodec codec)
{
    switch (codec) {
    case MOVIE_CODEC_H264:
        return "h264";
    case MOVIE_CODEC_MPEG4:
        return "mpeg4";
    default:
        return "unknown";
    }
}

#endif
