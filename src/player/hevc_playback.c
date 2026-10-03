#include "player_internal.h"

bool movie_uses_decode_ahead(const Movie *movie)
{
    return movie && ((NDVIDEO_WITH_H264 && movie->codec == MOVIE_CODEC_H264) ||
        (NDVIDEO_WITH_HEVC && movie->codec == MOVIE_CODEC_HEVC));
}

bool video_decoder_ready(const Movie *movie)
{
    if (!movie) return false;
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

bool video_decoder_reset(Movie *movie)
{
    return NDVIDEO_WITH_HEVC && movie && movie->codec == MOVIE_CODEC_HEVC
        ? reset_hevc_decoder(movie) : NDVIDEO_WITH_H264 && reset_h264_decoder(movie);
}

void video_decoder_mark_failed(Movie *movie)
{
    if (NDVIDEO_WITH_HEVC && movie->codec == MOVIE_CODEC_HEVC) movie->hevc.decoder_failed = true;
    else movie->h264.decoder_failed = true;
}

static unsigned hevc_units_per_ctu(const Movie *movie)
{
    if (!NDVIDEO_WITH_HEVC) return 0;
    unsigned size = hevc_ctu_size(movie->hevc.decoder);
    if (!size) size = 32;
    return (size / 16U) * (size / 16U);
}

uint32_t video_decoder_total_units(const Movie *movie)
{
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
    if (NDVIDEO_WITH_HEVC && movie->codec == MOVIE_CODEC_HEVC)
        return hevc_picture_ctus_done(movie->hevc.decoder) * hevc_units_per_ctu(movie);
    return NDVIDEO_WITH_H264 ? movie->h264.decoder->slice->numDecodedMbs : 0;
}

bool video_decoder_pump(Movie *movie, uint8_t *data, size_t size, size_t *consumed,
    unsigned *retries, unsigned units, bool *ready, bool *pending, uint8_t **picture)
{
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
    hevc_status_t status = hevc_step(movie->hevc.decoder, ctus);
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
    return movie->codec == MOVIE_CODEC_HEVC
        ? blit_hevc_picture_rows(movie, movie->hevc.picture, pixels, pitch, first, rows, flat)
        : blit_h264_picture_rows_to_target(movie, picture, pixels, pitch, first, rows, flat);
}

static bool hevc_frame_can_restart(const Movie *movie, const ChunkIndexEntry *entry, uint32_t local)
{
    size_t start = movie->frame_offsets[local];
    size_t end = local + 1U < entry->frame_count ? movie->frame_offsets[local + 1U] : movie->chunk_size;
    if (start >= end || end > movie->chunk_size) return false;
    const uint8_t *data = movie->chunk_bytes + start;
    size_t bytes = end - start;
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

static bool decode_hevc_frame_progress(Movie *movie, uint32_t frame_index, bool blit_output,
    H264FramePublishPredicate predicate, H264DecodedFrameHook hook, VideoDecodePoll poll, void *userdata,
    bool resumable)
{
    if (!NDVIDEO_WITH_HEVC) return false;
    int chunk = movie_chunk_for_frame(movie, frame_index);
    if (chunk < 0 || !load_chunk(movie, chunk) || !video_decoder_ready(movie)) return false;
    const ChunkIndexEntry *entry = movie->chunk_index + chunk;
    uint32_t local = frame_index - entry->first_frame;
    if (movie->decoded_local_frame >= (int)local) {
        if (!reset_hevc_decoder(movie)) return false;
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
            if (!hevc_frame_can_restart(movie, entry, i)) continue;
            if (!reset_hevc_decoder(movie)) return false;
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
                    &consumed, &retries, 4U, &ready, &pending, &picture)) return false;
            if (poll && !poll(userdata)) {
                if (!resumable) {
                    /* Cancellation of a loading operation discards its work.
                     * Interactive seeks keep it for the next input turn. */
                    reset_hevc_decoder(movie);
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
    return decode_hevc_frame_progress(movie, frame_index, blit_output, predicate, hook, poll, userdata, false);
}

bool decode_hevc_seek_step(Movie *movie, uint32_t frame_index, H264FramePublishPredicate predicate,
    H264DecodedFrameHook hook, VideoDecodePoll poll, void *userdata)
{
    return decode_hevc_frame_progress(movie, frame_index, true, predicate, hook, poll, userdata, true);
}

bool decode_hevc_frame(Movie *movie, uint32_t frame_index, bool blit_output)
{
    return decode_hevc_frame_with_progress(movie, frame_index, blit_output, NULL, NULL, NULL, NULL);
}

static bool hevc_loading_tick(void *userdata)
{
    loading_progress_tick(userdata, false);
    return true;
}

bool decode_to_frame_loading(Movie *movie, uint32_t frame_index, LoadingProgress *progress)
{
    if (!NDVIDEO_WITH_HEVC || movie->codec != MOVIE_CODEC_HEVC) return decode_to_frame(movie, frame_index);
    video_lookahead_cancel(movie);
    if (!decode_hevc_frame_with_progress(movie, frame_index, true, NULL, NULL,
            hevc_loading_tick, progress)) return false;
    movie->current_frame = frame_index;
    if (movie->lookahead_enabled && !video_lookahead_begin(movie)) movie->lookahead_enabled = false;
    return true;
}
