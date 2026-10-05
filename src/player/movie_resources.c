#include "player_internal.h"
#include "crash_recorder.h"
#include "storage_read_stream.h"
#include "performance_clock.h"
#if NDVIDEO_CODEC_MODULES
#include "codecs/codec_module_api.h"
#include "codecs/modules/h264_module_api.h"
#endif

static void decoder_allocation_error(const char *codec)
{
#if NDVIDEO_CODEC_MODULES
    const char *detail = codec_module_error();
    if (detail && *detail) {
        debug_failf("open failed: %s: %s", codec, detail);
        return;
    }
#endif
    debug_failf("open failed: %s decoder allocation", codec);
}

/* The large SRAM arena holds compressed input or planar decoder scratch.
 * Same-codec instances share only temporaries between complete decode steps. */
static Movie *sram_chunk_owner;
static unsigned hevc_sram_users, av1_sram_users;

static bool player_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1U)) == 0;
}

unsigned movie_prefetch_slots(const Movie *movie)
{
    return movie && movie->prefetch_slots && movie->prefetch_slots <= PREFETCH_CHUNK_COUNT
        ? movie->prefetch_slots : PREFETCH_CHUNK_COUNT;
}

void movie_configure_prefetch(Movie *movie)
{
    movie->prefetch_slots = PREFETCH_CHUNK_COUNT;
    if (!movie_uses_planar_decoder(movie) || movie->header.chunk_count < 4U ||
        !movie->header.fps_num || !movie->header.fps_den) return;
    /* Long chunks can cover ample read-ahead with fewer bins. Four slots
     * retain at least 16 seconds at 1x (four seconds at 4x); otherwise try
     * eight slots for that same coverage, then sixteen for shorter chunks. */
    for (unsigned slots = 4U; slots <= 8U; slots *= 2U) {
        if (movie->header.chunk_count < slots) continue;
        uint64_t frames = 0;
        bool sufficient = true;
        for (uint32_t i = 0; i < movie->header.chunk_count; ++i) {
            frames += movie->chunk_index[i].frame_count;
            if (i >= slots) frames -= movie->chunk_index[i - slots].frame_count;
            if (i + 1U >= slots && frames * movie->header.fps_den <
                (uint64_t)movie->header.fps_num * 16U) {
                sufficient = false;
                break;
            }
        }
        if (sufficient) {
            movie->prefetch_slots = slots;
            return;
        }
    }
}

size_t movie_lookahead_storage_bound(const Movie *movie)
{
    uint32_t largest_chunk=0,largest_frames=0;
    uint64_t rounded,prefetch,total;
    if(!movie || !movie->chunk_index || !movie->header.chunk_count)return SIZE_MAX;
    for(uint32_t i=0;i<movie->header.chunk_count;++i){
        const ChunkIndexEntry *entry=&movie->chunk_index[i];
        if(!entry->packed_size || !entry->unpacked_size || !entry->frame_count)return SIZE_MAX;
        if(entry->packed_size>largest_chunk)largest_chunk=entry->packed_size;
        if(entry->unpacked_size>largest_chunk)largest_chunk=entry->unpacked_size;
        if(entry->frame_count>largest_frames)largest_frames=entry->frame_count;
    }
    /* Retained prefetch bins can outlive their chunk IDs. Bound all slots by
     * the largest indexed bin, not only a contiguous window of current IDs.
     * Three additional bins cover main/replacement/seek-preview storage; two
     * offset arrays cover owned unaligned tables and their replacement. */
    rounded=((uint64_t)largest_chunk+4095U)&~(uint64_t)4095U;
    prefetch=rounded*movie_prefetch_slots(movie);
    if(prefetch>PREFETCH_MAX_TOTAL_BYTES)prefetch=PREFETCH_MAX_TOTAL_BYTES;
    total=prefetch+3U*rounded+2U*(uint64_t)largest_frames*sizeof(uint32_t);
    return total>SIZE_MAX?SIZE_MAX:(size_t)total;
}

static bool movie_storage_add(size_t *total,size_t bytes)
{
    if(bytes>SIZE_MAX-*total)return false;
    *total+=bytes;return true;
}

