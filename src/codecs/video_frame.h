#ifndef NDVIDEO_VIDEO_FRAME_H
#define NDVIDEO_VIDEO_FRAME_H

#include <stdint.h>

/* Decoded 8-bit planar output. Plane pointers include the display crop and
 * remain valid until their decoder's release function is called. */
typedef struct VideoFrame {
    const uint8_t *plane[3];
    int stride[3];
    unsigned width, height;
    unsigned coded_width, coded_height;
    unsigned crop_left, crop_top;
    int64_t pts;
} VideoFrame;

#endif
