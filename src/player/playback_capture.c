#include "player_internal.h"
#include "playback_capture_core.h"
#include "playback_cadence.h"
#include "native_screen_power.h"
#include "native_standby.h"
#include "private_writer.h"
#include "performance_clock.h"

#define CAPTURE_RENDER_REASON_COUNT 10U

typedef struct {
    const Movie *movie;
    bool active, have_settings, pending, transition, account_paused;
    uint64_t started, stopped, accounted, active_ticks, paused_ticks;
    uint64_t period[CAPTURE_STAGE_COUNT];
    CaptureTiming timing[CAPTURE_STAGE_COUNT];
    CaptureTiming io_timing[CAPTURE_IO_KIND_COUNT];
    uint64_t io_bytes[CAPTURE_IO_KIND_COUNT];
    CaptureFrame frames[CAPTURE_FRAME_CAPACITY], pending_frame;
    CaptureIo io[CAPTURE_IO_CAPACITY];
    uint32_t frame_next, frame_count, io_next, io_count;
    uint64_t frames_overwritten, io_overwritten;
    uint64_t presented, skipped, presentation_late, over_budget, lateness_ticks, max_lateness;
    uint64_t first_present, last_present;
    PlaybackCadence cadence;
    bool ahead_scope;
    uint32_t settings_changes, loops, format_events, interval_overflows;
    CaptureSettings first_settings, settings;
    uint32_t tick_hz, start_frame, end_frame;
    uint64_t render_reason_presented, render_reasons[CAPTURE_RENDER_REASON_COUNT];
    char media_name[160];
} PlaybackCapture;

static PlaybackCapture *g_capture;

bool playback_capture_active(const Movie *movie)
{
    return g_capture && g_capture->active && (!movie || movie == g_capture->movie);
}

bool playback_capture_available(const Movie *movie)
{
    return g_capture && (!movie || movie == g_capture->movie);
}

void playback_capture_render_reason(const Movie *movie, uint32_t reasons)
{
    unsigned i;
    if (!playback_capture_active(movie))
        return;
    ++g_capture->render_reason_presented;
    for (i = 0; i < CAPTURE_RENDER_REASON_COUNT; ++i)
        if (reasons & (1U << i))
            ++g_capture->render_reasons[i];
}

static void capture_account(uint64_t now)
{
    uint64_t elapsed = now - g_capture->accounted;
    if (g_capture->have_settings && !g_capture->account_paused)
        g_capture->active_ticks += elapsed;
    else
        g_capture->paused_ticks += elapsed;
    g_capture->accounted = now;
}

bool playback_capture_start(const Movie *movie, const char *movie_path)
{
    if (!debug_is_runtime_logging_enabled())
        return false;
    const char *name = movie_path ? movie_path : "unknown";
    const char *slash = strrchr(name, '/');
    const char *backslash = strrchr(name, '\\');
    if (backslash && (!slash || backslash > slash))
        slash = backslash;
    if (slash)
        name = slash + 1;
    if (!g_capture)
        g_capture = (PlaybackCapture *)malloc(sizeof(*g_capture));
    if (!g_capture)
        return false;
    memset(g_capture, 0, sizeof(*g_capture));
    g_capture->movie = movie;
    g_capture->active = true;
    g_capture->transition = true;
    g_capture->tick_hz = monotonic_clock_ticks_per_second();
    g_capture->start_frame = movie ? movie->current_frame : 0;
    snprintf(g_capture->media_name, sizeof(g_capture->media_name), "%s", name);
    g_capture->started = monotonic_clock_now_ticks();
    g_capture->accounted = g_capture->started;
    return true;
}

void playback_capture_reset_timeline(const Movie *movie)
{
    if (playback_capture_active(movie))
        g_capture->cadence.valid = false;
}

void playback_capture_tick(const Movie *movie, uint64_t now, bool paused)
{
    if (!playback_capture_active(movie))
        return;
    capture_account(now);
    if (g_capture->account_paused != paused) {
        g_capture->cadence.valid = false;
        g_capture->transition = true;
        memset(g_capture->period, 0, sizeof(g_capture->period));
    }
    g_capture->account_paused = paused;
    ++g_capture->loops;
}

