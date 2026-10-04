#include "player_internal.h"
#include <limits.h>

bool movie_uses_planar_decoder(const Movie *movie)
{
    return movie && ((NDVIDEO_WITH_HEVC && movie->codec == MOVIE_CODEC_HEVC) ||
        (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1));
}

bool movie_uses_decode_ahead(const Movie *movie)
{
    return movie && ((NDVIDEO_WITH_H264 && movie->codec == MOVIE_CODEC_H264) ||
        movie_uses_planar_decoder(movie));
}

bool video_decoder_ready(const Movie *movie)
{
    if (!movie) return false;
    if (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1)
        return movie->av1.decoder && !movie->av1.decoder_failed;
    if (NDVIDEO_WITH_HEVC && movie->codec == MOVIE_CODEC_HEVC)
        return movie->hevc.decoder && !movie->hevc.decoder_failed;
    return NDVIDEO_WITH_H264 && movie->codec == MOVIE_CODEC_H264 && movie->h264.decoder &&
        movie->h264.decoder_initialized && !movie->h264.decoder_failed;
}

bool reset_hevc_decoder(Movie *movie)
{
    if (!NDVIDEO_WITH_HEVC) return false;
    if (!movie || !movie->hevc.decoder) return false;
    hevc_reset(movie->hevc.decoder);
    movie->hevc.picture = NULL;
    movie->hevc.access_unit = NULL;
    movie->hevc.access_unit_size = 0;
    movie->hevc.decoder_failed = false;
    return true;
}

bool reset_av1_decoder(Movie *movie)
{
    if (!NDVIDEO_WITH_AV1 || !movie || !movie->av1.decoder) return false;
    av1_reset(movie->av1.decoder);
    movie->av1.picture = NULL;
    movie->av1.access_unit = NULL;
    movie->av1.access_unit_size = 0;
    movie->av1.decoder_failed = false;
    return true;
}

bool video_decoder_reset(Movie *movie)
{
    if (NDVIDEO_WITH_AV1 && movie && movie->codec == MOVIE_CODEC_AV1) return reset_av1_decoder(movie);
    return NDVIDEO_WITH_HEVC && movie && movie->codec == MOVIE_CODEC_HEVC
        ? reset_hevc_decoder(movie) : NDVIDEO_WITH_H264 && reset_h264_decoder(movie);
}

void video_decoder_mark_failed(Movie *movie)
{
    if (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1) movie->av1.decoder_failed = true;
    else if (NDVIDEO_WITH_HEVC && movie->codec == MOVIE_CODEC_HEVC) movie->hevc.decoder_failed = true;
    else movie->h264.decoder_failed = true;
}

static unsigned hevc_units_per_ctu(const Movie *movie)
{
    if (!NDVIDEO_WITH_HEVC) return 0;
    unsigned size = hevc_ctu_size(movie->hevc.decoder);
    if (!size) size = 32;
    return (size / 16U) * (size / 16U);
}

unsigned video_decoder_min_units(const Movie *movie)
{
    if (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1) {
        unsigned size = av1_block_size(movie->av1.decoder);
        return (size / 16U) * (size / 16U);
    }
    if (NDVIDEO_WITH_HEVC && movie->codec == MOVIE_CODEC_HEVC) return hevc_units_per_ctu(movie);
    return 1;
}

uint32_t video_decoder_total_units(const Movie *movie)
{
    if (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1) {
        unsigned size = av1_block_size(movie->av1.decoder);
        unsigned total = av1_picture_blocks_total(movie->av1.decoder);
        if (!total) total = ((movie->header.video_width + size - 1U) / size) *
            ((movie->header.video_height + size - 1U) / size);
        return total * video_decoder_min_units(movie);
    }
    if (movie->codec != MOVIE_CODEC_HEVC)
        return NDVIDEO_WITH_H264 ? h264_incremental_total_mbs(movie, movie->h264.decoder) : 0;
    if (!NDVIDEO_WITH_HEVC) return 0;
    unsigned total = hevc_picture_ctus_total(movie->hevc.decoder);
    if (!total) {
        unsigned size = hevc_ctu_size(movie->hevc.decoder);
        if (!size) size = 32;
        total = ((movie->header.video_width + size - 1U) / size) *
                ((movie->header.video_height + size - 1U) / size);
    }
    return total * hevc_units_per_ctu(movie);
}

