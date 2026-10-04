#ifndef NDVIDEO_FRAME_VIEW_API_H
#define NDVIDEO_FRAME_VIEW_API_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "codecs/video_frame.h"
struct Movie;

/* Borrowed visible 8-bit 4:2:0 planes, valid only until decoder release/reset.
 * H.264 raw_picture is the coded Y-plane base returned by its decoder. */
bool video_decoder_get_frame_view(const struct Movie *movie, const uint8_t *raw_picture,
                                  VideoFrame *out);
/* Current RGB row conversion supports positive, even visible dimensions.
 * Zero indicates unsupported dimensions or size overflow. */
size_t video_frame_packed_size(unsigned width, unsigned height);
/* Destination must be a distinct, caller-owned allocation of packed_bytes.
 * Copy complete even luma bands and the corresponding U/V rows before release.
 * No decoder pointer is retained. Invalid arguments cause no writes. */
bool video_frame_pack_rows(const VideoFrame *src, uint8_t *packed, size_t packed_bytes,
                           unsigned first, unsigned rows);
bool video_frame_packed_view(const uint8_t *packed, size_t packed_bytes,
                             unsigned width, unsigned height, VideoFrame *out);
#endif