size_t movie_lookahead_storage_bytes(const Movie *movie)
{
    size_t total=0;
    if(!movie)return 0;
    for(unsigned i=0;i<PREFETCH_CHUNK_COUNT;++i)
        if(movie->prefetched[i].chunk_storage &&
           !movie_storage_add(&total,movie->prefetched[i].chunk_capacity))return 0;
    if(movie->chunk_storage && !movie->chunk_storage_in_sram &&
       !movie_storage_add(&total,movie->chunk_storage_capacity))return 0;
    if(movie->frame_offsets_allocation){
        if(movie->frame_offsets_capacity>SIZE_MAX/sizeof(uint32_t) ||
           !movie_storage_add(&total,movie->frame_offsets_capacity*sizeof(uint32_t)))return 0;
    }
    /* An inconsistent/overflowed total grants no credit against future growth.
     * Existing pixels, decoder/debug/UI allocations are already reflected in
     * the native free-pool reading and must not be credited a second time. */
    return total;
}

void *player_malloc_aligned(size_t size, size_t alignment, uint8_t **allocation)
{
    uint8_t *raw;
    uintptr_t aligned_address;
    size_t extra;

    if (allocation) {
        *allocation = NULL;
    }
    if (size == 0 || !player_is_power_of_two(alignment) || alignment > 255U) {
        return NULL;
    }

    extra = alignment;
    if (size + extra < size) {
        return NULL;
    }

    raw = (uint8_t *) malloc(size + extra);
    if (!raw) {
        return NULL;
    }

    aligned_address = ((uintptr_t) raw + alignment - 1U) & ~((uintptr_t) alignment - 1U);
    if ((uint8_t *) aligned_address == raw) {
        aligned_address += alignment;
    }

    ((uint8_t *) aligned_address)[-1] = (uint8_t) ((uint8_t *) aligned_address - raw);
    if (allocation) {
        *allocation = raw;
    }
    return (void *) aligned_address;
}