uint32_t video_decoder_done_units(const Movie *movie)
{
    if (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1)
        return av1_picture_blocks_done(movie->av1.decoder) * video_decoder_min_units(movie);
    if (NDVIDEO_WITH_HEVC && movie->codec == MOVIE_CODEC_HEVC)
        return hevc_picture_ctus_done(movie->hevc.decoder) * hevc_units_per_ctu(movie);
    return NDVIDEO_WITH_H264 ? movie->h264.decoder->slice->numDecodedMbs : 0;
}

static bool pump_av1(Movie *movie, uint8_t *data, size_t size, size_t *consumed,
    unsigned units, uint64_t deadline_ticks, bool *ready, bool *pending, uint8_t **picture)
{
    *ready = *pending = false; *picture = NULL;
    if (!NDVIDEO_WITH_AV1 || !movie->av1.decoder || movie->av1.decoder_failed) return false;
    if (!*consumed) {
        if (movie->av1.access_unit) {
            if (movie->av1.access_unit != data || movie->av1.access_unit_size != size) {
                debug_failf("av1 pending input changed without a decoder reset");
                movie->av1.decoder_failed = true;
                return false;
            }
        } else {
            if (av1_submit_obus(movie->av1.decoder, data, size, 0) == AV1_ERROR) goto fail;
            movie->av1.access_unit = data;
            movie->av1.access_unit_size = size;
            ++movie->av1.submitted_frames;
        }
        *consumed = size;
    }
    unsigned blocks = units / video_decoder_min_units(movie);
    av1_status_t status = av1_step_until(movie->av1.decoder, blocks ? blocks : 1U, deadline_ticks,
        deadline_ticks ? monotonic_clock_now_ticks : NULL);
    movie->av1.decoded_blocks += av1_last_step_blocks(movie->av1.decoder);
    if (status == AV1_PROGRESS) { *pending = true; return true; }
    if (status != AV1_FRAME_READY) goto fail;
    movie->av1.picture = av1_get_frame(movie->av1.decoder);
    if (!movie->av1.picture || movie->av1.picture->width != movie->header.video_width ||
        movie->av1.picture->height != movie->header.video_height) {
        debug_failf("av1 frame size does not match movie header");
        movie->av1.decoder_failed = true;
        return false;
    }
    *picture = (uint8_t *)movie->av1.picture->plane[0];
    *ready = true;
    return true;
fail:
    debug_failf("av1 decode: %s", av1_error_string(movie->av1.decoder));
    movie->av1.decoder_failed = true;
    return false;
}

bool video_decoder_pump(Movie *movie, uint8_t *data, size_t size, size_t *consumed,
    unsigned *retries, unsigned units, uint64_t deadline_ticks,
    bool *ready, bool *pending, uint8_t **picture)
{
    if (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1)
        return pump_av1(movie, data, size, consumed, units, deadline_ticks, ready, pending, picture);
    if (movie->codec != MOVIE_CODEC_HEVC) {
        movie->h264.chunk_dirty = true;
        return NDVIDEO_WITH_H264 && pump_h264_access_unit(movie, movie->h264.decoder, data, size, consumed,
            retries, units, true, "lookahead", ready, pending, picture);
    }
    *ready = *pending = false; *picture = NULL;
    if (!NDVIDEO_WITH_HEVC) return false;
    if (!movie->hevc.decoder || movie->hevc.decoder_failed) return false;
    if (!*consumed) {
        if (movie->hevc.access_unit) {
            /* A seek can yield between CTUs, including with a held picture.
             * Resume that same input without submitting the frame twice. */
            if (movie->hevc.access_unit != data || movie->hevc.access_unit_size != size) {
                debug_failf("hevc pending input changed without a decoder reset");
                movie->hevc.decoder_failed = true;
                return false;
            }
        } else {
            hevc_status_t status = hevc_submit_annexb(movie->hevc.decoder, data, size, 0);
            if (status == HEVC_ERROR) goto fail;
            movie->hevc.access_unit = data;
            movie->hevc.access_unit_size = size;
            ++movie->hevc.submitted_frames;
        }
        *consumed = size;
    }
    unsigned ctus = units / hevc_units_per_ctu(movie);
    if (!ctus || !hevc_ctu_size(movie->hevc.decoder)) ctus = 1;
    hevc_status_t status = deadline_ticks
        ? hevc_step_until(movie->hevc.decoder, ctus, deadline_ticks, monotonic_clock_now_ticks)
        : hevc_step(movie->hevc.decoder, ctus);
    movie->hevc.decoded_ctus += hevc_last_step_ctus(movie->hevc.decoder);
    if (status == HEVC_PROGRESS) { *pending = true; return true; }
    if (status != HEVC_FRAME_READY) goto fail;
    movie->hevc.picture = hevc_get_frame(movie->hevc.decoder);
    if (!movie->hevc.picture || movie->hevc.picture->width != movie->header.video_width ||
        movie->hevc.picture->height != movie->header.video_height) {
        debug_failf("hevc frame size does not match movie header");
        video_decoder_mark_failed(movie);
        return false;
    }
    *picture = (uint8_t *)movie->hevc.picture->plane[0];
    *ready = true;
    return *picture != NULL;
fail:
    debug_failf("hevc decode: %s", hevc_error_string(movie->hevc.decoder));
    movie->hevc.decoder_failed = true;
    return false;
}

