#include "player_internal.h"

typedef struct {
    uint32_t deadline_ms;
    uint32_t target_frame;
    bool yielded;
} SeekSlice;

static bool seek_poll(void *userdata)
{
    SeekSlice *slice = userdata;
    if ((int32_t)(monotonic_clock_now_ms() - slice->deadline_ms) >= 0)
        slice->yielded = true;
    return !slice->yielded;
}

static bool seek_frame_ready(Movie *movie, uint32_t frame, void *userdata)
{
    SeekSlice *slice = userdata;
    (void)movie;
    /* Present each complete image through the ordinary UI loop. That loop
     * also owns pause, key repeats and pointer input during catch-up. */
    slice->yielded = frame != slice->target_frame;
    return !slice->yielded;
}

static bool seek_at_target(const Movie *movie, const PlaybackSeek *seek)
{
    if (movie->current_frame != seek->target_frame) return false;
    if (video_lookahead_active(movie)) return true;
    /* The visible image can outlive a decoder reset. Retargeting to that
     * image must rebuild its references cooperatively before normal playback. */
    return movie->loaded_chunk >= 0 &&
        (uint32_t)movie->loaded_chunk < movie->header.chunk_count &&
        movie->decoded_local_frame >= 0 &&
        movie->chunk_index[movie->loaded_chunk].first_frame +
            (uint32_t)movie->decoded_local_frame == seek->target_frame;
}

bool playback_seek_relative_target(const Movie *movie, const PlaybackSeek *seek,
    int32_t delta_ms, uint32_t *target)
{
    if (!movie || !target || !movie->header.frame_count) return false;
    uint32_t base = seek->active ? seek->target_frame : movie->current_frame;
    int64_t target_ms = (int64_t)movie_frame_time_ms(movie, base) + delta_ms;
    uint32_t duration = movie_duration_ms(movie);
    if (target_ms < 0) target_ms = 0;
    if ((uint64_t)target_ms >= duration) target_ms = duration ? duration - 1U : 0;
    *target = movie_frames_from_ms(movie, (uint32_t)target_ms);
    if (*target >= movie->header.frame_count) *target = movie->header.frame_count - 1U;
    return true;
}

void playback_seek_request(PlaybackSeek *seek, uint32_t target, int marker_x, bool *paused)
{
    bool keep_preview = seek->active && seek->preview_pending && marker_x < 0;
    if (!seek->active) seek->pause_after = *paused;
    seek->active = true;
    seek->target_frame = target;
    seek->marker_x = marker_x;
    seek->preview_pending = marker_x >= 0 || keep_preview;
    *paused = false;
}

bool playback_seek_step(Movie *movie, PlaybackSeek *seek, SeekBarPreviewState *preview, bool *paused)
{
    /* HEVC yields between CTUs; each finished picture returns sooner. */
    SeekSlice slice = {monotonic_clock_now_ms() + 8U, seek->target_frame, false};
    if (!seek->active) return true;
    if (*paused && !seek_at_target(movie, seek)) return true;
    if (!seek_at_target(movie, seek) && seek->preview_pending) {
        bool adopted = commit_seek_bar_preview_to_movie(movie, preview, seek->target_frame,
            seek_poll, &slice);
        if (!adopted && slice.yielded) return true;
        seek->preview_pending = false;
        clear_seek_bar_preview(preview);
        if (seek->marker_x >= 0)
            suppress_seek_bar_preview_rebuild(preview, seek->marker_x, seek->target_frame);
        if (adopted && movie->current_frame != seek->target_frame) return true;
    }
    if (!seek_at_target(movie, seek) &&
        !decode_to_frame_with_progress(movie, seek->target_frame, NULL, seek_frame_ready,
            seek_poll, &slice, &slice.yielded))
        return slice.yielded;

    seek->active = false;
    seek->preview_pending = false;
    *paused = *paused || seek->pause_after;
    if (movie->lookahead_enabled && !video_lookahead_active(movie))
        video_lookahead_begin(movie);
    clear_seek_bar_preview(preview);
    if (seek->marker_x >= 0)
        suppress_seek_bar_preview_rebuild(preview, seek->marker_x, seek->target_frame);
    return true;
}