void playback_capture_state(const Movie *movie, const CaptureSettings *settings, uint64_t now)
{
    if (!playback_capture_active(movie))
        return;
    capture_account(now);
    g_capture->account_paused = settings->paused;
    g_capture->end_frame = movie->current_frame;
    if (!g_capture->have_settings || !capture_settings_equal(&g_capture->settings, settings)) {
        if (!g_capture->have_settings || g_capture->settings.paused != settings->paused ||
            g_capture->settings.rate_num != settings->rate_num ||
            g_capture->settings.rate_den != settings->rate_den)
            g_capture->cadence.valid = false;
        if (!g_capture->have_settings)
            g_capture->first_settings = *settings;
        else
            ++g_capture->settings_changes;
        g_capture->settings = *settings;
        g_capture->have_settings = true;
        g_capture->transition = true;
        memset(g_capture->period, 0, sizeof(g_capture->period));
        debug_tracef("capture settings pause=%u rate=%u/%u skip=%u scale=%u subs=%u night=%u/%u",
                     settings->paused, settings->rate_num, settings->rate_den, settings->frame_skip,
                     settings->scale_mode, settings->subtitle_track, settings->night_enabled,
                     settings->night_percent);
    }
}

void playback_capture_stage(const Movie *movie, unsigned stage, uint64_t started, uint64_t ended)
{
    if (!playback_capture_active(movie) || stage >= CAPTURE_STAGE_COUNT)
        return;
    if (stage == CAPTURE_COLOR && g_capture->ahead_scope)
        stage = CAPTURE_AHEAD_COLOR;
    capture_timing_add(&g_capture->timing[stage], ended - started);
    g_capture->period[stage] += ended - started;
}

void playback_capture_ahead_scope(const Movie *movie, bool active)
{
    if (playback_capture_active(movie))
        g_capture->ahead_scope = active;
}

void playback_capture_frame_begin(const Movie *movie, uint32_t target_frame, uint64_t due_ticks,
                                  uint64_t interval_ticks)
{
    if (!playback_capture_active(movie))
        return;
    memset(&g_capture->pending_frame, 0, sizeof(g_capture->pending_frame));
    g_capture->pending_frame.frame = target_frame;
    g_capture->pending_frame.ahead_before =
        (uint16_t)h264_lookahead_prepared_depth(movie, target_frame);
    g_capture->pending_frame.skipped =
        target_frame > movie->current_frame ? target_frame - movie->current_frame - 1U : 0;
    g_capture->pending_frame.due_ticks = due_ticks;
    g_capture->pending_frame.interval_ticks =
        capture_ticks32(interval_ticks, &g_capture->pending_frame.flags);
    g_capture->pending_frame.rate_num = g_capture->settings.rate_num;
    g_capture->pending_frame.rate_den = g_capture->settings.rate_den;
    g_capture->pending_frame.night_percent = g_capture->settings.night_percent;
    g_capture->pending_frame.scale_mode = g_capture->settings.scale_mode;
    if (g_capture->settings.night_enabled)
        g_capture->pending_frame.flags |= CAPTURE_FRAME_NIGHT;
    if (g_capture->settings.frame_skip)
        g_capture->pending_frame.flags |= CAPTURE_FRAME_SKIP_ENABLED;
    g_capture->pending = true;
}