void video_decoder_release_picture(Movie *movie)
{
    if (NDVIDEO_WITH_AV1 && movie && movie->codec == MOVIE_CODEC_AV1 && movie->av1.decoder) {
        if (av1_get_frame(movie->av1.decoder)) {
            av1_release_frame(movie->av1.decoder);
            movie->av1.access_unit = NULL;
            movie->av1.access_unit_size = 0;
        }
        movie->av1.picture = NULL;
        return;
    }
    if (!NDVIDEO_WITH_HEVC) return;
    if (movie && movie->codec == MOVIE_CODEC_HEVC && movie->hevc.decoder) {
        if (hevc_get_frame(movie->hevc.decoder)) {
            hevc_release_frame(movie->hevc.decoder);
            movie->hevc.access_unit = NULL;
            movie->hevc.access_unit_size = 0;
        }
        movie->hevc.picture = NULL;
    }
}

bool video_blit_picture_rows(Movie *movie, const uint8_t *picture, uint16_t *pixels,
    size_t pitch, size_t first, size_t rows, bool *flat)
{
    if (movie->codec == MOVIE_CODEC_AV1)
        return blit_planar_picture_rows(movie, movie->av1.picture, pixels, pitch, first, rows, flat);
    return movie->codec == MOVIE_CODEC_HEVC
        ? blit_planar_picture_rows(movie, movie->hevc.picture, pixels, pitch, first, rows, flat)
        : blit_h264_picture_rows_to_target(movie, picture, pixels, pitch, first, rows, flat);
}

static bool planar_frame_can_restart(const Movie *movie, const ChunkIndexEntry *entry, uint32_t local)
{
    size_t start = movie->frame_offsets[local];
    size_t end = local + 1U < entry->frame_count ? movie->frame_offsets[local + 1U] : movie->chunk_size;
    if (start >= end || end > movie->chunk_size) return false;
    const uint8_t *data = movie->chunk_bytes + start;
    size_t bytes = end - start;
    if (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1)
        return av1_packet_is_independent(data, bytes);
    unsigned parameters = 0;
    for (size_t i = 0; i + 5U < bytes; ++i) {
        if (data[i] || data[i + 1U] || data[i + 2U] != 1U) continue;
        unsigned type = (data[i + 3U] >> 1U) & 63U;
        /* A fresh decoder needs all three parameter sets before the IDR.
         * Keep replaying from the chunk start for streams without them. */
        if (type >= 32U && type <= 34U) parameters |= 1U << (type - 32U);
        else if (type < 32U)
            return parameters == 7U && (type == 19U || type == 20U) && (data[i + 5U] & 0x80U);
    }
    return false;
}

