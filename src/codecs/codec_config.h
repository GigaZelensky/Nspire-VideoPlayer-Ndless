#ifndef NDVIDEO_CODEC_CONFIG_H
#define NDVIDEO_CODEC_CONFIG_H

/* Standalone builds keep every codec unless the build selects a subset.
 * These constants remove disabled adapter paths at compile time; the matching
 * decoder sources are also excluded from the link by the Makefile. */
#ifndef NDVIDEO_WITH_H264
#define NDVIDEO_WITH_H264 1
#endif
#ifndef NDVIDEO_WITH_MPEG4
#define NDVIDEO_WITH_MPEG4 1
#endif
#ifndef NDVIDEO_WITH_HEVC
#define NDVIDEO_WITH_HEVC 1
#endif
#ifndef NDVIDEO_WITH_AV1
#define NDVIDEO_WITH_AV1 1
#endif
#ifndef NDVIDEO_CODEC_MODULES
#define NDVIDEO_CODEC_MODULES 0
#endif

#if (NDVIDEO_WITH_H264 != 0 && NDVIDEO_WITH_H264 != 1) || \
    (NDVIDEO_WITH_MPEG4 != 0 && NDVIDEO_WITH_MPEG4 != 1) || \
    (NDVIDEO_WITH_HEVC != 0 && NDVIDEO_WITH_HEVC != 1) || \
    (NDVIDEO_WITH_AV1 != 0 && NDVIDEO_WITH_AV1 != 1)
#error Codec selections must be 0 or 1
#endif
#if NDVIDEO_CODEC_MODULES != 0 && NDVIDEO_CODEC_MODULES != 1
#error NDVIDEO_CODEC_MODULES must be 0 or 1
#endif
#if !NDVIDEO_WITH_H264 && !NDVIDEO_WITH_MPEG4 && !NDVIDEO_WITH_HEVC && !NDVIDEO_WITH_AV1
#error At least one video codec must be enabled
#endif

#endif
