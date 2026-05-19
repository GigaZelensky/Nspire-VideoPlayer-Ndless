#include "player_internal.h"

bool sram_movie_chunk_buffer_can_hold(size_t size)
{
    return g_sram_movie_chunk_buffer && size > 0 && size <= g_sram_movie_chunk_buffer_size;
}

void release_movie_chunk_storage(Movie *movie)
{
    if (!movie) {
        return;
    }
    if (movie->chunk_storage && !movie->chunk_storage_in_sram) {
        free(movie->chunk_storage);
    }
    movie->chunk_storage = NULL;
    movie->chunk_storage_size = 0;
    movie->chunk_storage_in_sram = false;
}

bool allocate_movie_chunk_storage(Movie *movie, size_t size)
{
    if (!movie || size == 0) {
        return false;
    }
    release_movie_chunk_storage(movie);
    if (sram_movie_chunk_buffer_can_hold(size)) {
        movie->chunk_storage = g_sram_movie_chunk_buffer;
        movie->chunk_storage_in_sram = true;
        return true;
    }

    movie->chunk_storage = (uint8_t *) malloc(size);
    movie->chunk_storage_in_sram = false;
    return movie->chunk_storage != NULL;
}

bool adopt_movie_chunk_storage(Movie *movie, uint8_t **storage, size_t size)
{
    uint8_t *owned_storage;

    if (!movie || !storage || !*storage || size == 0) {
        return false;
    }

    owned_storage = *storage;
    release_movie_chunk_storage(movie);
    if (sram_movie_chunk_buffer_can_hold(size)) {
        memcpy(g_sram_movie_chunk_buffer, owned_storage, size);
        free(owned_storage);
        movie->chunk_storage = g_sram_movie_chunk_buffer;
        movie->chunk_storage_in_sram = true;
    } else {
        movie->chunk_storage = owned_storage;
        movie->chunk_storage_in_sram = false;
    }
    movie->chunk_storage_size = size;
    *storage = NULL;
    return true;
}

static bool h264_codec_global_init(void)
{
    return init_h264_color_tables();
}

static bool h264_codec_open(Movie *movie)
{
    if (!movie) {
        return false;
    }

    init_sram_movie_chunk_buffer();
    movie->h264.decoder = h264bsdAlloc();
    if (movie->h264.decoder) {
        memset(movie->h264.decoder, 0, sizeof(*movie->h264.decoder));
    }
    if (!movie->h264.decoder) {
        debug_failf("open failed: h264 decoder alloc");
        return false;
    }
    return reset_h264_decoder(movie);
}

static void h264_codec_destroy(Movie *movie)
{
    if (!movie || !movie->h264.decoder) {
        return;
    }
    if (movie->h264.decoder_initialized) {
        h264bsdShutdown(movie->h264.decoder);
    }
    h264bsdFree(movie->h264.decoder);
    memset(&movie->h264, 0, sizeof(movie->h264));
}

static bool h264_codec_supports_incremental_seek_preview(const Movie *movie)
{
    (void) movie;
    return true;
}

static bool mpeg4_codec_open(Movie *movie)
{
    if (!movie) {
        return false;
    }
    if (!mpeg4_xvid_create(
            &movie->mpeg4.decoder,
            (int) movie->header.video_width,
            (int) movie->header.video_height)) {
        debug_failf("open failed: mpeg4 decoder alloc: %s", mpeg4_xvid_last_error());
        return false;
    }
    return true;
}

static void mpeg4_codec_destroy(Movie *movie)
{
    if (!movie || !movie->mpeg4.decoder) {
        return;
    }
    mpeg4_xvid_destroy(movie->mpeg4.decoder);
    memset(&movie->mpeg4, 0, sizeof(movie->mpeg4));
}

static bool mpeg4_codec_supports_incremental_seek_preview(const Movie *movie)
{
    (void) movie;
    return false;
}