void *player_calloc_aligned(size_t count, size_t element_size, size_t alignment, uint8_t **allocation)
{
    size_t total_size;
    void *ptr;

    if (element_size != 0 && count > ((size_t) -1) / element_size) {
        return NULL;
    }

    total_size = count * element_size;
    ptr = player_malloc_aligned(total_size, alignment, allocation);
    if (ptr) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

void player_free_aligned(void *ptr, uint8_t *allocation)
{
    if (!ptr) {
        return;
    }
    if (allocation) {
        free(allocation);
    } else {
        uint8_t *aligned = (uint8_t *) ptr;
        free(aligned - aligned[-1]);
    }
}

void player_copy_maybe_fast(void *dest, const void *src, size_t size)
{
#if defined(__arm__) || defined(__ARM_ARCH)
    size_t chunk_count;
    size_t chunk_bytes;

    if (dest && src && size >= PLAYER_CACHE_LINE_SIZE &&
            ((((uintptr_t) dest) | ((uintptr_t) src)) & 3U) == 0) {
        /* LDM/STM require word alignment, not cache-line alignment. SDL's
         * surfaces need not be 32-byte aligned; they can still use this loop. */
        chunk_count = size / PLAYER_CACHE_LINE_SIZE;
        if (chunk_count <= UINT32_MAX) {
            chunk_bytes = chunk_count * PLAYER_CACHE_LINE_SIZE;
            FastMemcpy(dest, src, (uint32_t) chunk_count);
            if (chunk_bytes < size) {
                memcpy((uint8_t *) dest + chunk_bytes, (const uint8_t *) src + chunk_bytes, size - chunk_bytes);
            }
            return;
        }
    }
#endif

    memcpy(dest, src, size);
}

bool sram_movie_chunk_buffer_can_hold(const Movie *movie, size_t size)
{
    return g_sram_movie_chunk_buffer && !hevc_sram_users && !av1_sram_users &&
        (!sram_chunk_owner || sram_chunk_owner == movie) &&
        size > 0 && size <= g_sram_movie_chunk_buffer_size;
}

void release_movie_chunk_storage(Movie *movie)
{
    if (!movie) {
        return;
    }
    if (movie->chunk_storage && !movie->chunk_storage_in_sram) {
        player_free_aligned(movie->chunk_storage, movie->chunk_storage_allocation);
    }
    if (sram_chunk_owner == movie) sram_chunk_owner = NULL;
    movie->chunk_storage = NULL;
    movie->chunk_storage_allocation = NULL;
    movie->chunk_storage_size = 0;
    movie->chunk_storage_capacity = 0;
    movie->chunk_storage_in_sram = false;
    movie->frame_offsets = NULL;
    movie->chunk_bytes = NULL;
    movie->chunk_size = 0;
    movie->loaded_chunk = -1;
    movie->decoded_local_frame = -1;
}

bool allocate_movie_chunk_storage(Movie *movie, size_t size)
{
    if (!movie || size == 0) {
        return false;
    }
    if (movie->chunk_storage && movie->chunk_storage_capacity >= size &&
        (!sram_movie_chunk_buffer_can_hold(movie, size) || movie->chunk_storage_in_sram)) {
        movie->chunk_storage_size = 0;
        movie->frame_offsets = NULL;
        movie->chunk_bytes = NULL;
        movie->chunk_size = 0;
        movie->loaded_chunk = -1;
        movie->decoded_local_frame = -1;
        return true;
    }
    release_movie_chunk_storage(movie);
    if (sram_movie_chunk_buffer_can_hold(movie, size)) {
        movie->chunk_storage = g_sram_movie_chunk_buffer;
        movie->chunk_storage_capacity = g_sram_movie_chunk_buffer_size;
        movie->chunk_storage_in_sram = true;
        sram_chunk_owner = movie;
        return true;
    }

    movie->chunk_storage = (uint8_t *) player_malloc_aligned(
        size,
        PLAYER_CACHE_LINE_SIZE,
        &movie->chunk_storage_allocation
    );
    movie->chunk_storage_in_sram = false;
    movie->chunk_storage_capacity = movie->chunk_storage ? size : 0;
    return movie->chunk_storage != NULL;
}

bool adopt_movie_chunk_storage_owned(Movie *movie, uint8_t **storage, uint8_t **allocation, size_t size)
{
    uint8_t *owned_storage;
    uint8_t *owned_allocation = NULL;

    if (!movie || !storage || !*storage || size == 0) {
        return false;
    }

    owned_storage = *storage;
    if (allocation) {
        owned_allocation = *allocation;
    }
    release_movie_chunk_storage(movie);
    if (sram_movie_chunk_buffer_can_hold(movie, size)) {
        player_copy_maybe_fast(g_sram_movie_chunk_buffer, owned_storage, size);
        player_free_aligned(owned_storage, owned_allocation ? owned_allocation : owned_storage);
        movie->chunk_storage = g_sram_movie_chunk_buffer;
        movie->chunk_storage_capacity = g_sram_movie_chunk_buffer_size;
        movie->chunk_storage_in_sram = true;
        sram_chunk_owner = movie;
    } else {
        movie->chunk_storage = owned_storage;
        movie->chunk_storage_capacity = size;
        movie->chunk_storage_allocation = owned_allocation ? owned_allocation : owned_storage;
        movie->chunk_storage_in_sram = false;
    }
    movie->chunk_storage_size = size;
    *storage = NULL;
    if (allocation) {
        *allocation = NULL;
    }
    return true;
}

bool adopt_movie_chunk_storage(Movie *movie, uint8_t **storage, size_t size)
{
    return adopt_movie_chunk_storage_owned(movie, storage, NULL, size);
}

bool adopt_prefetched_movie_chunk(Movie *movie, PrefetchedChunk *chunk)
{
    uint8_t *previous_storage;
    uint8_t *previous_allocation;
    size_t previous_capacity;

    if (!movie || !chunk || !chunk->chunk_storage ||
        chunk->state != PREFETCH_READY || chunk->chunk_storage_size == 0) {
        return false;
    }
    if (sram_movie_chunk_buffer_can_hold(movie, chunk->chunk_storage_size)) {
        /* Keep SRAM's decode locality, but retain the source allocation for
         * the next flash read instead of freeing and reallocating each chunk. */
        release_movie_chunk_storage(movie);
        player_copy_maybe_fast(g_sram_movie_chunk_buffer, chunk->chunk_storage, chunk->chunk_storage_size);
        movie->chunk_storage = g_sram_movie_chunk_buffer;
        movie->chunk_storage_capacity = g_sram_movie_chunk_buffer_size;
        movie->chunk_storage_in_sram = true;
        sram_chunk_owner = movie;
    } else {
        /* For larger chunks transfer ownership without copying. The consumed
         * prefetch slot takes the previous RAM buffer and can refill it. */
        previous_storage = movie->chunk_storage_in_sram ? NULL : movie->chunk_storage;
        previous_allocation = movie->chunk_storage_in_sram ? NULL : movie->chunk_storage_allocation;
        previous_capacity = movie->chunk_storage_in_sram ? 0 : movie->chunk_storage_capacity;
        if (sram_chunk_owner == movie) sram_chunk_owner = NULL;
        movie->chunk_storage = chunk->chunk_storage;
        movie->chunk_storage_allocation = chunk->chunk_allocation;
        movie->chunk_storage_capacity = chunk->chunk_capacity;
        movie->chunk_storage_in_sram = false;
        chunk->chunk_storage = previous_storage;
        chunk->chunk_allocation = previous_allocation;
        chunk->chunk_capacity = previous_capacity;
    }
    movie->chunk_storage_size = chunk->chunk_storage_size;
    movie->frame_offsets = NULL;
    movie->chunk_bytes = NULL;
    movie->chunk_size = 0;
    movie->loaded_chunk = -1;
    movie->decoded_local_frame = -1;
    chunk->chunk_storage_size = 0;
    chunk->chunk_index = -1;
    chunk->state = PREFETCH_IDLE;
    chunk->read_offset = 0;
    /* Retained buffers count against the same prefetch cap in every state. */
    if (!ensure_prefetch_budget(movie, -1, 0)) {
        clear_prefetched_chunk(chunk);
    }
    return true;
}

static bool h264_codec_global_init(void)
{
    if (!NDVIDEO_WITH_H264) return false;
    /* CX gives its small identity pool to the codec actually opened. */
    if (sram_is_enabled() && !sram_uses_native_clone()) h264bsdInitSramTables();
    return init_h264_color_tables();
}

static bool h264_codec_open(Movie *movie)
{
    if (!NDVIDEO_WITH_H264) return false;
    if (!movie) {
        return false;
    }

    init_sram_movie_chunk_buffer();
    movie->h264.decoder = h264bsdAlloc();
    if (!movie->h264.decoder) {
        decoder_allocation_error("H.264");
        return false;
    }
    /* h264bsdInit initializes the full storage; avoid clearing it twice. */
    return reset_h264_decoder(movie);
}

static void h264_codec_destroy(Movie *movie)
{
    if (!NDVIDEO_WITH_H264) return;
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
    if (!NDVIDEO_WITH_MPEG4) return false;
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
    if (!NDVIDEO_WITH_MPEG4) return;
    if (!movie || !movie->mpeg4.decoder) {
        return;
    }
    mpeg4_xvid_destroy(movie->mpeg4.decoder);
    memset(&movie->mpeg4, 0, sizeof(movie->mpeg4));
}

static bool mpeg4_codec_supports_incremental_seek_preview(const Movie *movie)
{
    (void) movie;
    return true;
}

static bool hevc_codec_open(Movie *movie)
{
    movie->hevc.decoder = player_hevc_decoder_create();
    if (!movie->hevc.decoder) { decoder_allocation_error("HEVC"); return false; }
    return true;
}

static void hevc_codec_destroy(Movie *movie)
{
    if (!movie) return;
    player_hevc_decoder_destroy(movie->hevc.decoder);
    memset(&movie->hevc, 0, sizeof(movie->hevc));
}

av1_decoder_t *player_av1_decoder_create(void)
{
    if (!NDVIDEO_WITH_AV1) return NULL;
    init_sram_movie_chunk_buffer();
    size_t working_bytes = av1_working_memory_size();
    if (!working_bytes) return NULL;
    if (g_sram_movie_chunk_buffer && !sram_chunk_owner && !hevc_sram_users &&
        working_bytes <= g_sram_movie_chunk_buffer_size) {
        av1_decoder_t *decoder = av1_create_with_memory(
            g_sram_movie_chunk_buffer, g_sram_movie_chunk_buffer_size);
        if (decoder) { ++av1_sram_users; return decoder; }
    }
    return av1_create();
}

void player_av1_decoder_destroy(av1_decoder_t *decoder)
{
    if (!NDVIDEO_WITH_AV1 || !decoder) return;
    bool borrowed_sram = av1_external_memory(decoder) == g_sram_movie_chunk_buffer &&
                         g_sram_movie_chunk_buffer != NULL;
    av1_destroy(decoder);
    if (borrowed_sram && av1_sram_users) --av1_sram_users;
}

static bool av1_codec_open(Movie *movie)
{
    movie->av1.decoder = player_av1_decoder_create();
    if (!movie->av1.decoder) { decoder_allocation_error("AV1"); return false; }
    return true;
}

static void av1_codec_destroy(Movie *movie)
{
    if (!movie) return;
    player_av1_decoder_destroy(movie->av1.decoder);
    memset(&movie->av1, 0, sizeof(movie->av1));
}

static const MovieCodecOps g_av1_codec_ops = {
    MOVIE_CODEC_AV1, "av1", init_h264_color_tables, av1_codec_open,
    av1_codec_destroy, reset_av1_decoder, decode_av1_frame,
    h264_codec_supports_incremental_seek_preview
};

static const MovieCodecOps g_hevc_codec_ops = {
    MOVIE_CODEC_HEVC, "hevc", init_h264_color_tables, hevc_codec_open,
    hevc_codec_destroy, reset_hevc_decoder, decode_hevc_frame,
    h264_codec_supports_incremental_seek_preview
};

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
        return NDVIDEO_WITH_H264 ? &g_h264_codec_ops : NULL;
    case MOVIE_CODEC_MPEG4:
        return NDVIDEO_WITH_MPEG4 ? &g_mpeg4_codec_ops : NULL;
    case MOVIE_CODEC_HEVC:
        return NDVIDEO_WITH_HEVC ? &g_hevc_codec_ops : NULL;
    case MOVIE_CODEC_AV1:
        return NDVIDEO_WITH_AV1 ? &g_av1_codec_ops : NULL;
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
    /* Queue slots own the other RGB allocations after presentation swaps.
     * Release them while the displayed framebuffer and decoder still exist. */
    video_lookahead_destroy(movie);
    movie_async_stop(movie);
    if (movie->file) {
        fclose(movie->file);
    }
    if (movie->frame_surface) {
        SDL_FreeSurface(movie->frame_surface);
    }
    if (movie->subtitle_storage) {
        free(movie->subtitle_storage);
    } else {
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
    }
    free(movie->chunk_index);
    player_free_aligned(movie->framebuffer, movie->framebuffer_allocation);
    release_movie_chunk_storage(movie);
    free(movie->frame_offsets_allocation);
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
    /* The picker transition retains only the displayed image. Partial decode
     * work cannot outlive the chunk storage released below. */
    video_lookahead_destroy(movie);
    movie_async_stop(movie);
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
    memset(fonts, 0, sizeof(*fonts));
    fonts->white = nSDL_LoadFont(NSDL_FONT_TINYTYPE, 255, 255, 255);
    fonts->outline = nSDL_LoadFont(NSDL_FONT_TINYTYPE, 0, 0, 0);
    fonts->subtitles = calloc(1, sizeof(*fonts->subtitles));
    if (!fonts->white || !fonts->outline || !fonts->subtitles) {
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
    if (fonts->subtitles) {
        for (font_id = 0; font_id < NSP_NUMFONTS; ++font_id) {
            if (fonts->subtitles->white[font_id]) nSDL_FreeFont(fonts->subtitles->white[font_id]);
            if (fonts->subtitles->outline[font_id]) nSDL_FreeFont(fonts->subtitles->outline[font_id]);
        }
        free(fonts->subtitles);
    }
    memset(fonts, 0, sizeof(*fonts));
}

bool movie_uses_h264(const Movie *movie)
{
    return NDVIDEO_WITH_H264 && movie && movie->codec == MOVIE_CODEC_H264;
}

bool init_mpeg4_decoder_global(void)
{
    if (!NDVIDEO_WITH_MPEG4) return false;
    static bool attempted = false;
    static bool initialized = false;
    static void *sram_pool = NULL;
    static unsigned int sram_pool_size = 0;

    if (initialized) {
        return true;
    }
    if (!attempted && sram_is_enabled()) {
        size_t bytes=MPEG4_XVID_SRAM_POOL_BYTES;
        if (!sram_uses_native_clone()) {
            /* Reserve the compact H.264/color tables before handing Xvid the
             * remaining arena, so switching codecs is independent of order. */
            if (NDVIDEO_WITH_H264) {
#if NDVIDEO_CODEC_MODULES
                h264_module_reserve_sram();
#else
                h264bsdInitSramTables();
#endif
            }
            if (NDVIDEO_WITH_H264 || NDVIDEO_WITH_HEVC || NDVIDEO_WITH_AV1) init_h264_color_tables();
            size_t used=sram_bytes_used(), capacity=sram_bytes_capacity();
            /* Small color/VLC tables go first in Xvid's partial arena. */
            used=(used+31U)&~(size_t)31U;
            bytes=used<capacity ? (capacity-used)&~(size_t)31U : 0U;
        }
        sram_pool = sram_alloc(bytes, 32U);
        if (sram_pool) sram_pool_size=(unsigned int)bytes;
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
        if (y < 0) {
            y = 0;
        }
        tables->y_base[index] = (298 * y) + 128;
        int chroma = index - 128;
        tables->u_to_blue[index] = 516 * chroma + H264_RGB565_RED_BLUE_BIAS;
        tables->u_to_green[index] = -100 * chroma;
        tables->v_to_red[index] = 409 * chroma + H264_RGB565_RED_BLUE_BIAS;
        tables->v_to_green[index] = -208 * chroma + H264_RGB565_GREEN_BIAS;
    }

    /* Across every 8-bit Y/U/V combination, biased red/blue indices lie in
     * [31,130] and green in [217,364]. One 384-byte table serves both clamps. */
    for (index = 0; index < H264_RGB565_CLIP_TABLE_SIZE; ++index) {
        int limit = index < 192 ? 31 : 63;
        int value = index - (index < 192 ? 64 : 256);
        if(value < 0) value=0;
        if(value > limit) value=limit;
        tables->clip[index]=(uint8_t)value;
    }

    tables->initialized = true;
    return true;
}

void init_sram_movie_chunk_buffer(void)
{
    if (g_sram_movie_chunk_buffer || !sram_is_enabled() || !sram_uses_native_clone()) {
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

hevc_decoder_t *player_hevc_decoder_create(void)
{
    if (!NDVIDEO_WITH_HEVC) return NULL;
    init_sram_movie_chunk_buffer();
    size_t working_bytes = hevc_working_memory_size();
    if (!working_bytes) return NULL;
    if (g_sram_movie_chunk_buffer && !sram_chunk_owner && !av1_sram_users &&
        working_bytes <= g_sram_movie_chunk_buffer_size) {
        hevc_decoder_t *decoder = hevc_create_with_memory(
            g_sram_movie_chunk_buffer, g_sram_movie_chunk_buffer_size);
        if (decoder) {
            ++hevc_sram_users;
            return decoder;
        }
    }
    return hevc_create();
}

void player_hevc_decoder_destroy(hevc_decoder_t *decoder)
{
    if (!NDVIDEO_WITH_HEVC) return;
    if (!decoder) return;
    const void *memory = hevc_external_memory(decoder);
    bool borrowed_sram = memory && memory == g_sram_movie_chunk_buffer;
    hevc_destroy(decoder);
    if (borrowed_sram && hevc_sram_users) --hevc_sram_users;
}


uint32_t h264_prefetch_io_min_spare_ms(const Movie *movie)
{
    if (movie && movie_uses_decode_ahead(movie)) {
        if (movie->foreground_decode_peak_ms >= H264_FOREGROUND_DECODE_HARD_MS) {
            return 10U;
        }
        if (movie->foreground_decode_avg_ms >= H264_FOREGROUND_DECODE_SOFT_MS) {
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
        (unsigned) movie->foreground_decode_avg_ms,
        (unsigned) movie->foreground_decode_peak_ms,
        (unsigned long) movie->diag_foreground_direct_decode_count,
        (unsigned long) movie->diag_h264_replay_count,
        (unsigned long) total_prefetched_chunk_bytes(movie)
    );
    debug_tracef("render shown=%lu unchanged=%lu total_ms=%lu peak_ms=%lu",
        (unsigned long) movie->diag_render_count,
        (unsigned long) movie->diag_render_skipped_count,
        (unsigned long) movie->diag_render_total_ms,
        (unsigned long) movie->diag_render_max_ms);
}

static bool debug_dump_report(const char *path, const Movie *movie, const char *reason, bool failure)
{
    FILE *log_file;
    char *log_buffer;
    size_t index;
    bool clip_in_sram = false;
    bool qpc_in_sram = false;
    bool deblocking_in_sram = false;

    if (!path || (!failure && !debug_is_runtime_logging_enabled() && !playback_capture_available(movie))) {
        return false;
    }

    /* Error exits can export before the playback loop unwinds. Freeze and
     * finalize any unfinished sample before doing filesystem work. */
    playback_capture_stop(movie);
    log_file = fopen(path, "wb");
    if (!log_file) {
        return false;
    }
    log_buffer = (char *) malloc(16384U);
    if (log_buffer) setvbuf(log_file, log_buffer, _IOFBF, 16384U);

    fputs("ND Video Player diagnostic log\n", log_file);
    fprintf(log_file, "build=%s %s hwtype=%u hwsubtype=%u ndless_rev=%u os_entry=%08lx\n",
        __DATE__, __TIME__, nl_hwtype(), nl_hwsubtype(), nl_ndless_rev(),
        (unsigned long)*(volatile uint32_t *)0x10000020U);
#if NDVIDEO_CODEC_MODULES
    fprintf(log_file, "codec_modules image_allocated_bytes=%lu h264_refs=%u mpeg4_refs=%u hevc_refs=%u av1_refs=%u\n",
        (unsigned long)codec_modules_loaded_bytes(),
        codec_module_references(CODEC_MODULE_H264), codec_module_references(CODEC_MODULE_MPEG4),
        codec_module_references(CODEC_MODULE_HEVC), codec_module_references(CODEC_MODULE_AV1));
#endif
    storage_read_stream_debug(log_file);
    performance_clock_debug(log_file);
    movie_async_debug(log_file,movie);
    crash_recorder_debug(log_file);
    player_standby_debug(log_file);
    fprintf(log_file, "reason=%s\n", reason ? reason : "unknown");
    fprintf(log_file, "last_error=%s\n", debug_last_error());
    fprintf(log_file, "verbose_logging=%u\n", debug_is_runtime_logging_enabled() ? 1U : 0U);
    fprintf(log_file, "metrics_collection=%u\n", debug_should_collect_metrics() ? 1U : 0U);
    fprintf(log_file, "debug_text_storage bytes=%lu capacity=%u retained=%lu line_bytes=%u\n",
        (unsigned long)(g_debug_ring ? DEBUG_RING_SIZE * sizeof(*g_debug_ring) : 0),
        DEBUG_RING_SIZE, (unsigned long)g_debug_ring_count, DEBUG_LINE_LEN);
    playback_capture_export(log_file, movie);
    if (NDVIDEO_WITH_H264) h264bsdGetSramStatus(&clip_in_sram, &qpc_in_sram, &deblocking_in_sram);
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
        fprintf(log_file, "async_storage used=%u reads=%lu bytes=%lu foreground_waits=%lu cancels=%lu failures=%lu native_status=%ld max_read_us=%llu\n",
            movie->diag_async_used?1U:0U, (unsigned long)movie->diag_async_reads,
            (unsigned long)movie->diag_async_bytes, (unsigned long)movie->diag_async_waits,
            (unsigned long)movie->diag_async_cancels, (unsigned long)movie->diag_async_failures,
            (long)movie->diag_async_native_error,
            (unsigned long long)capture_ticks_to_us(movie->diag_async_max_ticks, TIMER_TICKS_PER_SEC));
        fputs("memory_accounting=tracked_movie_buffers_only; mem_free is unused accounting budget, not measured OS free RAM; decoder/fonts/debug/OS allocations are excluded\n", log_file);
        fputs("movie_lifetime_metrics (not reset when D starts; memory is tracked player buffers, not OS heap usage):\n", log_file);
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
            (unsigned) movie->foreground_decode_avg_ms,
            (unsigned) movie->foreground_decode_peak_ms,
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

    bool saved = ferror(log_file) == 0;
    if (fclose(log_file) != 0) saved = false;
    free(log_buffer);
    return saved;
}

bool debug_dump_session(const char *path, const Movie *movie, const char *reason)
{
    return debug_dump_report(path, movie, reason, false);
}

bool debug_dump_failure(const char *path, const Movie *movie, const char *reason)
{
    return debug_dump_report(path, movie, reason, true);
}

void debug_log_sram_status(void)
{
    bool clip_in_sram = false;
    bool qpc_in_sram = false;
    bool deblocking_in_sram = false;

    if (NDVIDEO_WITH_H264) h264bsdGetSramStatus(&clip_in_sram, &qpc_in_sram, &deblocking_in_sram);
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