static bool decode_planar_frame_progress(Movie *movie, uint32_t frame_index, bool blit_output,
    H264FramePublishPredicate predicate, H264DecodedFrameHook hook, VideoDecodePoll poll, void *userdata,
    bool resumable)
{
    if (!movie_uses_planar_decoder(movie)) return false;
    int chunk = movie_chunk_for_frame(movie, frame_index);
    if (chunk < 0 || !load_chunk(movie, chunk) || !video_decoder_ready(movie)) return false;
    const ChunkIndexEntry *entry = movie->chunk_index + chunk;
    uint32_t local = frame_index - entry->first_frame;
    if (movie->decoded_local_frame >= (int)local) {
        if (!video_decoder_reset(movie)) return false;
        movie->decoded_local_frame = -1;
    }
    if (!hook || (resumable && movie->decoded_local_frame < 0)) {
        /* Resume previews need only their final image. Storage chunks can
         * contain many GOPs; skip earlier ones when a self-contained IDR is
         * closer than the decoder's current position. A new interactive seek
         * can start there too; an in-progress seek keeps its decoded frames
         * when its destination is extended. */
        uint32_t next = (uint32_t)(movie->decoded_local_frame + 1);
        for (uint32_t i = local; i > next; --i) {
            if (!planar_frame_can_restart(movie, entry, i)) continue;
            if (!video_decoder_reset(movie)) return false;
            movie->decoded_local_frame = (int)i - 1;
            break;
        }
    }
    for (uint32_t i = (uint32_t)(movie->decoded_local_frame + 1); i <= local; ++i) {
        size_t start = movie->frame_offsets[i];
        size_t end = i + 1U < entry->frame_count ? movie->frame_offsets[i+1] : movie->chunk_size;
        if (start >= end || end > movie->chunk_size) return false;
        size_t consumed = 0; unsigned retries = 0;
        bool ready = false, pending = false, flat = false;
        uint8_t *picture = NULL;
        do {
            if (!video_decoder_pump(movie, movie->chunk_bytes + start, end-start,
                    &consumed, &retries, 4U, 0U, &ready, &pending, &picture)) return false;
            if (poll && !poll(userdata)) {
                if (!resumable) {
                    /* Cancellation of a loading operation discards its work.
                     * Interactive seeks keep it for the next input turn. */
                    video_decoder_reset(movie);
                    invalidate_loaded_chunk_state(movie);
                }
                return false;
            }
        } while (pending);
        if (!ready) return false;
        uint32_t frame = entry->first_frame + i;
        bool publish = hook && (frame == frame_index || !predicate || predicate(movie, frame, userdata));
        if (blit_output && (frame == frame_index || publish) &&
            !video_blit_picture_rows(movie, picture, movie->framebuffer,
                movie->header.video_width, 0, movie->header.video_height, &flat)) return false;
        video_decoder_release_picture(movie);
        movie->decoded_local_frame = (int)i;
        if (publish) {
            movie->current_frame = frame;
            if (!hook(movie, frame, userdata)) return false;
        }
    }
    return true;
}

bool decode_hevc_frame_with_progress(Movie *movie, uint32_t frame_index, bool blit_output,
    H264FramePublishPredicate predicate, H264DecodedFrameHook hook, VideoDecodePoll poll, void *userdata)
{
    return decode_planar_frame_progress(movie, frame_index, blit_output, predicate, hook, poll, userdata, false);
}

bool decode_hevc_seek_step(Movie *movie, uint32_t frame_index, H264FramePublishPredicate predicate,
    H264DecodedFrameHook hook, VideoDecodePoll poll, void *userdata)
{
    return decode_planar_frame_progress(movie, frame_index, true, predicate, hook, poll, userdata, true);
}

bool decode_hevc_frame(Movie *movie, uint32_t frame_index, bool blit_output)
{
    return decode_hevc_frame_with_progress(movie, frame_index, blit_output, NULL, NULL, NULL, NULL);
}

bool decode_av1_frame(Movie *movie, uint32_t frame_index, bool blit_output)
{
    return NDVIDEO_WITH_AV1 && decode_planar_frame_progress(movie, frame_index, blit_output, NULL, NULL, NULL, NULL, false);
}

bool decode_av1_seek_step(Movie *movie, uint32_t frame_index, H264FramePublishPredicate predicate,
    H264DecodedFrameHook hook, VideoDecodePoll poll, void *userdata)
{
    return NDVIDEO_WITH_AV1 && decode_planar_frame_progress(movie, frame_index, true, predicate, hook, poll, userdata, true);
}

static bool planar_loading_tick(void *userdata)
{
    loading_progress_tick(userdata, false);
    return true;
}

bool decode_to_frame_loading(Movie *movie, uint32_t frame_index, LoadingProgress *progress)
{
    if (!movie_uses_planar_decoder(movie)) return decode_to_frame(movie, frame_index);
    video_lookahead_cancel(movie);
    if (!decode_planar_frame_progress(movie, frame_index, true, NULL, NULL,
            planar_loading_tick, progress, false)) return false;
    movie->current_frame = frame_index;
    if (movie->lookahead_enabled && !video_lookahead_begin(movie)) movie->lookahead_enabled = false;
    return true;
}

size_t video_frame_packed_size(unsigned width, unsigned height)
{
    if (!width || !height || ((width | height) & 1U) || width > INT_MAX ||
        height > SIZE_MAX / width) return 0;
    size_t luma = (size_t)width * height;
    return luma > SIZE_MAX - luma / 2U ? 0 : luma + luma / 2U;
}