static const MovieCodecOps g_h264_codec_ops = {
    MOVIE_CODEC_H264,
    "h264",
    h264_codec_global_init,
    h264_codec_open,
    h264_codec_destroy,
    reset_h264_decoder,
    decode_h264_frame,
    h264_codec_supports_incremental_seek_preview
};

static const MovieCodecOps g_mpeg4_codec_ops = {
    MOVIE_CODEC_MPEG4,
    "mpeg4",
    init_mpeg4_decoder_global,
    mpeg4_codec_open,
    mpeg4_codec_destroy,
    reset_mpeg4_decoder,
    decode_mpeg4_frame,
    mpeg4_codec_supports_incremental_seek_preview
};

const MovieCodecOps *movie_codec_ops(MovieCodec codec)
{
    switch (codec) {
    case MOVIE_CODEC_H264:
        return &g_h264_codec_ops;
    case MOVIE_CODEC_MPEG4:
        return &g_mpeg4_codec_ops;
    default:
        return NULL;
    }
}

void destroy_movie(Movie *movie)
{
    uint32_t index;
    int prefetch_index;
    if (!movie) {
        return;
    }
    if (movie->file) {
        fclose(movie->file);
    }
    if (movie->frame_surface) {
        SDL_FreeSurface(movie->frame_surface);
    }
    if (movie->subtitles) {
        for (index = 0; index < movie->header.subtitle_count; ++index) {
            free(movie->subtitles[index].text);
        }
    }
    if (movie->subtitle_tracks) {
        for (index = 0; index < movie->subtitle_track_count; ++index) {
            free(movie->subtitle_tracks[index].name);
        }
    }
    free(movie->subtitles);
    free(movie->subtitle_tracks);
    free(movie->chunk_index);
    free(movie->framebuffer);
    release_movie_chunk_storage(movie);
    free(movie->frame_offsets);
    if (movie->codec_ops && movie->codec_ops->destroy) {
        movie->codec_ops->destroy(movie);
    }
    for (prefetch_index = 0; prefetch_index < PREFETCH_CHUNK_COUNT; ++prefetch_index) {
        clear_prefetched_chunk(&movie->prefetched[prefetch_index]);
    }
    memset(movie, 0, sizeof(*movie));
    movie->loaded_chunk = -1;
    for (prefetch_index = 0; prefetch_index < PREFETCH_CHUNK_COUNT; ++prefetch_index) {
        movie->prefetched[prefetch_index].chunk_index = -1;
    }
    movie->decoded_local_frame = -1;
}

void defer_playback_movie_cleanup(Movie *movie)
{
    if (!movie) {
        return;
    }
    if (movie->file) {
        fclose(movie->file);
        movie->file = NULL;
    }
    release_movie_chunk_storage(movie);
    g_deferred_playback_movie = movie;
}

void cleanup_deferred_playback_movie(void)
{
    if (!g_deferred_playback_movie) {
        return;
    }
    destroy_movie(g_deferred_playback_movie);
    g_deferred_playback_movie = NULL;
}

bool init_fonts(Fonts *fonts)
{
    size_t index;

    memset(fonts, 0, sizeof(*fonts));
    fonts->white = nSDL_LoadFont(NSDL_FONT_TINYTYPE, 255, 255, 255);
    fonts->outline = nSDL_LoadFont(NSDL_FONT_TINYTYPE, 0, 0, 0);
    for (index = 0; index < SUBTITLE_FONT_CHOICE_COUNT; ++index) {
        int font_id = g_subtitle_font_choices[index];
        fonts->subtitle_white[font_id] = nSDL_LoadFont(font_id, 255, 255, 255);
        fonts->subtitle_outline[font_id] = nSDL_LoadFont(font_id, 0, 0, 0);
        if (fonts->subtitle_white[font_id]) {
            nSDL_SetFontSpacing(fonts->subtitle_white[font_id], 0, 0);
        }
        if (fonts->subtitle_outline[font_id]) {
            nSDL_SetFontSpacing(fonts->subtitle_outline[font_id], 0, 0);
        }
    }
    if (!fonts->white || !fonts->outline) {
        free_fonts(fonts);
        return false;
    }
    for (index = 0; index < SUBTITLE_FONT_CHOICE_COUNT; ++index) {
        int font_id = g_subtitle_font_choices[index];
        if (!fonts->subtitle_white[font_id] || !fonts->subtitle_outline[font_id]) {
            free_fonts(fonts);
            return false;
        }
    }
    if (!fonts->subtitle_white[NSDL_FONT_TINYTYPE] || !fonts->subtitle_outline[NSDL_FONT_TINYTYPE]) {
        free_fonts(fonts);
        return false;
    }
    return true;
}