static void capture_finish_frame(const Movie *movie, uint64_t now, bool presented)
{
    unsigned i;
    size_t slot;
    CaptureFrame *frame = &g_capture->pending_frame;
    frame->at_ticks = now;
    frame->chunk = (uint32_t)movie_chunk_for_frame(movie, frame->frame);
    frame->ahead_after = (uint16_t)h264_lookahead_queued(movie);
    frame->ahead_next_frame = h264_lookahead_next_frame(movie);
    if (presented) {
        playback_cadence_present(&g_capture->cadence, now, frame->frame, frame->interval_ticks);
        uint64_t late = now > frame->due_ticks ? now - frame->due_ticks : 0;
        frame->flags |= CAPTURE_FRAME_PRESENTED;
        ++g_capture->presented;
        g_capture->skipped += frame->skipped;
        if (late)
            ++g_capture->presentation_late;
        if (frame->interval_ticks && late >= frame->interval_ticks)
            ++g_capture->over_budget;
        g_capture->lateness_ticks += late;
        if (late > g_capture->max_lateness)
            g_capture->max_lateness = late;
        if (g_capture->presented == 1)
            g_capture->first_present = now;
        g_capture->last_present = now;
    }
    if (g_capture->transition)
        frame->flags |= CAPTURE_FRAME_TRANSITION;
    for (i = 0; i < CAPTURE_STAGE_COUNT; ++i)
        frame->stage[i] = capture_ticks32(g_capture->period[i], &frame->flags);
    if (frame->flags & CAPTURE_FRAME_OVERFLOW)
        ++g_capture->interval_overflows;
    slot = capture_ring_push_index(&g_capture->frame_next, &g_capture->frame_count,
                                   &g_capture->frames_overwritten, CAPTURE_FRAME_CAPACITY);
    g_capture->frames[slot] = *frame;
    g_capture->pending = false;
    g_capture->transition = false;
    memset(g_capture->period, 0, sizeof(g_capture->period));
}

void playback_capture_presented(const Movie *movie, uint64_t now)
{
    if (!playback_capture_active(movie) || !g_capture->pending)
        return;
    capture_finish_frame(movie, now, true);
    playback_capture_stage(movie, CAPTURE_BOOKKEEPING, now, monotonic_clock_now_ticks());
}

void playback_capture_stop(const Movie *movie)
{
    uint64_t now;
    if (!playback_capture_active(movie))
        return;
    now = monotonic_clock_now_ticks();
    capture_account(now);
    if (g_capture->pending)
        capture_finish_frame(g_capture->movie, now, false);
    g_capture->stopped = now;
    g_capture->end_frame =
        g_capture->movie ? g_capture->movie->current_frame : g_capture->end_frame;
    g_capture->active = false;
}

void playback_capture_release(void)
{
    free(g_capture);
    g_capture = NULL;
}

void playback_capture_io(Movie *movie, int kind, int chunk, uint32_t bytes, uint64_t started,
                         uint64_t ended)
{
    CaptureIo *event;
    size_t slot;
    if (!playback_capture_active(movie) || kind <= 0 || kind >= CAPTURE_IO_KIND_COUNT)
        return;
    if (kind == CAPTURE_IO_ASYNC || kind == CAPTURE_IO_ASYNC_SCREENSHOT) {
        /* A read may have started before D enabled/restarted this capture. */
        if (ended <= g_capture->started)
            return;
        if (started < g_capture->started)
            started = g_capture->started;
    }
    if (started == 0 && ended == 0)
        started = ended = monotonic_clock_now_ticks();
    capture_timing_add(&g_capture->io_timing[kind], ended - started);
    g_capture->io_bytes[kind] += bytes;
    if (kind <= CAPTURE_IO_SEEK)
        playback_capture_stage(movie, CAPTURE_IO, started, ended);
    slot = capture_ring_push_index(&g_capture->io_next, &g_capture->io_count,
                                   &g_capture->io_overwritten, CAPTURE_IO_CAPACITY);
    event = &g_capture->io[slot];
    event->at_ticks = started;
    event->duration_ticks = ended - started;
    event->kind = (uint8_t)kind;
    event->chunk = chunk;
    event->bytes = bytes;
}

static void export_settings(FILE *file, const char *label, const CaptureSettings *s)
{
    fprintf(
        file,
        "%s rate=%u/%u paused=%u skip=%u scale=%u subtitle_track=%u font=%u size=%d placement=%u memory_overlay=%u night=%u/%u\n",
        label, s->rate_num, s->rate_den, s->paused, s->frame_skip, s->scale_mode, s->subtitle_track,
        s->subtitle_font, s->subtitle_size, s->subtitle_placement, s->memory_overlay,
        s->night_enabled, s->night_percent);
}