static bool video_frame_view_valid(const VideoFrame *frame)
{
    if (!frame || !video_frame_packed_size(frame->width, frame->height)) return false;
    for (unsigned p = 0; p < 3U; ++p) {
        unsigned width = frame->width >> (p != 0);
        unsigned height = frame->height >> (p != 0);
        if (!frame->plane[p] || frame->stride[p] <= 0 ||
            (unsigned)frame->stride[p] < width ||
            height - 1U > (SIZE_MAX - width) / (unsigned)frame->stride[p]) return false;
    }
    return true;
}

bool video_decoder_get_frame_view(const Movie *movie, const uint8_t *raw_picture,
                                  VideoFrame *out)
{
    VideoFrame frame = {0};
    if (!movie || !raw_picture || !out) return false;
    if (NDVIDEO_WITH_HEVC && movie->codec == MOVIE_CODEC_HEVC) {
        if (movie->hevc.decoder_failed || !movie->hevc.picture) return false;
        frame = *movie->hevc.picture;
    } else if (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1) {
        if (movie->av1.decoder_failed || !movie->av1.picture) return false;
        frame = *movie->av1.picture;
    } else if (NDVIDEO_WITH_H264 && movie->codec == MOVIE_CODEC_H264) {
        unsigned full_width = movie->h264.full_width, full_height = movie->h264.full_height;
        unsigned left = movie->h264.crop_left, top = movie->h264.crop_top;
        unsigned width = movie->h264.crop_width, height = movie->h264.crop_height;
        if (!movie->h264.headers_ready || movie->h264.decoder_failed ||
            !video_frame_packed_size(full_width, full_height) ||
            !video_frame_packed_size(width, height) ||
            ((left | top) & 1U) || left > full_width || top > full_height ||
            width > full_width - left || height > full_height - top) return false;
        size_t luma = (size_t)full_width * full_height;
        size_t chroma = luma / 4U;
        size_t uv_offset = (size_t)(top / 2U) * (full_width / 2U) + left / 2U;
        frame.plane[0] = raw_picture + (size_t)top * full_width + left;
        frame.plane[1] = raw_picture + luma + uv_offset;
        frame.plane[2] = raw_picture + luma + chroma + uv_offset;
        frame.stride[0] = (int)full_width;
        frame.stride[1] = frame.stride[2] = (int)(full_width / 2U);
        frame.width = width; frame.height = height;
        frame.coded_width = full_width; frame.coded_height = full_height;
        frame.crop_left = left; frame.crop_top = top;
    } else return false;
    if ((movie->codec != MOVIE_CODEC_H264 && frame.plane[0] != raw_picture) ||
        frame.width != movie->header.video_width || frame.height != movie->header.video_height ||
        !video_frame_view_valid(&frame)) return false;
    *out = frame;
    return true;
}

bool video_frame_pack_rows(const VideoFrame *src, uint8_t *packed, size_t packed_bytes,
                           unsigned first, unsigned rows)
{
    if (!packed || !video_frame_view_valid(src) || !rows || ((first | rows) & 1U) ||
        first > src->height || rows > src->height - first ||
        packed_bytes < video_frame_packed_size(src->width, src->height)) return false;
    size_t luma = (size_t)src->width * src->height;
    for (unsigned p = 0; p < 3U; ++p) {
        unsigned width = src->width >> (p != 0), row = first >> (p != 0);
        unsigned count = rows >> (p != 0);
        size_t plane_offset = p == 0 ? 0 : luma + (p == 2 ? luma / 4U : 0);
        uint8_t *dst = packed + plane_offset + (size_t)row * width;
        const uint8_t *input = src->plane[p] + (size_t)row * (unsigned)src->stride[p];
        if ((unsigned)src->stride[p] == width) {
            player_copy_maybe_fast(dst, input, (size_t)count * width);
        } else {
            for (unsigned y = 0; y < count; ++y) {
                player_copy_maybe_fast(dst, input, width);
                dst += width;
                if (y + 1U < count) input += src->stride[p];
            }
        }
    }
    return true;
}

bool video_frame_packed_view(const uint8_t *packed, size_t packed_bytes,
                             unsigned width, unsigned height, VideoFrame *out)
{
    size_t bytes = video_frame_packed_size(width, height);
    if (!packed || !out || !bytes || packed_bytes < bytes) return false;
    size_t luma = (size_t)width * height;
    VideoFrame frame = {0};
    frame.plane[0] = packed; frame.plane[1] = packed + luma;
    frame.plane[2] = packed + luma + luma / 4U;
    frame.stride[0] = (int)width; frame.stride[1] = frame.stride[2] = (int)(width / 2U);
    frame.width = frame.coded_width = width; frame.height = frame.coded_height = height;
    *out = frame;
    return true;
}