void free_fonts(Fonts *fonts)
{
    int font_id;

    if (fonts->white) {
        nSDL_FreeFont(fonts->white);
    }
    if (fonts->outline) {
        nSDL_FreeFont(fonts->outline);
    }
    for (font_id = 0; font_id < NSP_NUMFONTS; ++font_id) {
        if (fonts->subtitle_white[font_id]) {
            nSDL_FreeFont(fonts->subtitle_white[font_id]);
        }
        if (fonts->subtitle_outline[font_id]) {
            nSDL_FreeFont(fonts->subtitle_outline[font_id]);
        }
    }
    memset(fonts, 0, sizeof(*fonts));
}

bool movie_uses_h264(const Movie *movie)
{
    return movie && movie->codec == MOVIE_CODEC_H264;
}

bool init_mpeg4_decoder_global(void)
{
    static bool attempted = false;
    static bool initialized = false;
    static void *sram_pool = NULL;
    static unsigned int sram_pool_size = 0;

    if (initialized) {
        return true;
    }
    if (!attempted && sram_is_enabled()) {
        sram_pool = sram_alloc(MPEG4_XVID_SRAM_POOL_BYTES, 32U);
        if (sram_pool) {
            sram_pool_size = MPEG4_XVID_SRAM_POOL_BYTES;
        }
    }
    attempted = true;
    initialized = mpeg4_xvid_global_init(sram_pool, sram_pool_size);
    if (!initialized) {
        debug_failf("mpeg4 init failed: %s", mpeg4_xvid_last_error());
    } else {
        debug_tracef(
            "mpeg4 init ok sram=%lu",
            (unsigned long) sram_pool_size
        );
    }
    return initialized;
}

bool init_h264_color_tables(void)
{
    int index;
    H264ColorTables *tables = g_h264_color_tables;

    if (tables->initialized) {
        return true;
    }
    if (tables == &g_h264_color_tables_storage) {
        H264ColorTables *sram_tables = (H264ColorTables *) sram_alloc(sizeof(*sram_tables), 32U);
        if (sram_tables) {
            memset(sram_tables, 0, sizeof(*sram_tables));
            g_h264_color_tables = sram_tables;
            g_h264_color_tables_in_sram = true;
            tables = sram_tables;
        } else {
            memset(&g_h264_color_tables_storage, 0, sizeof(g_h264_color_tables_storage));
            g_h264_color_tables = &g_h264_color_tables_storage;
            g_h264_color_tables_in_sram = false;
            tables = &g_h264_color_tables_storage;
        }
    }

    for (index = 0; index < 256; ++index) {
        int y = index - 16;
        int chroma = index - 128;
        if (y < 0) {
            y = 0;
        }
        tables->y_base[index] = (298 * y) + 128;
        tables->u_to_blue[index] = 516 * chroma;
        tables->u_to_green[index] = -100 * chroma;
        tables->v_to_red[index] = 409 * chroma;
        tables->v_to_green[index] = -208 * chroma;
        tables->red565[index] = (uint16_t) ((index & 0xF8) << 8);
        tables->green565[index] = (uint16_t) ((index & 0xFC) << 3);
        tables->blue565[index] = (uint16_t) (index >> 3);
    }

    for (index = 0; index < H264_CLIP_TABLE_SIZE; ++index) {
        int value = index - H264_CLIP_OFFSET;
        if (value < 0) {
            value = 0;
        } else if (value > 255) {
            value = 255;
        }
        tables->clip[index] = (uint8_t) value;
    }

    tables->initialized = true;
    return true;
}