void playback_capture_export(FILE *file, const Movie *movie)
{
    static const char *stage_names[] = {"input_poll",
                                        "decode_inclusive",
                                        "render_present",
                                        "prefetch_inclusive",
                                        "wait",
                                        "io_nested",
                                        "capture_bookkeeping",
                                        "text_format",
                                        "h264_color_nested",
                                        "night_filter_nested",
                                        "lcd_transfer_nested",
                                        "screenshot_encode_foreground",
                                        "decode_ahead_inclusive",
                                        "color_ahead_nested",
                                        "wait_input_nested",
                                        "wait_touchpad_nested",
                                        "writer_service_explicit",
                                        "wait_io_service_nested"};
    static const char *render_reason_names[] = {"scheduled_frame", "frame_changed",
                                                "input",           "effects_transition",
                                                "effects_tick",    "initial",
                                                "pointer",         "memory_refresh",
                                                "night_revision",  "screenshot_revision"};
    unsigned i;
    uint64_t ended, elapsed, active_us;
    if (!playback_capture_available(movie)) {
        fputs("capture=none\n", file);
        return;
    }
    playback_capture_stop(movie);
    ended = g_capture->stopped;
    elapsed = ended - g_capture->started;
    active_us = capture_ticks_to_us(g_capture->active_ticks, g_capture->tick_hz);
    fprintf(file, "capture_version=2 build=%s %s compiler=%s\n", __DATE__, __TIME__, __VERSION__);
    fprintf(file, "media_name=%s\n", g_capture->media_name);
    H264LookaheadStats ahead;
    h264_lookahead_get_stats(movie, &ahead);
    fprintf(
        file,
        "decode_ahead lifetime=1 active=%u capacity=%u queued=%u peak_queued=%u bytes=%lu background_frames=%lu foreground_frames=%lu hits=%lu misses=%lu chunk_waits=%lu cancellations=%lu failures=%lu slices=%lu max_pump_us=%llu max_color_us=%llu max_background_pump_us=%llu max_background_color_us=%llu total_decode_us=%llu total_color_us=%llu partial=%u base_band_rows=%u background_slices_8=%lu background_slices_16=%lu background_slices_32=%lu color_rows_max=64 background_color_bands_16=%lu background_color_bands_32=%lu background_color_bands_64=%lu background_color_bands_tail=%lu background_slices_4=%lu max_background_tail_us=%llu color_tail_guard_us=%llu\n",
        ahead.active ? 1U : 0U, ahead.capacity, ahead.queued, ahead.peak_queued,
        (unsigned long)ahead.allocated_bytes, (unsigned long)ahead.background_frames,
        (unsigned long)ahead.foreground_frames, (unsigned long)ahead.queue_hits,
        (unsigned long)ahead.queue_misses, (unsigned long)ahead.chunk_waits,
        (unsigned long)ahead.cancellations, (unsigned long)ahead.failures,
        (unsigned long)ahead.decode_slices,
        (unsigned long long)capture_ticks_to_us(ahead.max_pump_ticks, g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(ahead.max_color_ticks, g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(ahead.max_background_pump_ticks,
                                                g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(ahead.max_background_color_ticks,
                                                g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(ahead.decode_ticks, g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(ahead.color_ticks, g_capture->tick_hz),
        ahead.partial ? 1U : 0U, H264_LOOKAHEAD_COLOR_ROWS,
        (unsigned long)ahead.background_slices_8, (unsigned long)ahead.background_slices_16,
        (unsigned long)ahead.background_slices_32, (unsigned long)ahead.background_color_bands_16,
        (unsigned long)ahead.background_color_bands_32,
        (unsigned long)ahead.background_color_bands_64,
        (unsigned long)ahead.background_color_bands_tail, (unsigned long)ahead.background_slices_4,
        (unsigned long long)capture_ticks_to_us(ahead.max_background_tail_ticks,
                                                g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(ahead.color_tail_guard_ticks, g_capture->tick_hz));
    fprintf(
        file,
        "decode_ahead_memory budget_bytes=%lu reserve_bytes=%lu free_begin_valid=%u free_begin_bytes=%lu free_last_valid=%u free_last_bytes=%lu checks=%lu headroom_denials=%lu allocation_failures=%lu (DYNA_available_is_not_largest_contiguous_block; checks_only_at_begin_or_new_slot)\n",
        (unsigned long)ahead.budget_bytes, (unsigned long)ahead.reserve_bytes,
        ahead.memory_known_at_begin ? 1U : 0U, (unsigned long)ahead.free_bytes_at_begin,
        ahead.memory_known ? 1U : 0U, (unsigned long)ahead.free_bytes_last,
        (unsigned long)ahead.memory_checks, (unsigned long)ahead.memory_denials,
        (unsigned long)ahead.allocation_failures);
    fprintf(
        file,
        "device hwtype=%u cx2=%u color=1 touchpad=%u lcd=%d clock_hw=%u tick_hz=%lu timer_control_saved=%08x timer_speed_saved=%08x\n",
        hwtype(), is_cx2 ? 1U : 0U, is_touchpad ? 1U : 0U, (int)lcd_type(),
        g_clock.using_hw_timer ? 1U : 0U, (unsigned long)g_capture->tick_hz,
        g_clock.original_control, g_clock.original_speed);
    performance_clock_debug(file);
    if (movie && movie->codec == MOVIE_CODEC_H264)
        fputs("color_conversion flat_reuse=exact_yuv cache_entries=1 cache_scope=conversion_band\n", file);
    fprintf(file,
            "platform_features screen_power_profile=%u standby_profile=%u async_writer_profile=%u detection=driver_code_and_mapping\n",
            native_screen_power_supported() ? 1U : 0U, native_standby_supported() ? 1U : 0U,
            private_writer_supported() ? 1U : 0U);
    if (movie)
        fprintf(
            file,
            "media codec=%s nvp_version=%u flags=%u width=%u height=%u fps=%u/%u frames=%lu chunks=%lu chunk_frames=%u\n",
            movie_codec_name(movie->codec), movie->header.version, movie->header.flags,
            movie->header.video_width, movie->header.video_height, movie->header.fps_num,
            movie->header.fps_den, (unsigned long)movie->header.frame_count,
            (unsigned long)movie->header.chunk_count, movie->header.chunk_frames);
    fprintf(
        file,
        "capture active=%u started_ticks=%llu ended_ticks=%llu elapsed_us=%llu active_us=%llu paused_us=%llu start_frame=%lu end_frame=%lu settings_changes=%lu loops=%lu\n",
        g_capture->active ? 1U : 0U, (unsigned long long)g_capture->started,
        (unsigned long long)ended,
        (unsigned long long)capture_ticks_to_us(elapsed, g_capture->tick_hz),
        (unsigned long long)active_us,
        (unsigned long long)capture_ticks_to_us(g_capture->paused_ticks, g_capture->tick_hz),
        (unsigned long)g_capture->start_frame, (unsigned long)g_capture->end_frame,
        (unsigned long)g_capture->settings_changes, (unsigned long)g_capture->loops);
    export_settings(file, "settings_start", &g_capture->first_settings);
    export_settings(file, "settings_end", &g_capture->settings);
    if (movie && movie->header.fps_den && g_capture->first_settings.rate_den &&
        g_capture->settings.rate_den)
        fprintf(file, "target_fps_start_x1000=%llu target_fps_end_x1000=%llu\n",
                (unsigned long long)((uint64_t)movie->header.fps_num *
                                     g_capture->first_settings.rate_num * 1000U /
                                     (movie->header.fps_den * g_capture->first_settings.rate_den)),
                (unsigned long long)((uint64_t)movie->header.fps_num *
                                     g_capture->settings.rate_num * 1000U /
                                     (movie->header.fps_den * g_capture->settings.rate_den)));
    fprintf(
        file,
        "frames presented=%llu skipped=%llu effective_fps_x1000=%llu late_presentations=%llu over_one_frame_budget=%llu lateness_total_us=%llu lateness_max_us=%llu\n",
        (unsigned long long)g_capture->presented, (unsigned long long)g_capture->skipped,
        (unsigned long long)(active_us ? g_capture->presented * 1000000000ULL / active_us : 0),
        (unsigned long long)g_capture->presentation_late,
        (unsigned long long)g_capture->over_budget,
        (unsigned long long)capture_ticks_to_us(g_capture->lateness_ticks, g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(g_capture->max_lateness, g_capture->tick_hz));
    fprintf(
        file,
        "cadence lag_events=%lu whole_intervals_missed=%lu actual_us=%llu expected_us=%llu slowdown_us=%llu positive_excess_us=%llu max_excess_us=%llu max_gap_us=%llu lag_threshold=one_frame over_budget_intervals=%lu jitter_tolerance_ticks=1\n",
        (unsigned long)g_capture->cadence.events,
        (unsigned long)g_capture->cadence.missed_intervals,
        (unsigned long long)capture_ticks_to_us(g_capture->cadence.elapsed, g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(g_capture->cadence.expected, g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(
            g_capture->cadence.elapsed > g_capture->cadence.expected
                ? g_capture->cadence.elapsed - g_capture->cadence.expected
                : 0,
            g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(g_capture->cadence.excess, g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(g_capture->cadence.max_excess, g_capture->tick_hz),
        (unsigned long long)capture_ticks_to_us(g_capture->cadence.max_gap, g_capture->tick_hz),
        (unsigned long)g_capture->cadence.over_budget_intervals);
    for (i = 0; i < CAPTURE_STAGE_COUNT; ++i)
        fprintf(
            file, "timing %s count=%lu total_us=%llu max_us=%llu\n", stage_names[i],
            (unsigned long)g_capture->timing[i].count,
            (unsigned long long)capture_ticks_to_us(g_capture->timing[i].total, g_capture->tick_hz),
            (unsigned long long)capture_ticks_to_us(g_capture->timing[i].maximum,
                                                    g_capture->tick_hz));
    fprintf(file, "render_reason_presented=%llu (nonexclusive; main render gate only)\n",
            (unsigned long long)g_capture->render_reason_presented);
    for (i = 0; i < CAPTURE_RENDER_REASON_COUNT; ++i)
        fprintf(file, "render_reason %s count=%llu\n", render_reason_names[i],
                (unsigned long long)g_capture->render_reasons[i]);
    for (i = 1; i < CAPTURE_IO_KIND_COUNT; ++i)
        fprintf(file, "io kind=%u count=%lu bytes=%llu total_us=%llu max_us=%llu\n", i,
                (unsigned long)g_capture->io_timing[i].count,
                (unsigned long long)g_capture->io_bytes[i],
                (unsigned long long)capture_ticks_to_us(g_capture->io_timing[i].total,
                                                        g_capture->tick_hz),
                (unsigned long long)capture_ticks_to_us(g_capture->io_timing[i].maximum,
                                                        g_capture->tick_hz));
    fprintf(
        file,
        "capture_storage bytes=%lu frame_capacity=%u retained=%lu overwritten=%llu io_capacity=%u io_retained=%lu io_overwritten=%llu saturated_rows=%lu\n",
        (unsigned long)sizeof(*g_capture), CAPTURE_FRAME_CAPACITY,
        (unsigned long)g_capture->frame_count, (unsigned long long)g_capture->frames_overwritten,
        CAPTURE_IO_CAPACITY, (unsigned long)g_capture->io_count,
        (unsigned long long)g_capture->io_overwritten,
        (unsigned long)g_capture->interval_overflows);
    fputs(
        "capture_notes: due is the scheduled render-start time; smooth mode decodes ahead of due and waits before rendering, skip mode retains clock-driven decoding. over_one_frame_budget means present>=due+interval. Stage times are inclusive: IO/color overlap decode, IO overlaps prefetch, night/LCD overlap render. Background decode/color are reported separately in decode_ahead_us/color_ahead_us and overlap wait; frame/chunk identify presentation, not the decoder horizon. wait_input_us is nested in wait, and wait_touchpad_us is nested in wait_input; their timing counts measure actual wait polls and touchpad bus scans. writer_service_us measures explicit after-present/paused service. wait_io_service_us measures explicit roomy-wait I/O turns and overlaps wait; additional implicit idle work remains inside wait. Foreground H264 color conversion has a separate stage; MPEG4 conversion remains inside decode. Rows sum work since the preceding scheduled presentation; UI-only renders are included in render timing but excluded from effective FPS. Settings transitions reset row accumulators, not totals. Bookkeeping/text_format are measured overhead subsets; timer-read/cache overhead is not calibrated. The detailed capture stays in RAM until export; D also enables the separate asynchronous recovery journal.\n",
        file);
    fputs(
        "frame_flags: 1=presented 2=settings_or_pause_transition 4=timing_saturated 8=night_enabled 16=frame_skip_enabled\n",
        file);
    fputs(
        "io_kinds: 1=sync_read 2=prefetch_read 3=seek_preview_read 4=decoder_reset 5=sequential_decoder_reuse 6=independent_read_wall_time 7=async_screenshot_filesystem_wall_time (6/7 asynchronous wall time, not CPU execution time; spans crossing D-start are clipped)\n",
        file);
    fputs(
        "frames_csv: at_us,frame,chunk,due_us,lateness_us,interval_us,skipped,rate_num,rate_den,flags,night_percent,scale_mode,input_us,decode_us,render_us,prefetch_us,wait_us,io_us,bookkeeping_us,text_format_us,h264_color_us,night_filter_us,lcd_transfer_us,screenshot_encode_us,decode_ahead_us,color_ahead_us,wait_input_us,wait_touchpad_us,writer_service_us,wait_io_service_us,ahead_before,ahead_after,ahead_next_frame\n",
        file);
    for (i = 0; i < g_capture->frame_count; ++i) {
        size_t index = (capture_ring_oldest(g_capture->frame_next, g_capture->frame_count,
                                            CAPTURE_FRAME_CAPACITY) +
                        i) %
                       CAPTURE_FRAME_CAPACITY;
        const CaptureFrame *f = &g_capture->frames[index];
        unsigned j;
        int64_t due_relative = (int64_t)(f->due_ticks - g_capture->started);
        int64_t late = (int64_t)(f->at_ticks - f->due_ticks);
        fprintf(file, "%llu,%lu,%ld,%lld,%lld,%llu,%lu,%u,%u,%u,%u,%u",
                (unsigned long long)capture_ticks_to_us(f->at_ticks - g_capture->started,
                                                        g_capture->tick_hz),
                (unsigned long)f->frame, (long)(int32_t)f->chunk,
                (long long)(due_relative < 0
                                ? -(int64_t)capture_ticks_to_us((uint64_t)-due_relative,
                                                                g_capture->tick_hz)
                                : (int64_t)capture_ticks_to_us(due_relative, g_capture->tick_hz)),
                (long long)(late < 0
                                ? -(int64_t)capture_ticks_to_us((uint64_t)-late, g_capture->tick_hz)
                                : (int64_t)capture_ticks_to_us(late, g_capture->tick_hz)),
                (unsigned long long)capture_ticks_to_us(f->interval_ticks, g_capture->tick_hz),
                (unsigned long)f->skipped, f->rate_num, f->rate_den, f->flags, f->night_percent,
                f->scale_mode);
        for (j = 0; j < CAPTURE_STAGE_COUNT; ++j)
            fprintf(file, ",%llu",
                    (unsigned long long)capture_ticks_to_us(f->stage[j], g_capture->tick_hz));
        fprintf(file, ",%u,%u,%lu\n", f->ahead_before, f->ahead_after,
                (unsigned long)f->ahead_next_frame);
    }
    fputs("io_csv: at_us,kind,chunk,bytes,duration_us\n", file);
    for (i = 0; i < g_capture->io_count; ++i) {
        size_t index =
            (capture_ring_oldest(g_capture->io_next, g_capture->io_count, CAPTURE_IO_CAPACITY) +
             i) %
            CAPTURE_IO_CAPACITY;
        const CaptureIo *e = &g_capture->io[index];
        fprintf(file, "%llu,%u,%ld,%lu,%llu\n",
                (unsigned long long)capture_ticks_to_us(e->at_ticks - g_capture->started,
                                                        g_capture->tick_hz),
                e->kind, (long)e->chunk, (unsigned long)e->bytes,
                (unsigned long long)capture_ticks_to_us(e->duration_ticks, g_capture->tick_hz));
    }
}