void init_sram_movie_chunk_buffer(void)
{
    if (g_sram_movie_chunk_buffer || !sram_is_enabled()) {
        return;
    }

    g_sram_movie_chunk_buffer = (uint8_t *) sram_alloc(SRAM_MOVIE_CHUNK_BUFFER_BYTES, 32U);
    if (g_sram_movie_chunk_buffer) {
        g_sram_movie_chunk_buffer_size = SRAM_MOVIE_CHUNK_BUFFER_BYTES;
        debug_tracef(
            "sram current chunk buffer=%lu used=%lu/%lu",
            (unsigned long) g_sram_movie_chunk_buffer_size,
            (unsigned long) sram_bytes_used(),
            (unsigned long) sram_bytes_capacity()
        );
    } else {
        g_sram_movie_chunk_buffer_size = 0;
        debug_tracef(
            "sram current chunk buffer unavailable used=%lu/%lu",
            (unsigned long) sram_bytes_used(),
            (unsigned long) sram_bytes_capacity()
        );
    }
}


uint32_t h264_prefetch_io_min_spare_ms(const Movie *movie)
{
    if (movie && movie_uses_h264(movie)) {
        if (movie->h264.foreground_decode_peak_ms >= H264_FOREGROUND_DECODE_HARD_MS) {
            return 10U;
        }
        if (movie->h264.foreground_decode_avg_ms >= H264_FOREGROUND_DECODE_SOFT_MS) {
            return 11U;
        }
    }
    return PREFETCH_ACTIVE_H264_MIN_SPARE_MS;
}

void debug_trace_runtime_snapshot(
    Movie *movie,
    bool paused,
    uint32_t spare_ms,
    const PlaybackRate *playback_rate,
    const char *tag
)
{
    MemoryStats stats;

    if (!movie) {
        return;
    }

    stats = query_memory_stats(movie);
    debug_tracef(
        "snap %s pause=%u rate=%s frame=%lu chunk=%d spare=%lu mem=%u fg=%u/%u direct=%lu replay=%lu chunkpref=%lu",
        tag ? tag : "-",
        paused ? 1U : 0U,
        playback_rate ? playback_rate->label : "-",
        (unsigned long) movie->current_frame,
        movie->loaded_chunk,
        (unsigned long) spare_ms,
        stats.percent_used,
        (unsigned) movie->h264.foreground_decode_avg_ms,
        (unsigned) movie->h264.foreground_decode_peak_ms,
        (unsigned long) movie->diag_foreground_direct_decode_count,
        (unsigned long) movie->diag_h264_replay_count,
        (unsigned long) total_prefetched_chunk_bytes(movie)
    );
}

void debug_dump_session(const char *path, const Movie *movie, const char *reason)
{
    FILE *log_file;
    size_t index;
    bool clip_in_sram = false;
    bool qpc_in_sram = false;
    bool deblocking_in_sram = false;

    if (!path) {
        return;
    }

    log_file = fopen(path, "wb");
    if (!log_file) {
        return;
    }

    fputs("ND Video Player diagnostic log\n", log_file);
    fprintf(log_file, "reason=%s\n", reason ? reason : "unknown");
    fprintf(log_file, "last_error=%s\n", debug_last_error());
    fprintf(log_file, "verbose_logging=%u\n", debug_is_runtime_logging_enabled() ? 1U : 0U);
    fprintf(log_file, "metrics_collection=%u\n", debug_should_collect_metrics() ? 1U : 0U);
    h264bsdGetSramStatus(&clip_in_sram, &qpc_in_sram, &deblocking_in_sram);
    fprintf(
        log_file,
        "sram enabled=%u used=%lu cap=%lu state=%s color=%u clip=%u qpc=%u deblock=%u\n",
        sram_is_enabled() ? 1U : 0U,
        (unsigned long) sram_bytes_used(),
        (unsigned long) sram_bytes_capacity(),
        sram_status_message(),
        g_h264_color_tables_in_sram ? 1U : 0U,
        clip_in_sram ? 1U : 0U,
        qpc_in_sram ? 1U : 0U,
        deblocking_in_sram ? 1U : 0U
    );

    if (movie) {
        MemoryStats stats = query_memory_stats(movie);
        fprintf(
            log_file,
            "frame=%lu/%lu loaded_chunk=%d decoded_local=%d mem_used=%lu mem_prefetched=%lu mem_free=%lu mem_pct=%u\n",
            (unsigned long) movie->current_frame,
            (unsigned long) movie->header.frame_count,
            movie->loaded_chunk,
            movie->decoded_local_frame,
            (unsigned long) stats.used_bytes,
            (unsigned long) stats.prefetched_bytes,
            (unsigned long) stats.free_bytes,
            stats.percent_used
        );
        fprintf(
            log_file,
            "fg_decode count=%lu direct=%lu avg_ms=%u peak_ms=%u lag_events=%lu lag_frames_total=%lu max_lag_frames=%lu max_late_ms=%lu\n",
            (unsigned long) movie->diag_foreground_decode_count,
            (unsigned long) movie->diag_foreground_direct_decode_count,
            (unsigned) movie->h264.foreground_decode_avg_ms,
            (unsigned) movie->h264.foreground_decode_peak_ms,
            (unsigned long) movie->diag_lag_event_count,
            (unsigned long) movie->diag_lag_frame_total,
            (unsigned long) movie->diag_max_lag_frames,
            (unsigned long) movie->diag_max_late_ms
        );
        fprintf(
            log_file,
            "prefetch ticks=%lu active_ticks=%lu io_priority=%lu chunk_prefetched=%lu\n",
            (unsigned long) movie->diag_prefetch_tick_count,
            (unsigned long) movie->diag_active_prefetch_tick_count,
            (unsigned long) movie->diag_io_priority_count,
            (unsigned long) total_prefetched_chunk_bytes(movie)
        );
        fprintf(
            log_file,
            "chunk loads_sync=%lu loads_prefetched=%lu read_ops=%lu read_bytes=%lu max_spare_ms=%lu\n",
            (unsigned long) movie->diag_chunk_load_sync_count,
            (unsigned long) movie->diag_chunk_load_prefetched_count,
            (unsigned long) movie->diag_prefetch_read_ops,
            (unsigned long) movie->diag_prefetch_read_bytes,
            (unsigned long) movie->diag_max_spare_ms
        );
        fprintf(
            log_file,
            "replay count=%lu frames=%lu max_distance=%lu\n",
            (unsigned long) movie->diag_h264_replay_count,
            (unsigned long) movie->diag_h264_replay_frames_total,
            (unsigned long) movie->diag_h264_replay_max_distance
        );
    }

    fputs("recent_events:\n", log_file);
    for (index = 0; index < g_debug_ring_count; ++index) {
        size_t ring_index = (g_debug_ring_next + DEBUG_RING_SIZE - g_debug_ring_count + index) % DEBUG_RING_SIZE;
        fputs(g_debug_ring[ring_index], log_file);
        fputc('\n', log_file);
    }

    fclose(log_file);
}

void debug_log_sram_status(void)
{
    bool clip_in_sram = false;
    bool qpc_in_sram = false;
    bool deblocking_in_sram = false;

    h264bsdGetSramStatus(&clip_in_sram, &qpc_in_sram, &deblocking_in_sram);
    debug_tracef(
        "sram status enabled=%u used=%lu/%lu state=%s color=%u clip=%u qpc=%u deblock=%u",
        sram_is_enabled() ? 1U : 0U,
        (unsigned long) sram_bytes_used(),
        (unsigned long) sram_bytes_capacity(),
        sram_status_message(),
        g_h264_color_tables_in_sram ? 1U : 0U,
        clip_in_sram ? 1U : 0U,
        qpc_in_sram ? 1U : 0U,
        deblocking_in_sram ? 1U : 0U
    );
}

