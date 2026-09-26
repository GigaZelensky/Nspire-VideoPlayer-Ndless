#include "player_internal.h"
#include "prefetch_io_policy.h"

const PrefetchedChunk *find_prefetched_chunk_const(const Movie *movie, int chunk_index)
{
    int index;

    if (!movie) {
        return NULL;
    }

    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        if (movie->prefetched[index].chunk_index == chunk_index) {
            return &movie->prefetched[index];
        }
    }
    return NULL;
}

static bool chunk_storage_layout(
    const ChunkIndexEntry *entry,
    const uint8_t *chunk_storage,
    size_t chunk_storage_size,
    size_t *out_header_size,
    uint32_t *out_payload_size
)
{
    size_t local_offset;
    size_t remaining;
    size_t header_size;
    uint32_t payload_size;

    if (!entry || !chunk_storage || entry->frame_count == 0) {
        return false;
    }
    local_offset = (size_t) entry->frame_table_offset;
    if (local_offset > chunk_storage_size) {
        return false;
    }
    remaining = chunk_storage_size - local_offset;
    /* Bound the multiplication by the remaining buffer before computing it. */
    if (remaining < 4U || entry->frame_count > (remaining - 4U) / sizeof(uint32_t)) {
        return false;
    }
    header_size = 4U + (size_t) entry->frame_count * sizeof(uint32_t);
    payload_size = read_le32(chunk_storage + local_offset);
    if (payload_size == 0 || payload_size > remaining - header_size) {
        return false;
    }
    *out_header_size = header_size;
    *out_payload_size = payload_size;
    return true;
}

uint32_t h264_frame_size_from_offsets(const uint32_t *frame_offsets, uint32_t frame_count, size_t chunk_size, uint32_t local_index)
{
    size_t start;
    size_t end;

    if (!frame_offsets || local_index >= frame_count) {
        return 0;
    }

    start = frame_offsets[local_index];
    end = (local_index + 1U < frame_count) ? frame_offsets[local_index + 1U] : chunk_size;
    if (end <= start || end > chunk_size) {
        return 0;
    }
    return (uint32_t) (end - start);
}

uint32_t h264_frame_size_from_chunk_storage(const Movie *movie, const ChunkIndexEntry *entry, const uint8_t *chunk_storage, size_t chunk_storage_size, uint32_t local_index)
{
    size_t local_offset;
    size_t header_size;
    uint32_t payload_size;
    uint32_t start;
    uint32_t end;

    if (!movie || !entry || !chunk_storage || local_index >= entry->frame_count) {
        return 0;
    }

    local_offset = (size_t) entry->frame_table_offset;
    if (!chunk_storage_layout(entry, chunk_storage, chunk_storage_size,
                              &header_size, &payload_size)) {
        return 0;
    }

    start = read_le32(chunk_storage + local_offset + 4U + ((size_t) local_index * sizeof(uint32_t)));
    end = (local_index + 1U < entry->frame_count)
        ? read_le32(chunk_storage + local_offset + 4U + ((size_t) (local_index + 1U) * sizeof(uint32_t)))
        : payload_size;
    if (end <= start || end > payload_size) {
        return 0;
    }
    return end - start;
}

uint32_t estimate_h264_chunk_average_frame_bytes(const Movie *movie, const ChunkIndexEntry *entry)
{
    size_t payload_bytes;

    if (!movie || !entry || entry->frame_count == 0) {
        return 0;
    }

    payload_bytes = entry->unpacked_size;
    if (entry->frame_table_offset > payload_bytes) {
        return 0;
    }
    payload_bytes -= entry->frame_table_offset;
    if (payload_bytes < 4U || entry->frame_count > (payload_bytes - 4U) / sizeof(uint32_t)) {
        return 0;
    }
    payload_bytes -= 4U + (size_t) entry->frame_count * sizeof(uint32_t);

    if (payload_bytes == 0) {
        return 0;
    }

    return (uint32_t) (1U + (payload_bytes - 1U) / entry->frame_count);
}

uint32_t estimate_h264_frame_bytes(const Movie *movie, uint32_t frame_index)
{
    int chunk_index;
    const ChunkIndexEntry *entry;
    uint32_t local_index;
    const PrefetchedChunk *prefetched;

    if (!movie || !movie_uses_h264(movie) || frame_index >= movie->header.frame_count) {
        return 0;
    }

    chunk_index = movie_chunk_for_frame(movie, frame_index);
    if (chunk_index < 0) {
        return 0;
    }

    entry = movie->chunk_index + chunk_index;
    local_index = frame_index - entry->first_frame;

    if (movie->loaded_chunk == chunk_index && movie->frame_offsets && movie->chunk_bytes) {
        return h264_frame_size_from_offsets(movie->frame_offsets, entry->frame_count, movie->chunk_size, local_index);
    }

    prefetched = find_prefetched_chunk_const(movie, chunk_index);
    if (prefetched && prefetched->state == PREFETCH_READY && prefetched->chunk_storage) {
        return h264_frame_size_from_chunk_storage(movie, entry, prefetched->chunk_storage, prefetched->chunk_storage_size, local_index);
    }

    return estimate_h264_chunk_average_frame_bytes(movie, entry);
}

bool reset_h264_storage_decoder(storage_t *decoder, bool *initialized)
{
    if (!decoder || !initialized) {
        debug_failf("h264 decoder reset failed: decoder missing");
        return false;
    }
    if (*initialized) {
        h264bsdShutdown(decoder);
    }
    if (h264bsdInit(decoder, HANTRO_TRUE) != HANTRO_OK) {
        *initialized = false;
        debug_failf("h264 decoder init failed");
        return false;
    }
    *initialized = true;
    return true;
}

bool read_h264_picture_params(
    const Movie *movie,
    storage_t *decoder,
    uint32_t *full_width,
    uint32_t *full_height,
    uint32_t *crop_left,
    uint32_t *crop_top,
    uint32_t *crop_width,
    uint32_t *crop_height
)
{
    u32 cropping_flag = 0;
    u32 out_crop_left = 0;
    u32 out_crop_width = 0;
    u32 out_crop_top = 0;
    u32 out_crop_height = 0;
    u32 out_full_width;
    u32 out_full_height;

    if (!movie || !decoder || !full_width || !full_height ||
        !crop_left || !crop_top || !crop_width || !crop_height) {
        debug_failf("h264 picture params failed: decoder missing");
        return false;
    }

    out_full_width = h264bsdPicWidth(decoder) * 16U;
    out_full_height = h264bsdPicHeight(decoder) * 16U;
    h264bsdCroppingParams(decoder, &cropping_flag, &out_crop_left, &out_crop_width, &out_crop_top, &out_crop_height);
    if (!cropping_flag) {
        out_crop_left = 0;
        out_crop_top = 0;
        out_crop_width = out_full_width;
        out_crop_height = out_full_height;
    }
    if (out_crop_width != movie->header.video_width || out_crop_height != movie->header.video_height) {
        debug_failf(
            "h264 size mismatch crop=%lux%lu header=%ux%u",
            (unsigned long) out_crop_width,
            (unsigned long) out_crop_height,
            (unsigned) movie->header.video_width,
            (unsigned) movie->header.video_height
        );
        return false;
    }

    *full_width = out_full_width;
    *full_height = out_full_height;
    *crop_left = out_crop_left;
    *crop_top = out_crop_top;
    *crop_width = out_crop_width;
    *crop_height = out_crop_height;
    return true;
}

void store_h264_picture_params(
    Movie *movie,
    uint32_t full_width,
    uint32_t full_height,
    uint32_t crop_left,
    uint32_t crop_top,
    uint32_t crop_width,
    uint32_t crop_height
)
{
    if (!movie) {
        return;
    }

    movie->h264.full_width = full_width;
    movie->h264.full_height = full_height;
    movie->h264.crop_left = crop_left;
    movie->h264.crop_top = crop_top;
    movie->h264.crop_width = crop_width;
    movie->h264.crop_height = crop_height;
    movie->h264.headers_ready = true;
}

bool sync_h264_picture_params(Movie *movie, storage_t *decoder, bool force_commit)
{
    uint32_t full_width = 0;
    uint32_t full_height = 0;
    uint32_t crop_left = 0;
    uint32_t crop_top = 0;
    uint32_t crop_width = 0;
    uint32_t crop_height = 0;

    if (!movie || !decoder) {
        debug_failf("h264 picture params failed: decoder missing");
        return false;
    }
    if (!read_h264_picture_params(
            movie,
            decoder,
            &full_width,
            &full_height,
            &crop_left,
            &crop_top,
            &crop_width,
            &crop_height)) {
        return false;
    }
    if (force_commit || !movie->h264.headers_ready) {
        store_h264_picture_params(movie, full_width, full_height, crop_left, crop_top, crop_width, crop_height);
    }
    return true;
}

bool reset_h264_decoder(Movie *movie)
{
    bool capture_reset;
    bool reset_ok;
    uint64_t reset_started_ticks;

    if (!movie || !movie->h264.decoder) {
        debug_failf("h264 decoder reset failed: decoder missing");
        return false;
    }
    capture_reset = debug_is_runtime_logging_enabled();
    reset_started_ticks = capture_reset ? monotonic_clock_now_ticks() : 0;
    reset_ok = reset_h264_storage_decoder(movie->h264.decoder, &movie->h264.decoder_initialized);
    if (capture_reset) {
        playback_capture_io(movie, 4, movie->loaded_chunk, 0, reset_started_ticks, monotonic_clock_now_ticks());
    }
    if (!reset_ok) {
        return false;
    }
    movie->h264.headers_ready = false;
    movie->h264.decoder_failed = false;
    movie->h264.full_width = 0;
    movie->h264.full_height = 0;
    movie->h264.crop_left = 0;
    movie->h264.crop_top = 0;
    movie->h264.crop_width = 0;
    movie->h264.crop_height = 0;
    movie->h264.chunk_dirty = false;
    return true;
}

bool reset_mpeg4_decoder(Movie *movie)
{
    bool capture_reset;
    bool reset_ok;
    uint64_t reset_started_ticks;

    if (!movie) {
        debug_failf("mpeg4 decoder reset failed: movie missing");
        return false;
    }
    if (!init_mpeg4_decoder_global()) {
        return false;
    }
    capture_reset = debug_is_runtime_logging_enabled();
    reset_started_ticks = capture_reset ? monotonic_clock_now_ticks() : 0;
    reset_ok = mpeg4_xvid_reset(
            &movie->mpeg4.decoder,
            (int) movie->header.video_width,
            (int) movie->header.video_height);
    if (capture_reset) {
        playback_capture_io(movie, 4, movie->loaded_chunk, 0, reset_started_ticks, monotonic_clock_now_ticks());
    }
    if (!reset_ok) {
        debug_failf("mpeg4 decoder reset failed: %s", mpeg4_xvid_last_error());
        return false;
    }
    movie->mpeg4.chunk_dirty = false;
    movie->mpeg4.discontinuity = false;
    return true;
}

static void clear_movie_codec_chunk_progress(Movie *movie)
{
    movie->decoded_local_frame = -1;
    movie->h264.chunk_dirty = false;
    movie->mpeg4.chunk_dirty = false;
}

static bool reset_movie_codec_for_chunk(Movie *movie)
{
    if (!movie || !movie->codec_ops || !movie->codec_ops->reset) {
        debug_failf("codec reset failed: no codec ops");
        return false;
    }
    if (!movie->codec_ops->reset(movie)) {
        return false;
    }
    clear_movie_codec_chunk_progress(movie);
    return true;
}

static bool prepare_movie_codec_for_loaded_chunk(Movie *movie, int previous_chunk, int chunk_index)
{
    if (!movie) {
        debug_failf("codec chunk prepare failed: movie missing");
        return false;
    }
    if (movie->codec == MOVIE_CODEC_MPEG4 &&
        movie->mpeg4.decoder &&
        previous_chunk >= 0 &&
        chunk_index == previous_chunk + 1) {
        clear_movie_codec_chunk_progress(movie);
        movie->mpeg4.discontinuity = true;
        if (debug_is_runtime_logging_enabled()) {
            playback_capture_io(movie, 5, chunk_index, 0, 0, 0);
        }
        return true;
    }
    if (movie->codec == MOVIE_CODEC_H264 && movie->h264.decoder &&
        movie->h264.decoder_initialized && !movie->h264.decoder_failed &&
        previous_chunk >= 0 && chunk_index == previous_chunk + 1) {
        /* Each NVP chunk starts an independent IDR sequence. The decoder
         * already handles its SPS/PPS and DPB reset, so keep its allocations
         * across normal sequential playback. Rewinds/errors still reset. */
        clear_movie_codec_chunk_progress(movie);
        if (debug_is_runtime_logging_enabled()) {
            playback_capture_io(movie, 5, chunk_index, 0, 0, 0);
        }
        return true;
    }
    return reset_movie_codec_for_chunk(movie);
}

static inline bool h264_flat_luma_blocks(const uint8_t *plane, size_t stride, size_t width, size_t height)
{
    /* Only take the per-block flat-color branch when 3/8 of 128 spaced probes
     * are constant. Dense scenes use the branch-free loop; cartoon backgrounds
     * can reuse one exact RGB value for all four pixels sharing their chroma. */
    unsigned flat=0;
    if(width<64 || height<64) return false;
    for(unsigned row=0;row<16;++row) {
        size_t y=(((height-2U)*row)>>4)&~(size_t)1;
        for(unsigned col=0;col<8;++col) {
            size_t x=(((width-2U)*col)>>3)&~(size_t)1;
            const uint8_t *p=plane+y*stride+x;
            flat += p[0]==p[1] && p[0]==p[stride] && p[0]==p[stride+1];
        }
    }
    return flat>=48;
}

static inline uint32_t h264_rgb565_pair(const uint8_t *row, const int32_t *y_base, const uint8_t *clip,
    int32_t red, int32_t green, int32_t blue)
{
    /* floor(clamp8(fixed>>8)/8) == clamp5(fixed>>11), likewise green's
     * 6-bit clamp at >>10. Biased chroma selects disjoint byte-table ranges.
     * The ARM sequence packs both pixels without extra masks or compiler
     * spills for intermediate channel values; the C fallback is identical. */
#if defined(__arm__) && !defined(__thumb__)
    uint32_t result, luma, value;
    __asm__ volatile(
        "ldrb %[value], [%[row]]\n\t"
        "ldr %[luma], [%[ytab], %[value], lsl #2]\n\t"
        "add %[value], %[luma], %[blue]\n\t"
        "ldrb %[result], [%[clip], %[value], asr #11]\n\t"
        "add %[value], %[luma], %[green]\n\t"
        "ldrb %[value], [%[clip], %[value], asr #10]\n\t"
        "orr %[result], %[result], %[value], lsl #5\n\t"
        "add %[value], %[luma], %[red]\n\t"
        "ldrb %[value], [%[clip], %[value], asr #11]\n\t"
        "orr %[result], %[result], %[value], lsl #11\n\t"
        "ldrb %[value], [%[row], #1]\n\t"
        "ldr %[luma], [%[ytab], %[value], lsl #2]\n\t"
        "add %[value], %[luma], %[blue]\n\t"
        "ldrb %[value], [%[clip], %[value], asr #11]\n\t"
        "orr %[result], %[result], %[value], lsl #16\n\t"
        "add %[value], %[luma], %[green]\n\t"
        "ldrb %[value], [%[clip], %[value], asr #10]\n\t"
        "orr %[result], %[result], %[value], lsl #21\n\t"
        "add %[value], %[luma], %[red]\n\t"
        "ldrb %[value], [%[clip], %[value], asr #11]\n\t"
        "orr %[result], %[result], %[value], lsl #27\n\t"
        : [result] "=&r"(result), [luma] "=&r"(luma), [value] "=&r"(value)
        : [row] "r"(row), [ytab] "r"(y_base), [clip] "r"(clip), [red] "r"(red), [green] "r"(green), [blue] "r"(blue)
        : "memory");
    return result;
#else
    int32_t l0=y_base[row[0]], l1=y_base[row[1]];
    uint32_t p0=((uint32_t)clip[(l0+red)>>11]<<11)|((uint32_t)clip[(l0+green)>>10]<<5)|clip[(l0+blue)>>11];
    uint32_t p1=((uint32_t)clip[(l1+red)>>11]<<11)|((uint32_t)clip[(l1+green)>>10]<<5)|clip[(l1+blue)>>11];
    return p0|(p1<<16);
#endif
}

static inline bool blit_h264_planes_to_rgb565_rows(
    const Movie *movie,
    const uint8_t *restrict y_plane,
    const uint8_t *restrict u_plane,
    const uint8_t *restrict v_plane,
    size_t luma_stride,
    size_t chroma_stride,
    uint16_t *restrict dst_pixels,
    size_t dst_pitch_pixels,
    size_t crop_width,
    size_t crop_height,
    bool try_flat
)
{
    const int32_t *restrict y_base = g_h264_color_tables->y_base;
    const uint8_t *restrict clip = g_h264_color_tables->clip;

    size_t y;
    if (!movie || !y_plane || !u_plane || !v_plane || !dst_pixels || !crop_width || !crop_height || ((crop_width | crop_height) & 1U)) return false;
    for (y=0;y<crop_height;y+=2U) {
        const uint8_t *y0=y_plane+y*luma_stride, *y1=y0+luma_stride;
        const uint8_t *u=u_plane+(y/2U)*chroma_stride, *v=v_plane+(y/2U)*chroma_stride;
        uint16_t *d0=dst_pixels+y*dst_pitch_pixels, *d1=d0+dst_pitch_pixels;
        const bool packed=(((uintptr_t)d0|(uintptr_t)d1)&3U)==0;
        size_t x;
        if(packed) {
            uint32_t *w0=(uint32_t *)(void *)d0,*w1=(uint32_t *)(void *)d1;
#pragma GCC unroll 1
            for(x=0;x<crop_width;x+=2U) {
                int32_t r,g,b;
                h264_compute_chroma_terms(u[x/2U],v[x/2U],&r,&g,&b);
                uint8_t sample=y0[x];
                if(try_flat && sample==y0[x+1] && sample==y1[x] && sample==y1[x+1]) {
                    int32_t luma=y_base[sample];
                    uint32_t pixel=((uint32_t)clip[(luma+r)>>11]<<11)|((uint32_t)clip[(luma+g)>>10]<<5)|clip[(luma+b)>>11];
                    pixel|=pixel<<16;
                    w0[x/2U]=pixel;w1[x/2U]=pixel;
                    continue;
                }
                w0[x/2U]=h264_rgb565_pair(y0+x,y_base,clip,r,g,b);
                w1[x/2U]=h264_rgb565_pair(y1+x,y_base,clip,r,g,b);
            }
        } else {
#pragma GCC unroll 1
            for(x=0;x<crop_width;x+=2U) {
                int32_t r,g,b,l0,l1;
                h264_compute_chroma_terms(u[x/2U],v[x/2U],&r,&g,&b);
                l0=y_base[y0[x]];l1=y_base[y0[x+1]];
                d0[x]=(uint16_t)(((uint32_t)clip[(l0+r)>>11] << 11) | ((uint32_t)clip[(l0+g)>>10] << 5) | clip[(l0+b)>>11]);d0[x+1]=(uint16_t)(((uint32_t)clip[(l1+r)>>11] << 11) | ((uint32_t)clip[(l1+g)>>10] << 5) | clip[(l1+b)>>11]);
                l0=y_base[y1[x]];l1=y_base[y1[x+1]];
                d1[x]=(uint16_t)(((uint32_t)clip[(l0+r)>>11] << 11) | ((uint32_t)clip[(l0+g)>>10] << 5) | clip[(l0+b)>>11]);d1[x+1]=(uint16_t)(((uint32_t)clip[(l1+r)>>11] << 11) | ((uint32_t)clip[(l1+g)>>10] << 5) | clip[(l1+b)>>11]);
            }
        }
    }
    return true;
}

bool blit_h264_planes_to_rgb565_target_with_crop(
    const Movie *movie,
    const uint8_t *restrict y_plane,
    const uint8_t *restrict u_plane,
    const uint8_t *restrict v_plane,
    size_t luma_stride,
    size_t chroma_stride,
    uint16_t *restrict dst_pixels,
    size_t dst_pitch_pixels,
    size_t crop_width,
    size_t crop_height
)
{
    if (!movie || !y_plane || !u_plane || !v_plane || !dst_pixels ||
        !crop_width || !crop_height || ((crop_width | crop_height) & 1U)) return false;
    return blit_h264_planes_to_rgb565_rows(movie,y_plane,u_plane,v_plane,
        luma_stride,chroma_stride,dst_pixels,dst_pitch_pixels,crop_width,crop_height,
        h264_flat_luma_blocks(y_plane,luma_stride,crop_width,crop_height));
}

bool blit_h264_planes_to_rgb565_target(
    const Movie *movie,
    const uint8_t *restrict y_plane,
    const uint8_t *restrict u_plane,
    const uint8_t *restrict v_plane,
    size_t luma_stride,
    size_t chroma_stride,
    uint16_t *restrict dst_pixels,
    size_t dst_pitch_pixels
)
{
    if (!movie || !y_plane || !u_plane || !v_plane || !dst_pixels || !movie->h264.headers_ready) {
        return false;
    }

    bool capture_color = playback_capture_active(movie);
    uint64_t capture_started = capture_color ? monotonic_clock_now_ticks() : 0;
    bool converted = blit_h264_planes_to_rgb565_target_with_crop(
        movie,
        y_plane,
        u_plane,
        v_plane,
        luma_stride,
        chroma_stride,
        dst_pixels,
        dst_pitch_pixels,
        movie->h264.crop_width,
        movie->h264.crop_height
    );
    if (capture_color) playback_capture_stage(movie, CAPTURE_COLOR, capture_started, monotonic_clock_now_ticks());
    return converted;
}

bool blit_h264_picture_to_target(
    Movie *movie,
    const uint8_t *picture,
    uint16_t *dst_pixels,
    size_t dst_pitch_pixels
)
{
    const size_t luma_stride = movie->h264.full_width;
    const size_t chroma_stride = luma_stride / 2U;
    const size_t luma_plane_size = luma_stride * movie->h264.full_height;
    const size_t chroma_plane_size = chroma_stride * (movie->h264.full_height / 2U);
    const uint8_t *y_plane;
    const uint8_t *u_plane;
    const uint8_t *v_plane;

    if (!movie || !picture || !movie->h264.headers_ready || !dst_pixels) {
        return false;
    }

    y_plane = picture + ((size_t) movie->h264.crop_top * luma_stride) + movie->h264.crop_left;
    u_plane = picture + luma_plane_size
        + ((size_t) (movie->h264.crop_top / 2U) * chroma_stride)
        + (movie->h264.crop_left / 2U);
    v_plane = picture + luma_plane_size + chroma_plane_size
        + ((size_t) (movie->h264.crop_top / 2U) * chroma_stride)
        + (movie->h264.crop_left / 2U);

    return blit_h264_planes_to_rgb565_target(
        movie,
        y_plane,
        u_plane,
        v_plane,
        luma_stride,
        chroma_stride,
        dst_pixels,
        dst_pitch_pixels
    );
}

bool blit_h264_picture_rows_to_target(
    Movie *movie,
    const uint8_t *picture,
    uint16_t *dst_pixels,
    size_t dst_pitch_pixels,
    size_t first_row,
    size_t row_count,
    bool *flat_mode
)
{
    const uint8_t *y_plane, *u_plane, *v_plane;
    size_t luma_stride, chroma_stride, luma_size, chroma_size;
    bool capture_color;
    uint64_t started;
    bool converted;
    if (!movie || !picture || !dst_pixels || !flat_mode || !movie->h264.headers_ready ||
        !row_count || ((first_row | row_count) & 1U) ||
        first_row > movie->h264.crop_height || row_count > movie->h264.crop_height - first_row ||
        dst_pitch_pixels < movie->h264.crop_width) return false;
    luma_stride = movie->h264.full_width;
    chroma_stride = luma_stride / 2U;
    luma_size = luma_stride * movie->h264.full_height;
    chroma_size = chroma_stride * (movie->h264.full_height / 2U);
    y_plane = picture + (size_t) movie->h264.crop_top * luma_stride + movie->h264.crop_left;
    u_plane = picture + luma_size + (size_t) (movie->h264.crop_top / 2U) * chroma_stride + movie->h264.crop_left / 2U;
    v_plane = picture + luma_size + chroma_size + (size_t) (movie->h264.crop_top / 2U) * chroma_stride + movie->h264.crop_left / 2U;
    capture_color = playback_capture_active(movie);
    started = capture_color ? monotonic_clock_now_ticks() : 0U;
    /* The original heuristic samples the full picture exactly once. Testing
     * each short band would disable its flat-block optimization (<64 rows). */
    if (!first_row) *flat_mode = h264_flat_luma_blocks(y_plane,luma_stride,
        movie->h264.crop_width,movie->h264.crop_height);
    converted = blit_h264_planes_to_rgb565_rows(movie,
        y_plane + first_row * luma_stride,
        u_plane + (first_row / 2U) * chroma_stride,
        v_plane + (first_row / 2U) * chroma_stride,
        luma_stride,chroma_stride,dst_pixels + first_row * dst_pitch_pixels,
        dst_pitch_pixels,movie->h264.crop_width,row_count,*flat_mode);
    if (capture_color) playback_capture_stage(movie,CAPTURE_COLOR,started,monotonic_clock_now_ticks());
    return converted;
}

uint8_t *take_h264_output_picture(storage_t *decoder, const char *context)
{
    const char *label = context ? context : "h264";
    u32 pic_id = 0;
    u32 is_idr_pic = 0;
    u32 num_err_mbs = 0;
    uint8_t *picture;

    if (!decoder) {
        debug_failf("%s decoder missing", label);
        return NULL;
    }

    picture = h264bsdNextOutputPicture(decoder, &pic_id, &is_idr_pic, &num_err_mbs);
    if (!picture) {
        debug_failf("%s picture ready but no output picture", label);
        return NULL;
    }
    return picture;
}

bool pump_h264_access_unit(
    Movie *movie,
    storage_t *decoder,
    uint8_t *frame_data,
    size_t frame_size,
    size_t *inout_consumed,
    unsigned *inout_zero_advance_retries,
    uint32_t macroblock_budget,
    bool force_commit_picture_params,
    const char *context,
    bool *out_picture_ready,
    bool *out_pending,
    uint8_t **out_picture
)
{
    const char *label = context ? context : "h264";

    if (out_picture_ready) {
        *out_picture_ready = false;
    }
    if (out_pending) {
        *out_pending = false;
    }
    if (out_picture) {
        *out_picture = NULL;
    }
    if (!movie || !decoder || !frame_data || frame_size == 0 ||
        !inout_consumed || !inout_zero_advance_retries ||
        !out_picture_ready || !out_pending || !out_picture) {
        debug_failf("%s access-unit decode invalid input size=%lu", label, (unsigned long) frame_size);
        return false;
    }

    h264bsdSetMacroblockBudget(decoder, macroblock_budget);

    while (*inout_consumed < frame_size) {
        u32 read_bytes = 0;
        u32 result = h264bsdDecode(
            decoder,
            frame_data + *inout_consumed,
            (u32) (frame_size - *inout_consumed),
            0,
            &read_bytes
        );

        if (result == H264BSD_HDRS_RDY) {
            if (!sync_h264_picture_params(movie, decoder, force_commit_picture_params)) {
                return false;
            }
        } else if (result == H264BSD_PIC_RDY) {
            if (!movie->h264.headers_ready && !sync_h264_picture_params(movie, decoder, false)) {
                return false;
            }
            *out_picture = take_h264_output_picture(decoder, label);
            if (!(*out_picture)) {
                return false;
            }
            *out_picture_ready = true;
        } else if (result == H264BSD_PENDING) {
            *out_pending = true;
        } else if (result != H264BSD_RDY) {
            debug_failf("%s decode error result=%lu read=%lu", label, (unsigned long) result, (unsigned long) read_bytes);
            return false;
        }

        if (read_bytes == 0U) {
            if (result == H264BSD_HDRS_RDY && *inout_zero_advance_retries < 8U) {
                (*inout_zero_advance_retries)++;
                debug_tracef(
                    "%s hdrs ready retry=%u consumed=%lu size=%lu",
                    label,
                    *inout_zero_advance_retries,
                    (unsigned long) *inout_consumed,
                    (unsigned long) frame_size
                );
                continue;
            }
            if (!(*out_picture_ready) && !(*out_pending)) {
                debug_failf(
                    "%s decode stalled with zero-byte advance result=%lu retries=%u consumed=%lu size=%lu",
                    label,
                    (unsigned long) result,
                    *inout_zero_advance_retries,
                    (unsigned long) *inout_consumed,
                    (unsigned long) frame_size
                );
                return false;
            }
            return true;
        }

        *inout_zero_advance_retries = 0U;
        *inout_consumed += read_bytes;

        if (*out_picture_ready || *out_pending) {
            return true;
        }
        if (macroblock_budget > 0U && decoder->macroblockBudget == 0U) {
            *out_pending = true;
            return true;
        }
    }

    return *out_picture_ready;
}

bool decode_h264_access_unit_to_target(
    Movie *movie,
    uint8_t *frame_data,
    size_t frame_size,
    uint16_t *dst_pixels,
    size_t dst_pitch_pixels
)
{
    size_t consumed = 0;
    bool picture_ready = false;
    bool pending = false;
    unsigned zero_advance_retries = 0;
    uint8_t *picture = NULL;

    if (!movie || !movie->h264.decoder) {
        debug_failf("h264 access-unit decode invalid decoder");
        return false;
    }
    if (!pump_h264_access_unit(
            movie,
            movie->h264.decoder,
            frame_data,
            frame_size,
            &consumed,
            &zero_advance_retries,
            0U,
            true,
            "h264",
            &picture_ready,
            &pending,
            &picture)) {
        movie->h264.decoder_failed = true;
        return false;
    }
    if (pending) {
        movie->h264.decoder_failed = true;
        debug_failf("h264 decode yielded unexpectedly");
        return false;
    }
    if (!picture_ready || !picture) {
        movie->h264.decoder_failed = true;
        return false;
    }
    if (dst_pixels && !blit_h264_picture_to_target(movie, picture, dst_pixels, dst_pitch_pixels)) {
        movie->h264.decoder_failed = true;
        debug_failf("h264 blit failed");
        return false;
    }
    return true;
}

bool decode_h264_access_unit(
    Movie *movie,
    uint8_t *frame_data,
    size_t frame_size,
    bool blit_output
)
{
    return decode_h264_access_unit_to_target(
        movie,
        frame_data,
        frame_size,
        blit_output ? movie->framebuffer : NULL,
        movie ? movie->header.video_width : 0U
    );
}

bool configure_chunk_view_from_storage(
    const Movie *movie,
    int chunk_index,
    const uint8_t *chunk_storage,
    size_t chunk_storage_size,
    uint32_t **out_frame_offsets,
    uint8_t **out_chunk_bytes,
    size_t *out_chunk_size
)
{
    const ChunkIndexEntry *entry;
    size_t local_offset;
    size_t table_bytes;
    size_t header_size;
    uint32_t *frame_offsets = NULL;
    uint8_t *chunk_bytes = NULL;
    size_t chunk_size = 0;
    uint32_t payload_size;
    uint32_t frame;

    if (!movie || !movie->chunk_index || !chunk_storage || !out_frame_offsets || !out_chunk_bytes || !out_chunk_size ||
        chunk_index < 0 || (uint32_t) chunk_index >= movie->header.chunk_count) {
        return false;
    }
    entry = movie->chunk_index + chunk_index;
    local_offset = (size_t) entry->frame_table_offset;
    if (!chunk_storage_layout(entry, chunk_storage, chunk_storage_size,
                              &header_size, &payload_size)) {
        debug_failf(
            "chunk view invalid table chunk=%d table=%lu storage=%lu",
            chunk_index,
            (unsigned long) entry->frame_table_offset,
            (unsigned long) chunk_storage_size
        );
        return false;
    }
    table_bytes = header_size - 4U;
    frame_offsets = (uint32_t *) malloc(table_bytes);
    if (!frame_offsets) {
        debug_failf("chunk view frame offset alloc failed chunk=%d count=%u", chunk_index, (unsigned) entry->frame_count);
        return false;
    }
    /* Decode and validate once at chunk load, before the codec can dereference
     * any frame. malloc avoids clearing the whole table before overwriting it. */
    for (frame = 0; frame < entry->frame_count; ++frame) {
        uint32_t offset = read_le32(chunk_storage + local_offset + 4U + (size_t) frame * 4U);
        if ((frame == 0 && offset != 0) || offset >= payload_size ||
            (frame > 0 && offset <= frame_offsets[frame - 1U])) {
            debug_failf("chunk view invalid frame offset chunk=%d frame=%lu offset=%lu",
                        chunk_index, (unsigned long) frame, (unsigned long) offset);
            free(frame_offsets);
            return false;
        }
        frame_offsets[frame] = offset;
    }
    chunk_bytes = (uint8_t *) chunk_storage + local_offset + header_size;
    chunk_size = payload_size;
    *out_frame_offsets = frame_offsets;
    *out_chunk_bytes = chunk_bytes;
    *out_chunk_size = chunk_size;
    return true;
}

bool configure_chunk_view(Movie *movie, int chunk_index)
{
    const ChunkIndexEntry *entry;
    const uint8_t *table;
    size_t header_size;
    uint32_t payload_size;
    uint32_t previous_offset = 0;
    uint32_t frame;

    if (!movie || !movie->chunk_index || chunk_index < 0 ||
        (uint32_t) chunk_index >= movie->header.chunk_count) {
        return false;
    }
    movie->frame_offsets = NULL;
    movie->chunk_bytes = NULL;
    movie->chunk_size = 0;
    entry = movie->chunk_index + chunk_index;
    if (!chunk_storage_layout(entry, movie->chunk_storage, movie->chunk_storage_size,
                              &header_size, &payload_size)) {
        return false;
    }
    table = movie->chunk_storage + entry->frame_table_offset + 4U;
    for (frame = 0; frame < entry->frame_count; ++frame) {
        uint32_t offset = read_le32(table + (size_t) frame * sizeof(uint32_t));
        if ((frame == 0 && offset != 0) || offset >= payload_size ||
            (frame > 0 && offset <= previous_offset)) {
            debug_failf("chunk view invalid frame offset chunk=%d frame=%lu offset=%lu",
                        chunk_index, (unsigned long) frame, (unsigned long) offset);
            return false;
        }
        previous_offset = offset;
    }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (((uintptr_t) table & (sizeof(uint32_t) - 1U)) == 0) {
        /* NVP's aligned little-endian table is already the required array.
         * It stays valid for exactly the lifetime of the active chunk. */
        movie->frame_offsets = (uint32_t *) table;
    } else
#endif
    {
        /* Valid older/custom files may place the table at an unaligned offset.
         * Keep an owned fallback instead of making ARM unaligned word reads. */
        if (movie->frame_offsets_capacity < entry->frame_count) {
            uint32_t *replacement = (uint32_t *) realloc(movie->frame_offsets_allocation,
                (size_t) entry->frame_count * sizeof(uint32_t));
            if (!replacement) {
                return false;
            }
            movie->frame_offsets_allocation = replacement;
            movie->frame_offsets_capacity = entry->frame_count;
        }
        for (frame = 0; frame < entry->frame_count; ++frame) {
            movie->frame_offsets_allocation[frame] = read_le32(table + (size_t) frame * sizeof(uint32_t));
        }
        movie->frame_offsets = movie->frame_offsets_allocation;
    }
    movie->chunk_bytes = movie->chunk_storage + entry->frame_table_offset + header_size;
    movie->chunk_size = payload_size;
    return true;
}

bool prefetch_deadline_reached(uint32_t deadline_ms)
{
    return (int32_t) (monotonic_clock_now_ms() - deadline_ms) >= 0;
}

void reset_prefetched_chunk(PrefetchedChunk *chunk)
{
    if (!chunk) {
        return;
    }
    chunk->chunk_storage = NULL;
    chunk->chunk_allocation = NULL;
    chunk->chunk_storage_size = 0;
    chunk->chunk_capacity = 0;
    chunk->chunk_index = -1;
    chunk->state = PREFETCH_IDLE;
    chunk->read_offset = 0;
}

void clear_prefetched_chunk(PrefetchedChunk *chunk)
{
    if (!chunk) {
        return;
    }
    if (chunk->chunk_storage) {
        player_free_aligned(
            chunk->chunk_storage,
            chunk->chunk_allocation ? chunk->chunk_allocation : chunk->chunk_storage
        );
    } else {
        free(chunk->chunk_allocation);
    }
    reset_prefetched_chunk(chunk);
}

PrefetchedChunk *find_prefetched_chunk(Movie *movie, int chunk_index)
{
    int index;
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        if (movie->prefetched[index].chunk_index == chunk_index) {
            return &movie->prefetched[index];
        }
    }
    return NULL;
}

PrefetchedChunk *find_prefetch_work_chunk(Movie *movie, int current_chunk, int max_distance)
{
    PrefetchedChunk *best = NULL;
    int index;
    int wanted_min = current_chunk + 1;
    int wanted_max = current_chunk + max_distance;

    if (max_distance < 1) {
        return NULL;
    }

    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        PrefetchedChunk *candidate = &movie->prefetched[index];
        if (candidate->chunk_index < wanted_min || candidate->chunk_index > wanted_max) {
            continue;
        }
        if (candidate->state == PREFETCH_IDLE || candidate->state == PREFETCH_READY) {
            continue;
        }
        if (!best || candidate->chunk_index < best->chunk_index) {
            best = candidate;
        }
    }
    return best;
}

static bool prefetch_read_step_impl(Movie *movie, PrefetchedChunk *chunk, bool respect_deadline,
    uint32_t deadline_ms, bool maintenance)
{
    const ChunkIndexEntry *entry;
    size_t remaining;
    size_t read_size;
    size_t block_size;
    size_t bytes_read;
    long target_pos;
    uint32_t read_start_ms;
    uint32_t read_elapsed_ms;
    bool capture_read;
    uint64_t read_started_ticks;
    uint64_t read_finished_ticks;

    if (!movie || !chunk || chunk->chunk_index < 0 ||
        (uint32_t) chunk->chunk_index >= movie->header.chunk_count) {
        return false;
    }
    entry = movie->chunk_index + chunk->chunk_index;
    if (chunk->read_offset > entry->packed_size) {
        return false;
    }
    remaining = (size_t) entry->packed_size - chunk->read_offset;
    if (remaining == 0) {
        chunk->state = PREFETCH_READY;
        return true;
    }
    if (entry->packed_size != entry->unpacked_size) {
        debug_failf(
            "prefetch chunk=%d unsupported packed=%lu unpacked=%lu",
            chunk->chunk_index,
            (unsigned long) entry->packed_size,
            (unsigned long) entry->unpacked_size
        );
        return false;
    }

    if (movie_async_enabled(movie)) {
        read_size = remaining > MOVIE_ASYNC_BLOCK_BYTES ? MOVIE_ASYNC_BLOCK_BYTES : remaining;
        int result = movie_async_read(movie, (uint64_t)entry->offset + chunk->read_offset,
            chunk->chunk_storage + chunk->read_offset, read_size, chunk->chunk_index, !respect_deadline);
        if (result == MOVIE_ASYNC_PENDING) return true;
        if (result == MOVIE_ASYNC_READY) {
            chunk->read_offset += read_size;
            if (chunk->read_offset == entry->packed_size) chunk->state = PREFETCH_READY;
            return true;
        }
        /* Native failure already joined its worker: legacy I/O is safe now. */
    }
    if (respect_deadline) {
        int32_t time_left_ms = (int32_t) (deadline_ms - monotonic_clock_now_ms());
        block_size = maintenance
            ? prefetch_io_maintenance_read_size(movie->prefetch_read_bytes_per_ms, remaining, time_left_ms)
            : prefetch_io_read_size(movie->prefetch_read_bytes_per_ms, remaining, time_left_ms);
        if (block_size == 0) {
            /* Leave partial data intact when the remaining budget cannot fit
             * even one sector. The work loop will yield without spinning. */
            return true;
        }
    } else {
        block_size = PREFETCH_FILE_BLOCK_SIZE;
    }
    read_size = remaining > block_size ? block_size : remaining;
    target_pos = (long) (entry->offset + chunk->read_offset);
    if (movie->current_file_pos != target_pos) {
        if (fseek(movie->file, target_pos, SEEK_SET) != 0) {
            movie->current_file_pos = -1;
            return false;
        }
        movie->current_file_pos = target_pos;
    }
    if (respect_deadline) {
        /* Seeking can itself consume time. Recheck before entering fread. */
        int32_t time_left_ms = (int32_t) (deadline_ms - monotonic_clock_now_ms());
        read_size = maintenance
            ? prefetch_io_maintenance_read_size(movie->prefetch_read_bytes_per_ms, remaining, time_left_ms)
            : prefetch_io_read_size(movie->prefetch_read_bytes_per_ms, remaining, time_left_ms);
        if (read_size == 0) {
            return true;
        }
    }
    read_start_ms = monotonic_clock_now_ms();
    capture_read = debug_is_runtime_logging_enabled();
    read_started_ticks = capture_read ? monotonic_clock_now_ticks() : 0;
    bytes_read = fread(chunk->chunk_storage + chunk->read_offset, 1, read_size, movie->file);
    read_finished_ticks = capture_read ? monotonic_clock_now_ticks() : 0;
    read_elapsed_ms = monotonic_clock_now_ms() - read_start_ms;
    if (capture_read) {
        playback_capture_io(movie, 2, chunk->chunk_index, (uint32_t) bytes_read, read_started_ticks, read_finished_ticks);
    }
    movie->last_read_time_ms = read_elapsed_ms;
    movie->last_read_bytes = (uint32_t) bytes_read;
    movie->prefetch_read_bytes_per_ms = prefetch_io_update_rate(
        movie->prefetch_read_bytes_per_ms, (uint32_t) bytes_read, read_elapsed_ms);
    if (bytes_read != read_size) {
        movie->current_file_pos = -1;
        return false;
    }
    movie->current_file_pos += (long) read_size;
    if (debug_should_collect_metrics()) {
        movie->diag_prefetch_read_ops++;
        movie->diag_prefetch_read_bytes += (uint32_t) read_size;
    }
    chunk->read_offset += read_size;
    if (chunk->read_offset == entry->packed_size) {
        chunk->state = PREFETCH_READY;
    }
    return true;
}

bool prefetch_read_step(Movie *movie, PrefetchedChunk *chunk, bool respect_deadline, uint32_t deadline_ms)
{
    return prefetch_read_step_impl(movie, chunk, respect_deadline, deadline_ms, false);
}

bool prefetch_process_chunk(
    Movie *movie,
    PrefetchedChunk *chunk,
    uint32_t deadline_ms,
    bool respect_deadline,
    const PointerState *abort_pointer
)
{
    while (chunk && chunk->state != PREFETCH_READY) {
        if (prefetch_abort_requested(abort_pointer)) {
            return true;
        }
        if (chunk->state != PREFETCH_READING) {
            return false;
        }
        if (!prefetch_read_step(movie, chunk, respect_deadline, deadline_ms)) {
            return false;
        }
        /* A cooperative step is one bounded read, including a deferred read.
         * The caller may request more work after checking input/time again. */
        if (respect_deadline) {
            return true;
        }
        if (prefetch_abort_requested(abort_pointer)) {
            return true;
        }
    }
    return true;
}

bool prefetch_finish_chunk(Movie *movie, PrefetchedChunk *chunk)
{
    if (!chunk) {
        return false;
    }
    if (chunk->state == PREFETCH_READY) {
        return true;
    }
    if (!prefetch_process_chunk(movie, chunk, 0, false, NULL)) {
        return false;
    }
    return chunk->state == PREFETCH_READY;
}

bool load_chunk_from_file(Movie *movie, int chunk_index, bool allow_prefetch_retry)
{
    const ChunkIndexEntry *entry = movie->chunk_index + chunk_index;
    int previous_chunk = movie->loaded_chunk;
    size_t bytes_read;
    bool capture_read;
    uint64_t read_started_ticks;
    bool async_loaded = false;

retry:
    if (entry->packed_size != entry->unpacked_size) {
        debug_failf(
            "load chunk=%d unsupported packed=%lu unpacked=%lu",
            chunk_index,
            (unsigned long) entry->packed_size,
            (unsigned long) entry->unpacked_size
        );
        return false;
    }

    if (!allocate_movie_chunk_storage(movie, entry->packed_size)) {
        if (allow_prefetch_retry) {
            debug_failf(
                "load chunk=%d retry after packed alloc fail packed=%lu prefetched=%lu",
                chunk_index,
                (unsigned long) entry->packed_size,
                (unsigned long) total_prefetched_chunk_bytes(movie)
            );
            clear_all_prefetched_chunks(movie);
            allow_prefetch_retry = false;
            goto retry;
        }
        debug_failf("load chunk=%d packed alloc fail packed=%lu", chunk_index, (unsigned long) entry->packed_size);
        return false;
    }
    if (movie_async_enabled(movie)) {
        size_t offset = 0;
        while (offset < entry->packed_size) {
            size_t amount = entry->packed_size - offset;
            if (amount > MOVIE_ASYNC_BLOCK_BYTES) amount = MOVIE_ASYNC_BLOCK_BYTES;
            if (movie_async_read(movie, (uint64_t)entry->offset + offset,
                    movie->chunk_storage + offset, amount, chunk_index, true) != MOVIE_ASYNC_READY) break;
            offset += amount;
        }
        async_loaded = offset == entry->packed_size;
    }
    if (!async_loaded && movie->current_file_pos != (long) entry->offset &&
        fseek(movie->file, (long) entry->offset, SEEK_SET) != 0) {
        debug_failf("load chunk=%d fseek fail offset=%lu", chunk_index, (unsigned long) entry->offset);
        movie->current_file_pos = -1;
        release_movie_chunk_storage(movie);
        return false;
    }
    capture_read = !async_loaded && debug_is_runtime_logging_enabled();
    read_started_ticks = capture_read ? monotonic_clock_now_ticks() : 0;
    bytes_read = async_loaded ? entry->packed_size : fread(movie->chunk_storage, 1, entry->packed_size, movie->file);
    if (capture_read) {
        playback_capture_io(movie, 1, chunk_index, (uint32_t) bytes_read, read_started_ticks, monotonic_clock_now_ticks());
    }
    if (bytes_read != entry->packed_size) {
        debug_failf("load chunk=%d fread fail packed=%lu", chunk_index, (unsigned long) entry->packed_size);
        movie->current_file_pos = -1;
        release_movie_chunk_storage(movie);
        return false;
    }
    if (!async_loaded) movie->current_file_pos = (long) entry->offset + (long) entry->packed_size;
    movie->chunk_storage_size = entry->packed_size;
    if (!configure_chunk_view(movie, chunk_index)) {
        debug_tracef(
            "load chunk=%d configure fail storage=%lu frame_count=%u table=%lu",
            chunk_index,
            (unsigned long) movie->chunk_storage_size,
            (unsigned) entry->frame_count,
            (unsigned long) entry->frame_table_offset
        );
        release_movie_chunk_storage(movie);
        return false;
    }
    movie->loaded_chunk = chunk_index;
    if (!prepare_movie_codec_for_loaded_chunk(movie, previous_chunk, chunk_index)) {
        return false;
    }
    if (debug_should_collect_metrics()) {
        movie->diag_chunk_load_sync_count++;
    }
    debug_tracef(
        "load chunk=%d sync packed=%lu unpacked=%lu prefetched=%lu",
        chunk_index,
        (unsigned long) entry->packed_size,
        (unsigned long) entry->unpacked_size,
        (unsigned long) total_prefetched_chunk_bytes(movie)
    );
    return true;
}

int load_ready_chunk(Movie *movie, int chunk_index)
{
    if(!movie || !movie_uses_h264(movie) || chunk_index<0 ||
       (uint32_t)chunk_index>=movie->header.chunk_count)return -1;
    if(movie->loaded_chunk==chunk_index)
        return movie->chunk_bytes && movie->frame_offsets ? 1 : -1;
    /* Background adoption must never fall through to a blocking file read
     * or a decoder reset. Only the healthy sequential path is eligible. */
    int previous=movie->loaded_chunk;
    if(previous<0 || chunk_index!=previous+1 || !movie->h264.decoder_initialized ||
       movie->h264.decoder_failed)return 0;
    PrefetchedChunk *slot=find_prefetched_chunk(movie,chunk_index);
    if(!slot || slot->state!=PREFETCH_READY)return 0;
    if(!adopt_prefetched_movie_chunk(movie,slot))return -1;
    if(!configure_chunk_view(movie,chunk_index))return -1;
    movie->loaded_chunk=chunk_index;
    if(!prepare_movie_codec_for_loaded_chunk(movie,previous,chunk_index))return -1;
    if(debug_should_collect_metrics())++movie->diag_chunk_load_prefetched_count;
    return 1;
}

bool load_chunk(Movie *movie, int chunk_index)
{
    const ChunkIndexEntry *entry;
    PrefetchedChunk *prefetched;
    int previous_chunk;
    if (chunk_index < 0 || (uint32_t) chunk_index >= movie->header.chunk_count) {
        return false;
    }
    if (movie->loaded_chunk == chunk_index) {
        return true;
    }

    entry = movie->chunk_index + chunk_index;
    previous_chunk = movie->loaded_chunk;
    prefetched = find_prefetched_chunk(movie, chunk_index);
    if (prefetched) {
        if (prefetched->state != PREFETCH_READY) {
            if (!prefetch_finish_chunk(movie, prefetched)) {
                debug_tracef(
                    "prefetch finish fail chunk=%d state=%d read=%lu",
                    chunk_index,
                    (int) prefetched->state,
                    (unsigned long) prefetched->read_offset
                );
                clear_prefetched_chunk(prefetched);
                return load_chunk_from_file(movie, chunk_index, true);
            }
        }
        if (!adopt_prefetched_movie_chunk(movie, prefetched)) {
            clear_prefetched_chunk(prefetched);
            return load_chunk_from_file(movie, chunk_index, true);
        }
        if (!configure_chunk_view(movie, chunk_index)) {
            debug_tracef(
                "prefetched configure fail chunk=%d storage=%lu frame_count=%u table=%lu",
                chunk_index,
                (unsigned long) movie->chunk_storage_size,
                (unsigned) entry->frame_count,
                (unsigned long) entry->frame_table_offset
            );
            release_movie_chunk_storage(movie);
            return load_chunk_from_file(movie, chunk_index, false);
        }
        movie->loaded_chunk = chunk_index;
        if (!prepare_movie_codec_for_loaded_chunk(movie, previous_chunk, chunk_index)) {
            return false;
        }
        if (debug_should_collect_metrics()) {
            movie->diag_chunk_load_prefetched_count++;
        }
        debug_tracef(
            "load chunk=%d prefetched packed=%lu unpacked=%lu prefetched=%lu",
            chunk_index,
            (unsigned long) entry->packed_size,
            (unsigned long) entry->unpacked_size,
            (unsigned long) total_prefetched_chunk_bytes(movie)
        );
        return true;
    }

    return load_chunk_from_file(movie, chunk_index, true);
}

bool seek_bar_preview_decode_active(const SeekBarPreviewState *preview)
{
    return preview && preview->decode_job.active;
}

bool begin_seek_bar_preview_decode(Movie *movie, SeekBarPreviewState *preview, int chunk_index, uint32_t target_frame)
{
    const ChunkIndexEntry *entry;
    SeekPreviewDecodeJob *job;
    size_t frame_pixels;

    if (!movie || !preview || chunk_index < 0 || (uint32_t) chunk_index >= movie->header.chunk_count) {
        return false;
    }

    entry = movie->chunk_index + chunk_index;
    if (entry->frame_count == 0 ||
        target_frame < entry->first_frame ||
        target_frame >= entry->first_frame + entry->frame_count ||
        entry->packed_size != entry->unpacked_size) {
        return false;
    }

    clear_seek_bar_preview_decode_job(preview);
    job = &preview->decode_job;
    job->chunk_index = chunk_index;
    job->target_frame = target_frame;
    job->next_frame = entry->first_frame;
    job->chunk_storage_size = entry->packed_size;
    job->codec = movie->codec;

    frame_pixels = (size_t) movie->header.video_width * movie->header.video_height;
    job->chunk_storage = (uint8_t *) malloc(job->chunk_storage_size);
    job->pixels = (uint16_t *) malloc(frame_pixels * sizeof(uint16_t));
    if (!job->chunk_storage || !job->pixels) {
        clear_seek_bar_preview_decode_job(preview);
        return false;
    }
    if (movie_uses_h264(movie)) {
        bool capture_reset;
        bool reset_ok;
        uint64_t reset_started_ticks;

        job->decoder = h264bsdAlloc();
        if (!job->decoder) {
            clear_seek_bar_preview_decode_job(preview);
            return false;
        }
        capture_reset = debug_is_runtime_logging_enabled();
        reset_started_ticks = capture_reset ? monotonic_clock_now_ticks() : 0;
        reset_ok = reset_h264_storage_decoder(job->decoder, &job->decoder_initialized);
        if (capture_reset) {
            playback_capture_io(movie, 4, job->chunk_index, 0, reset_started_ticks, monotonic_clock_now_ticks());
        }
        if (!reset_ok) {
            clear_seek_bar_preview_decode_job(preview);
            return false;
        }
    } else if (movie->codec == MOVIE_CODEC_MPEG4) {
        if (!init_mpeg4_decoder_global() ||
            !mpeg4_xvid_create(
                &job->mpeg4_decoder,
                (int) movie->header.video_width,
                (int) movie->header.video_height)) {
            clear_seek_bar_preview_decode_job(preview);
            return false;
        }
    } else {
        clear_seek_bar_preview_decode_job(preview);
        return false;
    }

    job->active = true;
    return true;
}

bool read_seek_bar_preview_chunk_step(Movie *movie, SeekPreviewDecodeJob *job, uint32_t deadline_ms)
{
    const ChunkIndexEntry *entry;

    if (!movie || !job || !job->active || job->chunk_index < 0 ||
        (uint32_t) job->chunk_index >= movie->header.chunk_count) {
        return false;
    }
    if (job->frame_offsets) {
        return true;
    }

    entry = movie->chunk_index + job->chunk_index;
    while (job->read_offset < job->chunk_storage_size) {
        size_t remaining;
        size_t read_size;
        long target_pos;
        size_t bytes_read;
        bool capture_read;
        uint64_t read_started_ticks;

        if (deadline_ms != 0U && prefetch_deadline_reached(deadline_ms)) {
            return true;
        }

        remaining = job->chunk_storage_size - job->read_offset;
        read_size = remaining > SEEK_BAR_PREVIEW_IO_BLOCK_SIZE
            ? SEEK_BAR_PREVIEW_IO_BLOCK_SIZE
            : remaining;
        target_pos = (long) (entry->offset + job->read_offset);
        if (movie_async_enabled(movie)) {
            if (read_size > MOVIE_ASYNC_BLOCK_BYTES) read_size = MOVIE_ASYNC_BLOCK_BYTES;
            int result = movie_async_read(movie, (uint64_t)entry->offset + job->read_offset,
                job->chunk_storage + job->read_offset, read_size, job->chunk_index, deadline_ms == 0U);
            if (result == MOVIE_ASYNC_PENDING) return true;
            if (result == MOVIE_ASYNC_READY) {
                job->read_offset += read_size;
                continue;
            }
        }
        if (movie->current_file_pos != target_pos) {
            if (fseek(movie->file, target_pos, SEEK_SET) != 0) {
                movie->current_file_pos = -1;
                return false;
            }
            movie->current_file_pos = target_pos;
        }
        capture_read = debug_is_runtime_logging_enabled();
        read_started_ticks = capture_read ? monotonic_clock_now_ticks() : 0;
        bytes_read = fread(job->chunk_storage + job->read_offset, 1, read_size, movie->file);
        if (capture_read) {
            playback_capture_io(movie, 3, job->chunk_index, (uint32_t) bytes_read, read_started_ticks, monotonic_clock_now_ticks());
        }
        if (bytes_read != read_size) {
            movie->current_file_pos = -1;
            return false;
        }
        movie->current_file_pos += (long) read_size;
        job->read_offset += read_size;
    }

    return configure_chunk_view_from_storage(
        movie,
        job->chunk_index,
        job->chunk_storage,
        job->chunk_storage_size,
        &job->frame_offsets,
        &job->chunk_bytes,
        &job->chunk_size
    );
}

static bool publish_seek_bar_preview_pixels(Movie *movie, SeekBarPreviewState *preview, uint32_t frame_index)
{
    SeekPreviewDecodeJob *job;
    SDL_Surface *full_surface = NULL;
    SDL_Surface *thumbnail = NULL;
    bool had_surface;

    if (!movie || !preview) {
        return false;
    }

    job = &preview->decode_job;
    if (!job->pixels) {
        return false;
    }

    full_surface = SDL_CreateRGBSurfaceFrom(
        job->pixels,
        movie->header.video_width,
        movie->header.video_height,
        16,
        movie->header.video_width * 2,
        0xF800, 0x07E0, 0x001F, 0
    );
    if (!full_surface) {
        return false;
    }
    thumbnail = create_scaled_surface_from_surface(full_surface, SEEK_BAR_PREVIEW_MAX_W, SEEK_BAR_PREVIEW_MAX_H);
    SDL_FreeSurface(full_surface);
    if (!thumbnail) {
        return false;
    }

    had_surface = preview->surface != NULL;
    if (preview->surface) {
        SDL_FreeSurface(preview->surface);
    }
    preview->surface = thumbnail;
    preview->decoded_chunk_index = job->chunk_index;
    preview->decoded_frame_index = frame_index;
    preview->surface_render_pending = true;
    if (!had_surface) {
        preview->surface_started_ms = 0;
        preview->surface_fade_pending = true;
    } else {
        if (preview->surface_started_ms == 0U) {
            uint32_t now_ms = monotonic_clock_now_ms();
            preview->surface_started_ms = now_ms > UI_TOOLTIP_ANIM_MS ? (now_ms - UI_TOOLTIP_ANIM_MS) : 1U;
        }
        preview->surface_fade_pending = false;
    }
    return true;
}

bool publish_seek_bar_preview_picture(Movie *movie, SeekBarPreviewState *preview, uint32_t frame_index, const uint8_t *picture)
{
    SeekPreviewDecodeJob *job;

    if (!movie || !preview || !picture) {
        return false;
    }

    job = &preview->decode_job;
    if (!job->pixels ||
        !blit_h264_picture_to_target(movie, picture, job->pixels, movie->header.video_width)) {
        return false;
    }

    return publish_seek_bar_preview_pixels(movie, preview, frame_index);
}

uint32_t seek_bar_preview_macroblock_budget(
    const Movie *movie,
    const storage_t *decoder,
    uint16_t avg_mbs_per_ms_q8,
    uint32_t spare_ms
)
{
    if (!decoder) {
        return h264_incremental_total_mbs(movie, decoder);
    }
    return h264_incremental_budget(movie, decoder, avg_mbs_per_ms_q8, spare_ms);
}

static void step_mpeg4_seek_bar_preview_decode(Movie *movie, SeekBarPreviewState *preview, uint32_t deadline_ms)
{
    SeekPreviewDecodeJob *job;
    const ChunkIndexEntry *entry;

    job = &preview->decode_job;
    if (!read_seek_bar_preview_chunk_step(movie, job, deadline_ms)) {
        clear_seek_bar_preview_decode_job(preview);
        return;
    }
    if (!job->frame_offsets) {
        return;
    }

    entry = movie->chunk_index + job->chunk_index;
    while (job->active && job->next_frame <= job->target_frame) {
        uint32_t local_index;
        size_t start;
        size_t end;

        if (deadline_ms != 0U && prefetch_deadline_reached(deadline_ms)) {
            return;
        }

        local_index = job->next_frame - entry->first_frame;
        if (local_index >= entry->frame_count) {
            clear_seek_bar_preview_decode_job(preview);
            return;
        }

        start = job->frame_offsets[local_index];
        end = (local_index + 1U < entry->frame_count) ? job->frame_offsets[local_index + 1U] : job->chunk_size;
        if (end <= start || end > job->chunk_size) {
            clear_seek_bar_preview_decode_job(preview);
            return;
        }

        if (!job->mpeg4_decoder ||
            !mpeg4_xvid_decode_frame(
                job->mpeg4_decoder,
                job->chunk_bytes + start,
                end - start,
                job->pixels,
                (int) movie->header.video_width,
                (int) movie->header.video_height,
                true,
                false)) {
            debug_tracef(
                "mpeg4 seek preview decode fail frame=%lu chunk=%d local=%lu: %s",
                (unsigned long) job->next_frame,
                job->chunk_index,
                (unsigned long) local_index,
                mpeg4_xvid_last_error()
            );
            clear_seek_bar_preview_decode_job(preview);
            return;
        }

        if (!publish_seek_bar_preview_pixels(movie, preview, job->next_frame)) {
            clear_seek_bar_preview_decode_job(preview);
            return;
        }

        job->next_frame++;
        if (job->next_frame > job->target_frame) {
            finish_seek_bar_preview_decode_job(preview);
        }
        return;
    }

    clear_seek_bar_preview_decode_job(preview);
}

void step_seek_bar_preview_decode(Movie *movie, SeekBarPreviewState *preview, uint32_t deadline_ms)
{
    SeekPreviewDecodeJob *job;
    const ChunkIndexEntry *entry;

    if (!movie || !preview || !preview->decode_job.active) {
        return;
    }
    if (preview->surface_render_pending) {
        return;
    }

    job = &preview->decode_job;
    if (job->codec == MOVIE_CODEC_MPEG4) {
        step_mpeg4_seek_bar_preview_decode(movie, preview, deadline_ms);
        return;
    }

    if (!read_seek_bar_preview_chunk_step(movie, job, deadline_ms)) {
        clear_seek_bar_preview_decode_job(preview);
        return;
    }
    if (!job->frame_offsets) {
        return;
    }

    entry = movie->chunk_index + job->chunk_index;
    while (job->active && job->next_frame <= job->target_frame) {
        uint32_t local_index;
        size_t start;
        size_t end;
        uint32_t remaining_ms;
        uint32_t macroblock_budget;
        uint32_t decode_start_ms;
        uint32_t start_decoded_mbs;
        uint32_t total_mbs;
        uint32_t decoded_mbs;
        bool picture_ready = false;
        bool pending = false;
        uint8_t *picture = NULL;

        if (deadline_ms != 0U) {
            uint32_t now_ms = monotonic_clock_now_ms();
            if ((int32_t) (deadline_ms - now_ms) <= 0) {
                return;
            }
            remaining_ms = deadline_ms - now_ms;
        } else {
            remaining_ms = SEEK_BAR_PREVIEW_SLICE_MS;
        }

        local_index = job->next_frame - entry->first_frame;
        if (local_index >= entry->frame_count) {
            clear_seek_bar_preview_decode_job(preview);
            return;
        }

        start = job->frame_offsets[local_index];
        end = (local_index + 1U < entry->frame_count) ? job->frame_offsets[local_index + 1U] : job->chunk_size;
        if (end <= start || end > job->chunk_size) {
            clear_seek_bar_preview_decode_job(preview);
            return;
        }

        macroblock_budget = seek_bar_preview_macroblock_budget(movie, job->decoder, job->avg_mbs_per_ms_q8, remaining_ms);
        if (macroblock_budget == 0U) {
            return;
        }

        decode_start_ms = monotonic_clock_now_ms();
        start_decoded_mbs = job->decoder->slice->numDecodedMbs;
        total_mbs = h264_incremental_total_mbs(movie, job->decoder);
        if (!pump_h264_access_unit(
                movie,
                job->decoder,
                job->chunk_bytes + start,
                end - start,
                &job->consumed_bytes,
                &job->zero_advance_retries,
                macroblock_budget,
                false,
                "seek preview",
                &picture_ready,
                &pending,
                &picture)) {
            clear_seek_bar_preview_decode_job(preview);
            return;
        }

        decoded_mbs = job->decoder->slice->numDecodedMbs - start_decoded_mbs;
        if (picture_ready && decoded_mbs == 0U) {
            decoded_mbs = total_mbs > start_decoded_mbs ? (total_mbs - start_decoded_mbs) : 0U;
        }
        update_h264_incremental_rate(&job->avg_mbs_per_ms_q8, monotonic_clock_now_ms() - decode_start_ms, decoded_mbs);

        if (picture_ready) {
            if (!publish_seek_bar_preview_picture(movie, preview, job->next_frame, picture)) {
                clear_seek_bar_preview_decode_job(preview);
                return;
            }

            job->next_frame++;
            job->consumed_bytes = 0;
            job->zero_advance_retries = 0;
            if (job->next_frame > job->target_frame) {
                finish_seek_bar_preview_decode_job(preview);
            }
            return;
        } else if (pending) {
            return;
        } else {
            clear_seek_bar_preview_decode_job(preview);
            return;
        }

        if (deadline_ms != 0U && prefetch_deadline_reached(deadline_ms)) {
            return;
        }
    }

    clear_seek_bar_preview_decode_job(preview);
}

bool finish_seek_bar_preview_pending_frame(Movie *movie, SeekBarPreviewState *preview)
{
    SeekPreviewDecodeJob *job;
    const ChunkIndexEntry *entry;
    uint32_t local_index;
    size_t start;
    size_t end;
    bool picture_ready = false;
    bool pending = false;
    uint8_t *picture = NULL;

    if (!movie || !preview || !preview->decode_job.active) {
        return true;
    }

    job = &preview->decode_job;
    if (job->codec != MOVIE_CODEC_H264) {
        return true;
    }
    if (job->consumed_bytes == 0) {
        return true;
    }
    if (!job->frame_offsets || !job->chunk_bytes ||
        job->chunk_index < 0 ||
        (uint32_t) job->chunk_index >= movie->header.chunk_count) {
        return false;
    }

    entry = movie->chunk_index + job->chunk_index;
    if (job->next_frame < entry->first_frame || job->next_frame >= entry->first_frame + entry->frame_count) {
        return false;
    }
    local_index = job->next_frame - entry->first_frame;
    start = job->frame_offsets[local_index];
    end = (local_index + 1U < entry->frame_count) ? job->frame_offsets[local_index + 1U] : job->chunk_size;
    if (end <= start || end > job->chunk_size) {
        return false;
    }

    if (!pump_h264_access_unit(
            movie,
            job->decoder,
            job->chunk_bytes + start,
            end - start,
            &job->consumed_bytes,
            &job->zero_advance_retries,
            0U,
            false,
            "seek preview commit",
            &picture_ready,
            &pending,
            &picture)) {
        return false;
    }
    if (pending || !picture_ready || !picture) {
        return false;
    }
    if (!publish_seek_bar_preview_picture(movie, preview, job->next_frame, picture)) {
        return false;
    }

    job->next_frame++;
    job->consumed_bytes = 0;
    job->zero_advance_retries = 0;
    if (job->next_frame > job->target_frame) {
        finish_seek_bar_preview_decode_job(preview);
    }
    return true;
}

bool prefetch_chunk(Movie *movie, int chunk_index)
{
    const ChunkIndexEntry *entry;
    PrefetchedChunk *slot = NULL;
    size_t capacity;
    int index;

    if (!movie || chunk_index < 0 || (uint32_t) chunk_index >= movie->header.chunk_count) {
        return false;
    }
    if (movie->loaded_chunk == chunk_index || find_prefetched_chunk(movie, chunk_index)) {
        return true;
    }
    entry = movie->chunk_index + chunk_index;
    if (entry->packed_size != entry->unpacked_size || entry->unpacked_size == 0 ||
        entry->unpacked_size > PREFETCH_MAX_TOTAL_BYTES) {
        return false;
    }
    /* Prefer the smallest retained buffer that fits. Once the rotating slots
     * have warmed up, ordinary playback does not touch the heap at all. */
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        PrefetchedChunk *candidate = &movie->prefetched[index];
        if (candidate->chunk_index >= 0) {
            continue;
        }
        if (!slot ||
            (candidate->chunk_capacity >= entry->unpacked_size &&
                (slot->chunk_capacity < entry->unpacked_size || candidate->chunk_capacity < slot->chunk_capacity)) ||
            (slot->chunk_capacity < entry->unpacked_size && candidate->chunk_capacity > slot->chunk_capacity)) {
            slot = candidate;
        }
    }
    if (!slot) {
        slot = &movie->prefetched[0];
        for (index = 1; index < PREFETCH_CHUNK_COUNT; ++index) {
            if (movie->prefetched[index].chunk_index < slot->chunk_index) {
                slot = &movie->prefetched[index];
            }
        }
        clear_prefetched_chunk(slot);
    }

    if (!slot->chunk_storage || slot->chunk_capacity < entry->unpacked_size) {
        clear_prefetched_chunk(slot);
        /* Small bins absorb typical neighbouring chunk-size variation, while
         * the full capacity remains charged to the existing memory budget. */
        capacity = ((size_t) entry->unpacked_size + 4095U) & ~(size_t) 4095U;
        if (capacity > PREFETCH_MAX_TOTAL_BYTES) {
            capacity = entry->unpacked_size;
        }
        if (!ensure_prefetch_budget(movie, chunk_index, capacity)) {
            return false;
        }
        slot->chunk_storage = (uint8_t *) player_malloc_aligned(
            capacity,
            PLAYER_CACHE_LINE_SIZE,
            &slot->chunk_allocation
        );
        if (!slot->chunk_storage) {
            debug_tracef("prefetch alloc fail chunk=%d unpacked=%lu total=%lu",
                        chunk_index, (unsigned long) entry->unpacked_size,
                        (unsigned long) total_prefetched_chunk_bytes(movie));
            return false;
        }
        slot->chunk_capacity = capacity;
    }
    slot->chunk_storage_size = entry->unpacked_size;
    slot->chunk_index = chunk_index;
    slot->state = PREFETCH_READING;
    slot->read_offset = 0;
    debug_tracef(
        "prefetch start chunk=%d packed=%lu unpacked=%lu total=%lu",
        chunk_index,
        (unsigned long) entry->packed_size,
        (unsigned long) entry->unpacked_size,
        (unsigned long) total_prefetched_chunk_bytes(movie)
    );
    return true;
}

void prefetch_ahead(Movie *movie, int current_chunk, int max_new_chunks, int max_new_distance)
{
    int index;
    int loaded = 0;

    if (max_new_distance < 1) {
        max_new_distance = 1;
    } else if (max_new_distance > PREFETCH_CHUNK_COUNT) {
        max_new_distance = PREFETCH_CHUNK_COUNT;
    }

    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        int wanted_min = current_chunk + 1;
        int wanted_max = current_chunk + PREFETCH_CHUNK_COUNT;
        if (movie->prefetched[index].chunk_index >= 0 &&
            (movie->prefetched[index].chunk_index < wanted_min || movie->prefetched[index].chunk_index > wanted_max)) {
            /* Drop stale contents while retaining a budgeted allocation. */
            movie->prefetched[index].chunk_index = -1;
            movie->prefetched[index].state = PREFETCH_IDLE;
            movie->prefetched[index].chunk_storage_size = 0;
            movie->prefetched[index].read_offset = 0;
        }
    }
    for (index = 1; index <= max_new_distance; ++index) {
        int wanted_chunk = current_chunk + index;
        if ((uint32_t) wanted_chunk < movie->header.chunk_count &&
            movie->loaded_chunk != wanted_chunk &&
            !find_prefetched_chunk(movie, wanted_chunk)) {
            if (!prefetch_chunk(movie, wanted_chunk)) {
                break;
            }
            loaded++;
            if (loaded >= max_new_chunks) {
                break;
            }
        }
    }
}

void prefetch_do_work(
    Movie *movie,
    int current_chunk,
    int max_work_distance,
    uint32_t deadline_ms,
    bool single_step,
    const PointerState *abort_pointer
)
{
    while (!prefetch_deadline_reached(deadline_ms)) {
        PrefetchedChunk *slot = find_prefetch_work_chunk(movie, current_chunk, max_work_distance);
        size_t previous_read_offset;
        if (!slot) {
            break;
        }
        /* prefetch_process_chunk checks input immediately before its read. */
        previous_read_offset = slot->read_offset;
        if (!prefetch_process_chunk(movie, slot, deadline_ms, true, abort_pointer)) {
            debug_tracef(
                "prefetch work fail chunk=%d state=%d read=%lu",
                slot->chunk_index,
                (int) slot->state,
                (unsigned long) slot->read_offset
            );
            clear_prefetched_chunk(slot);
            break;
        }
        if (slot->read_offset == previous_read_offset && slot->state != PREFETCH_READY) {
            break;
        }
        if (single_step) {
            break;
        }
    }
}

int prefetch_budget_for_state(const Movie *movie, bool paused, uint32_t spare_ms)
{
    if (paused) {
        return PREFETCH_CHUNK_COUNT;
    }
    if (movie_uses_h264(movie) && spare_ms < h264_prefetch_io_min_spare_ms(movie)) {
        return 0;
    }
    if (spare_ms >= 24U) {
        return 2;
    }
    if (spare_ms > 0) {
        return 1;
    }
    return 0;
}
 
int prefetch_target_chunk(const Movie *movie)
{
    int current_chunk;

    if (!movie) {
        return -1;
    }

    if(h264_lookahead_active(movie) && movie->loaded_chunk>=0 &&
       (uint32_t)movie->loaded_chunk<movie->header.chunk_count)return movie->loaded_chunk;

    current_chunk = movie_chunk_for_frame(movie, movie->current_frame);
    if (current_chunk >= 0) {
        return current_chunk;
    }

    if (movie->loaded_chunk < 0) {
        return -1;
    }
    if ((uint32_t) movie->loaded_chunk >= movie->header.chunk_count) {
        return -1;
    }
    return movie->loaded_chunk;
}

bool next_chunk_needs_prefetch(const Movie *movie, int current_chunk)
{
    int index;
    int next_chunk = current_chunk + 1;

    if (!movie || current_chunk < 0 || (uint32_t) next_chunk >= movie->header.chunk_count) {
        return false;
    }
    if (movie->loaded_chunk == next_chunk) {
        return false;
    }
    for (index = 0; index < PREFETCH_CHUNK_COUNT; ++index) {
        const PrefetchedChunk *prefetched = &movie->prefetched[index];
        if (prefetched->chunk_index == next_chunk) {
            return prefetched->state != PREFETCH_READY;
        }
    }
    return true;
}

bool next_chunk_prefetched_ready(const Movie *movie, int current_chunk)
{
    const PrefetchedChunk *prefetched;
    int next_chunk = current_chunk + 1;

    if (!movie || current_chunk < 0 || (uint32_t) next_chunk >= movie->header.chunk_count) {
        return false;
    }
    if (movie->loaded_chunk == next_chunk) {
        return true;
    }
    prefetched = find_prefetched_chunk_const(movie, next_chunk);
    return prefetched && prefetched->state == PREFETCH_READY;
}

uint32_t next_chunk_prefetch_guard_frames(const Movie *movie, int current_chunk)
{
    const ChunkIndexEntry *entry;
    uint32_t guard_frames = H264_PREFETCH_NEXT_CHUNK_GUARD_FRAMES;
    uint32_t idr_frame_bytes = 0U;
    int next_chunk = current_chunk + 1;

    if (!movie || current_chunk < 0 || (uint32_t) next_chunk >= movie->header.chunk_count) {
        return guard_frames;
    }

    entry = movie->chunk_index + next_chunk;
    if (entry->unpacked_size >= 768U * 1024U) {
        guard_frames += 16U;
    } else if (entry->unpacked_size >= 512U * 1024U) {
        guard_frames += 12U;
    } else if (entry->unpacked_size >= 256U * 1024U) {
        guard_frames += 8U;
    } else if (entry->unpacked_size >= 96U * 1024U) {
        guard_frames += 6U;
    }
    idr_frame_bytes = estimate_h264_frame_bytes(movie, entry->first_frame);
    if (idr_frame_bytes >= 12288U) {
        guard_frames += 8U;
    } else if (idr_frame_bytes >= 8192U) {
        guard_frames += 4U;
    } else if (idr_frame_bytes >= 4096U) {
        guard_frames += 2U;
    }
    if (movie_uses_h264(movie)) {
        if (movie->h264.foreground_decode_avg_ms >= 34U) {
            guard_frames += 10U;
        } else if (movie->h264.foreground_decode_avg_ms >= 30U) {
            guard_frames += 6U;
        } else if (movie->h264.foreground_decode_avg_ms >= 28U) {
            guard_frames += 3U;
        }
    }
    return guard_frames;
}

bool should_accelerate_next_chunk_io(Movie *movie, int current_chunk)
{
    const ChunkIndexEntry *entry;
    const ChunkIndexEntry *next_entry;
    PrefetchedChunk *prefetched;
    uint32_t frames_remaining;
    uint32_t catchup_window;
    int next_chunk = current_chunk + 1;

    if (!movie || !movie_uses_h264(movie) || current_chunk < 0 ||
        (uint32_t) current_chunk >= movie->header.chunk_count ||
        (uint32_t) next_chunk >= movie->header.chunk_count) {
        return false;
    }

    prefetched = find_prefetched_chunk(movie, next_chunk);
    if (!prefetched || prefetched->state != PREFETCH_READING || prefetched->chunk_storage == NULL) {
        return false;
    }

    entry = movie->chunk_index + current_chunk;
    next_entry = movie->chunk_index + next_chunk;
    if (movie->current_frame < entry->first_frame) {
        return true;
    }

    frames_remaining = (entry->first_frame + entry->frame_count) - movie->current_frame;
    catchup_window = next_chunk_prefetch_guard_frames(movie, current_chunk) + H264_PREFETCH_NEXT_CHUNK_IO_CATCHUP_FRAMES;
    if (movie_uses_h264(movie) && movie->h264.foreground_decode_avg_ms >= 30U) {
        catchup_window += 8U;
    }
    if (next_entry->unpacked_size >= 96U * 1024U) {
        catchup_window += 8U;
    }
    return frames_remaining <= catchup_window;
}

uint32_t second_next_chunk_prefetch_window_frames(const Movie *movie, int current_chunk)
{
    const ChunkIndexEntry *entry;
    uint32_t window_frames = H264_PREFETCH_SECOND_NEXT_CHUNK_WINDOW_FRAMES;
    uint32_t idr_frame_bytes = 0U;
    int second_next_chunk = current_chunk + 2;

    if (!movie || current_chunk < 0 || (uint32_t) second_next_chunk >= movie->header.chunk_count) {
        return window_frames;
    }

    entry = movie->chunk_index + second_next_chunk;
    if (entry->unpacked_size >= 96U * 1024U) {
        window_frames += 24U;
    } else if (entry->unpacked_size >= 64U * 1024U) {
        window_frames += 12U;
    } else if (entry->unpacked_size >= 48U * 1024U) {
        window_frames += 8U;
    }

    idr_frame_bytes = estimate_h264_frame_bytes(movie, entry->first_frame);
    if (idr_frame_bytes >= 12288U) {
        window_frames += 8U;
    } else if (idr_frame_bytes >= 8192U) {
        window_frames += 4U;
    } else if (idr_frame_bytes >= 4096U) {
        window_frames += 2U;
    }

    if (movie_uses_h264(movie)) {
        if (movie->h264.foreground_decode_avg_ms >= 30U) {
            window_frames += 8U;
        } else if (movie->h264.foreground_decode_avg_ms >= 28U) {
            window_frames += 4U;
        }
    }

    return window_frames;
}

bool should_prefetch_second_next_chunk(Movie *movie, int current_chunk)
{
    const ChunkIndexEntry *entry;
    uint32_t frames_remaining;
    int second_next_chunk = current_chunk + 2;

    if (!movie || !movie_uses_h264(movie) || current_chunk < 0 ||
        (uint32_t) current_chunk >= movie->header.chunk_count ||
        (uint32_t) second_next_chunk >= movie->header.chunk_count) {
        return false;
    }

    entry = movie->chunk_index + current_chunk;
    if (movie->current_frame < entry->first_frame) {
        return true;
    }

    frames_remaining = (entry->first_frame + entry->frame_count) - movie->current_frame;
    return frames_remaining <= second_next_chunk_prefetch_window_frames(movie, current_chunk);
}

bool should_prioritize_next_chunk_io(const Movie *movie, int current_chunk)
{
    const ChunkIndexEntry *entry;
    uint32_t frames_remaining;
    uint32_t guard_frames;

    if (!movie || current_chunk < 0 || (uint32_t) current_chunk >= movie->header.chunk_count) {
        return false;
    }
    if (!next_chunk_needs_prefetch(movie, current_chunk)) {
        return false;
    }

    entry = movie->chunk_index + current_chunk;
    guard_frames = next_chunk_prefetch_guard_frames(movie, current_chunk);
    if (movie->current_frame < entry->first_frame) {
        return true;
    }

    frames_remaining = (entry->first_frame + entry->frame_count) - movie->current_frame;
    return frames_remaining <= guard_frames;
}

void prefetch_tick(Movie *movie, bool paused, uint32_t spare_ms, const PointerState *abort_pointer)
{
    uint32_t time_slice_ms;
    int current_chunk;
    int budget;
    bool prioritize_io = false;
    bool accelerate_next_chunk_io = false;
    bool next_chunk_ready = false;
    bool maintenance = false;
    int max_new_prefetch_distance = PREFETCH_CHUNK_COUNT;
    int max_work_distance = PREFETCH_CHUNK_COUNT;

    if (!movie) {
        return;
    }
    if (movie_async_enabled(movie)) {
        /* Fill bounded lookahead early. Unlike an OS worker, the independent
         * reader needs explicit CPU slices even when decoding uses all slack. */
        uint32_t service_started_ms=spare_ms?monotonic_clock_now_ms():0U;
        current_chunk = prefetch_target_chunk(movie);
        if (current_chunk < 0) return;
        /* Decide before polling the reader: even a pending-read poll spends
         * a bounded SPI slice. At high playback rates the post-render decode
         * deadline is already due, so deep lookahead belongs in real slack. */
        uint32_t buffered_end=movie->chunk_index[current_chunk].first_frame+
            movie->chunk_index[current_chunk].frame_count;
        for(int n=current_chunk+1;n<current_chunk+PREFETCH_CHUNK_COUNT+1 &&
            (uint32_t)n<movie->header.chunk_count;++n){
            PrefetchedChunk *ready=find_prefetched_chunk(movie,n);
            if(!ready || ready->state!=PREFETCH_READY)break;
            buffered_end=movie->chunk_index[n].first_frame+movie->chunk_index[n].frame_count;
        }
        uint32_t needed=h264_lookahead_active(movie)?h264_lookahead_next_frame(movie):movie->current_frame;
        uint32_t runway=buffered_end>needed?buffered_end-needed:0;
        if(!prefetch_async_should_work(paused,runway,spare_ms))return;
        PrefetchedChunk *work = find_prefetch_work_chunk(movie, current_chunk, PREFETCH_CHUNK_COUNT);
        bool missing = false;
        for (int distance = 1; !work && distance <= PREFETCH_CHUNK_COUNT; ++distance) {
            int wanted = current_chunk + distance;
            if ((uint32_t)wanted >= movie->header.chunk_count) break;
            if (!find_prefetched_chunk(movie, wanted)) { missing = true; break; }
        }
        if ((!work && !missing) || prefetch_abort_requested(abort_pointer)) return;
        prefetch_ahead(movie, current_chunk, 1, PREFETCH_CHUNK_COUNT);
        work = find_prefetch_work_chunk(movie, current_chunk, PREFETCH_CHUNK_COUNT);
        size_t before_read=work?work->read_offset:0;
        if (work && !prefetch_read_step(movie, work, true, 0)) clear_prefetched_chunk(work);
        if(movie_async_enabled(movie) && work && work->state==PREFETCH_READING && work->read_offset==before_read){
            unsigned service_ticks=prefetch_async_remaining_ticks(runway,spare_ms,
                spare_ms?monotonic_clock_now_ms()-service_started_ms:0U);
            if(!service_ticks)return;
            movie_async_service(movie,service_ticks);
            /* Publish a block completed in that slice instead of leaving its
             * READY bytes parked until the next video frame. */
            if(!prefetch_read_step(movie,work,true,0))clear_prefetched_chunk(work);
        }
        return;
    }
    if (spare_ms <= PREFETCH_IO_GUARD_MS) {
        if (paused || spare_ms != 0) {
            return;
        }
        current_chunk = prefetch_target_chunk(movie);
        if (current_chunk < 0 || !should_prioritize_next_chunk_io(movie, current_chunk)) {
            return;
        }
        /* Under sustained decode overload there is no positive spare time.
         * Only an urgent, incomplete next chunk earns this small slice. */
        maintenance = true;
        prioritize_io = true;
        time_slice_ms = PREFETCH_IO_OVERLOAD_SLICE_MS;
        budget = 1;
        max_new_prefetch_distance = max_work_distance = 1;
        if (debug_should_collect_metrics()) {
            movie->diag_io_priority_count++;
        }
    } else {
        time_slice_ms = paused && spare_ms > PREFETCH_PAUSED_SLICE_MS ? PREFETCH_PAUSED_SLICE_MS : spare_ms;
        current_chunk = prefetch_target_chunk(movie);
        budget = prefetch_budget_for_state(movie, paused, spare_ms);
    }

    if (movie_uses_h264(movie)) {
        if (debug_should_collect_metrics()) {
            movie->diag_prefetch_tick_count++;
        }
        if (!paused) {
            if (debug_should_collect_metrics()) {
                movie->diag_active_prefetch_tick_count++;
            }
        }
    }

    if (!maintenance && !paused && movie_uses_h264(movie) && current_chunk >= 0) {
        bool allow_second_next_chunk = false;

        max_new_prefetch_distance = 1;
        max_work_distance = 1;
        next_chunk_ready = next_chunk_prefetched_ready(movie, current_chunk);
        allow_second_next_chunk = should_prefetch_second_next_chunk(movie, current_chunk);
        if (allow_second_next_chunk) {
            max_new_prefetch_distance = 2;
            max_work_distance = 2;
        }
        accelerate_next_chunk_io = should_accelerate_next_chunk_io(movie, current_chunk);
        prioritize_io = should_prioritize_next_chunk_io(movie, current_chunk);
        if (prioritize_io || accelerate_next_chunk_io) {
            if (debug_should_collect_metrics()) {
                movie->diag_io_priority_count++;
            }
            if (time_slice_ms < H264_PREFETCH_IO_PRIORITY_SLICE_MS && spare_ms > 0) {
                time_slice_ms = spare_ms < H264_PREFETCH_IO_PRIORITY_SLICE_MS
                    ? spare_ms
                    : H264_PREFETCH_IO_PRIORITY_SLICE_MS;
            }
        }

        if (prioritize_io) {
            budget = 1;
        } else if (next_chunk_ready && !allow_second_next_chunk) {
            budget = 0;
        } else {
            budget = 1;
        }
    }

    if (!paused && movie_uses_h264(movie) && !prioritize_io) {
        uint32_t max_io_slice = accelerate_next_chunk_io
            ? spare_ms
            : ((spare_ms > 16U) ? (spare_ms / 2U) : PREFETCH_ACTIVE_H264_SLICE_MS);
        if (next_chunk_ready && max_io_slice > PREFETCH_ACTIVE_H264_SLICE_MS) {
            max_io_slice = PREFETCH_ACTIVE_H264_SLICE_MS;
        }
        if (time_slice_ms > max_io_slice) {
            time_slice_ms = max_io_slice;
        }
    }
    if (current_chunk >= 0 && budget > 0 && time_slice_ms > 0) {
        bool work_pending = find_prefetch_work_chunk(movie, current_chunk, max_work_distance) != NULL;
        int distance;
        /* Most playback ticks already have their next chunks ready. Avoid
         * another expensive keypad/touchpad scan unless I/O can do work. */
        for (distance = 1; !work_pending && distance <= max_new_prefetch_distance; ++distance) {
            int wanted = current_chunk + distance;
            if ((uint32_t) wanted >= movie->header.chunk_count) {
                break;
            }
            work_pending = movie->loaded_chunk != wanted && !find_prefetched_chunk(movie, wanted);
        }
        if (!work_pending || prefetch_abort_requested(abort_pointer)) {
            return;
        }
        uint32_t io_start_ms = monotonic_clock_now_ms();
        uint32_t io_deadline_ms = io_start_ms + time_slice_ms;
        if (!io_deadline_ms || !prefetch_deadline_reached(io_deadline_ms)) {
            prefetch_ahead(movie, current_chunk, budget, max_new_prefetch_distance);
            if (maintenance) {
                PrefetchedChunk *slot = find_prefetch_work_chunk(movie, current_chunk, 1);
                if (slot && !prefetch_abort_requested(abort_pointer) &&
                    !prefetch_read_step_impl(movie, slot, true, io_deadline_ms, true)) {
                    clear_prefetched_chunk(slot);
                }
            } else {
                prefetch_do_work(
                    movie,
                    current_chunk,
                    max_work_distance,
                    io_deadline_ms,
                    !paused && !prioritize_io && !accelerate_next_chunk_io,
                    abort_pointer
                );
            }
            if (!paused) {
                uint32_t io_elapsed_ms = monotonic_clock_now_ms() - io_start_ms;
                if (io_elapsed_ms >= time_slice_ms + 8U) {
                    debug_tracef(
                        "prefetch io overrun ms=%lu slice=%lu spare=%lu chunk=%d prio=%u",
                        (unsigned long) io_elapsed_ms,
                        (unsigned long) time_slice_ms,
                        (unsigned long) spare_ms,
                        current_chunk,
                        prioritize_io ? 1U : 0U
                    );
                }
            }
        }
    }
}

int movie_chunk_for_frame(const Movie *movie, uint32_t frame_index)
{
    uint32_t left = 0;
    uint32_t right = movie->header.chunk_count;

    /* Playback and prefetch queries usually stay in the loaded chunk or cross
     * its next boundary. Avoid searching the entire index for those frames. */
    if (movie->loaded_chunk >= 0 && (uint32_t) movie->loaded_chunk < right) {
        uint32_t loaded = (uint32_t) movie->loaded_chunk;
        const ChunkIndexEntry *entry = movie->chunk_index + loaded;
        if (frame_index >= entry->first_frame &&
            frame_index - entry->first_frame < entry->frame_count) {
            return (int) loaded;
        }
        if (loaded + 1U < right) {
            ++entry;
            if (frame_index >= entry->first_frame &&
                frame_index - entry->first_frame < entry->frame_count) {
                return (int) (loaded + 1U);
            }
        }
    }

    while (left < right) {
        uint32_t mid = left + ((right - left) / 2);
        const ChunkIndexEntry *entry = movie->chunk_index + mid;
        if (frame_index < entry->first_frame) {
            right = mid;
        } else if (frame_index - entry->first_frame >= entry->frame_count) {
            left = mid + 1;
        } else {
            return (int) mid;
        }
    }
    return -1;
}

bool decode_h264_frame_with_progress(
    Movie *movie,
    uint32_t frame_index,
    bool blit_output,
    H264FramePublishPredicate predicate,
    H264DecodedFrameHook hook,
    void *userdata
)
{
    int chunk_index = movie_chunk_for_frame(movie, frame_index);
    const ChunkIndexEntry *entry;
    uint32_t local_index;
    uint32_t replay_index;

    if (chunk_index < 0) {
        debug_failf("decode frame=%lu invalid h264 chunk", (unsigned long) frame_index);
        return false;
    }
    if (!load_chunk(movie, chunk_index)) {
        debug_tracef("decode frame=%lu load h264 chunk=%d fail", (unsigned long) frame_index, chunk_index);
        return false;
    }

    entry = movie->chunk_index + chunk_index;
    local_index = frame_index - entry->first_frame;
    if (movie->decoded_local_frame > (int) local_index) {
        uint32_t replay_distance = (uint32_t) (movie->decoded_local_frame - (int) local_index);
        if (debug_should_collect_metrics()) {
            movie->diag_h264_replay_count++;
            movie->diag_h264_replay_frames_total += replay_distance;
            if (replay_distance > movie->diag_h264_replay_max_distance) {
                movie->diag_h264_replay_max_distance = replay_distance;
            }
        }
        debug_tracef(
            "h264 replay frame=%lu chunk=%d local=%lu decoded_local=%d dirty=%u",
            (unsigned long) frame_index,
            chunk_index,
            (unsigned long) local_index,
            movie->decoded_local_frame,
            movie->h264.chunk_dirty ? 1U : 0U
        );
        if (movie->h264.chunk_dirty) {
            if (!load_chunk_from_file(movie, chunk_index, true)) {
                debug_failf("h264 chunk reload failed chunk=%d for replay", chunk_index);
                return false;
            }
            entry = movie->chunk_index + chunk_index;
            local_index = frame_index - entry->first_frame;
        } else if (!reset_h264_decoder(movie)) {
            return false;
        }
        movie->decoded_local_frame = -1;
    }

    for (replay_index = (uint32_t) (movie->decoded_local_frame + 1); replay_index <= local_index; ++replay_index) {
        size_t start = movie->frame_offsets[replay_index];
        size_t end = (replay_index + 1 < entry->frame_count)
            ? movie->frame_offsets[replay_index + 1]
            : movie->chunk_size;
        uint32_t decoded_frame_index = entry->first_frame + replay_index;
        bool is_target_frame = decoded_frame_index == frame_index;
        bool should_publish = hook && (is_target_frame || !predicate || predicate(movie, decoded_frame_index, userdata));
        bool should_blit = blit_output && (is_target_frame || should_publish);

        movie->h264.chunk_dirty = true;
        if (!decode_h264_access_unit(
                movie,
                movie->chunk_bytes + start,
                end - start,
                should_blit)) {
            debug_tracef(
                "h264 frame decode fail frame=%lu chunk=%d local=%lu start=%lu end=%lu size=%lu",
                (unsigned long) decoded_frame_index,
                chunk_index,
                (unsigned long) replay_index,
                (unsigned long) start,
                (unsigned long) end,
                (unsigned long) (end - start)
            );
            return false;
        }
        movie->decoded_local_frame = (int) replay_index;
        if (hook && should_blit && should_publish) {
            movie->current_frame = decoded_frame_index;
            if (!hook(movie, decoded_frame_index, userdata)) {
                return false;
            }
        }
    }

    return true;
}

bool decode_h264_frame(
    Movie *movie,
    uint32_t frame_index,
    bool blit_output
)
{
    return decode_h264_frame_with_progress(movie, frame_index, blit_output, NULL, NULL, NULL);
}

bool decode_mpeg4_frame_with_progress(
    Movie *movie,
    uint32_t frame_index,
    bool blit_output,
    H264FramePublishPredicate predicate,
    H264DecodedFrameHook hook,
    void *userdata
)
{
    int chunk_index = movie_chunk_for_frame(movie, frame_index);
    const ChunkIndexEntry *entry;
    uint32_t local_index;
    uint32_t replay_index;

    if (chunk_index < 0) {
        debug_failf("decode frame=%lu invalid mpeg4 chunk", (unsigned long) frame_index);
        return false;
    }
    if (!load_chunk(movie, chunk_index)) {
        debug_tracef("decode frame=%lu load mpeg4 chunk=%d fail", (unsigned long) frame_index, chunk_index);
        return false;
    }
    if (!movie->mpeg4.decoder) {
        debug_failf("mpeg4 decode failed: decoder missing");
        return false;
    }

    entry = movie->chunk_index + chunk_index;
    local_index = frame_index - entry->first_frame;
    if (movie->decoded_local_frame > (int) local_index) {
        uint32_t replay_distance = (uint32_t) (movie->decoded_local_frame - (int) local_index);
        if (debug_should_collect_metrics()) {
            movie->diag_h264_replay_count++;
            movie->diag_h264_replay_frames_total += replay_distance;
            if (replay_distance > movie->diag_h264_replay_max_distance) {
                movie->diag_h264_replay_max_distance = replay_distance;
            }
        }
        debug_tracef(
            "mpeg4 replay frame=%lu chunk=%d local=%lu decoded_local=%d",
            (unsigned long) frame_index,
            chunk_index,
            (unsigned long) local_index,
            movie->decoded_local_frame
        );
        if (!reset_mpeg4_decoder(movie)) {
            return false;
        }
        movie->decoded_local_frame = -1;
    }

    for (replay_index = (uint32_t) (movie->decoded_local_frame + 1); replay_index <= local_index; ++replay_index) {
        size_t start = movie->frame_offsets[replay_index];
        size_t end = (replay_index + 1 < entry->frame_count)
            ? movie->frame_offsets[replay_index + 1]
            : movie->chunk_size;
        uint32_t decoded_frame_index = entry->first_frame + replay_index;
        bool is_target_frame = decoded_frame_index == frame_index;
        bool should_publish = hook && (is_target_frame || !predicate || predicate(movie, decoded_frame_index, userdata));
        bool should_blit = blit_output && (is_target_frame || should_publish);
        bool discontinuity = movie->mpeg4.discontinuity && replay_index == 0;

        if (end <= start || end > movie->chunk_size) {
            debug_failf(
                "mpeg4 frame bounds invalid frame=%lu chunk=%d local=%lu start=%lu end=%lu size=%lu",
                (unsigned long) decoded_frame_index,
                chunk_index,
                (unsigned long) replay_index,
                (unsigned long) start,
                (unsigned long) end,
                (unsigned long) movie->chunk_size
            );
            return false;
        }
        movie->mpeg4.chunk_dirty = true;
        if (!mpeg4_xvid_decode_frame(
                movie->mpeg4.decoder,
                movie->chunk_bytes + start,
                end - start,
                movie->framebuffer,
                (int) movie->header.video_width,
                (int) movie->header.video_height,
                should_blit,
                discontinuity)) {
            debug_failf(
                "mpeg4 frame decode fail frame=%lu chunk=%d local=%lu: %s",
                (unsigned long) decoded_frame_index,
                chunk_index,
                (unsigned long) replay_index,
                mpeg4_xvid_last_error()
            );
            return false;
        }
        movie->mpeg4.discontinuity = false;
        movie->decoded_local_frame = (int) replay_index;
        if (hook && should_blit && should_publish) {
            movie->current_frame = decoded_frame_index;
            if (!hook(movie, decoded_frame_index, userdata)) {
                return false;
            }
        }
    }

    return true;
}

bool decode_mpeg4_frame(
    Movie *movie,
    uint32_t frame_index,
    bool blit_output
)
{
    return decode_mpeg4_frame_with_progress(movie, frame_index, blit_output, NULL, NULL, NULL);
}

void invalidate_loaded_chunk_state(Movie *movie)
{
    if (!movie) {
        return;
    }
    movie->frame_offsets = NULL;
    movie->chunk_bytes = NULL;
    movie->chunk_size = 0;
    movie->loaded_chunk = -1;
    movie->decoded_local_frame = -1;
    movie->h264.chunk_dirty = false;
    movie->mpeg4.chunk_dirty = false;
    movie->mpeg4.discontinuity = false;
}

bool recover_failed_h264_playback_state(Movie *movie)
{
    bool had_picture_params;
    uint32_t full_width;
    uint32_t full_height;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_width;
    uint32_t crop_height;

    if (!movie || !movie_uses_h264(movie)) {
        return false;
    }

    had_picture_params = movie->h264.headers_ready;
    full_width = movie->h264.full_width;
    full_height = movie->h264.full_height;
    crop_left = movie->h264.crop_left;
    crop_top = movie->h264.crop_top;
    crop_width = movie->h264.crop_width;
    crop_height = movie->h264.crop_height;
    invalidate_loaded_chunk_state(movie);
    if (!reset_h264_decoder(movie)) {
        return false;
    }
    if (had_picture_params) {
        store_h264_picture_params(movie, full_width, full_height, crop_left, crop_top, crop_width, crop_height);
    }
    return true;
}

bool decode_to_frame(Movie *movie, uint32_t frame_index)
{
    if(movie && movie->h264_lookahead){
        if(h264_lookahead_take(movie,frame_index))return true;
        int ahead=h264_lookahead_finish_target(movie,frame_index);
        if(ahead>0)return true;
        h264_lookahead_cancel(movie);
    }
    int chunk_index = movie_chunk_for_frame(movie, frame_index);
    const ChunkIndexEntry *entry;

    if (chunk_index < 0) {
        debug_failf("decode frame=%lu invalid chunk", (unsigned long) frame_index);
        return false;
    }
    if (!load_chunk(movie, chunk_index)) {
        debug_tracef("decode frame=%lu load chunk=%d fail", (unsigned long) frame_index, chunk_index);
        return false;
    }
    entry = movie->chunk_index + chunk_index;
    if (entry->frame_count == 0) {
        debug_failf("decode frame=%lu empty chunk=%d", (unsigned long) frame_index, chunk_index);
        return false;
    }
    if (debug_should_collect_metrics()) {
        movie->diag_foreground_direct_decode_count++;
    }
    if (!movie->codec_ops || !movie->codec_ops->decode_frame) {
        debug_failf("decode frame=%lu missing codec ops", (unsigned long) frame_index);
        return false;
    }
    if (!movie->codec_ops->decode_frame(movie, frame_index, true)) {
        if (!movie_uses_h264(movie)) {
            return false;
        }
        debug_tracef("decode frame=%lu retry after h264 recovery", (unsigned long) frame_index);
        if (recover_failed_h264_playback_state(movie) &&
            movie->codec_ops->decode_frame(movie, frame_index, true)) {
            movie->current_frame = frame_index;
            if(movie->lookahead_enabled && !h264_lookahead_begin(movie))movie->lookahead_enabled=false;
            return true;
        }
        return false;
    }
    movie->current_frame = frame_index;
    if(movie->lookahead_enabled && !h264_lookahead_begin(movie))movie->lookahead_enabled=false;
    return true;
}

bool decode_to_frame_with_progress(
    Movie *movie,
    uint32_t frame_index,
    H264FramePublishPredicate predicate,
    H264DecodedFrameHook hook,
    void *userdata,
    bool *abort_requested
)
{
    int chunk_index;
    const ChunkIndexEntry *entry;
    uint32_t local_index;
    bool continue_loaded_stream;

    if (!hook) {
        return decode_to_frame(movie, frame_index);
    }

    h264_lookahead_cancel(movie);

    chunk_index = movie_chunk_for_frame(movie, frame_index);
    if (chunk_index < 0) {
        debug_failf("progress seek frame=%lu invalid chunk", (unsigned long) frame_index);
        return false;
    }

    entry = movie->chunk_index + chunk_index;
    if (entry->frame_count == 0 || entry->packed_size != entry->unpacked_size) {
        return decode_to_frame(movie, frame_index);
    }
    local_index = frame_index - entry->first_frame;
    continue_loaded_stream = false;
    if (movie->loaded_chunk == chunk_index &&
        movie->frame_offsets != NULL &&
        movie->chunk_bytes != NULL &&
        movie->decoded_local_frame >= -1 &&
        movie->decoded_local_frame < (int) local_index) {
        if (movie_uses_h264(movie)) {
            continue_loaded_stream = movie->h264.decoder != NULL;
        } else if (movie->codec == MOVIE_CODEC_MPEG4) {
            continue_loaded_stream = movie->mpeg4.decoder != NULL;
        }
    }

    if (!continue_loaded_stream) {
        /* Rewinding/equal-frame progress seeks restart at the chunk's IDR.
         * H264 has already removed emulation-prevention bytes in place; a
         * decoder reset clears chunk_dirty, so reload before that reset can
         * hide the need for pristine input. Forward continuation needs none. */
        bool reload_dirty = movie_uses_h264(movie) &&
            movie->loaded_chunk == chunk_index && movie->h264.chunk_dirty;
        bool loaded = reload_dirty ? load_chunk_from_file(movie, chunk_index, true)
            : load_chunk(movie, chunk_index);
        if (!loaded) {
            debug_tracef("progress seek frame=%lu load chunk=%d fail", (unsigned long) frame_index, chunk_index);
            return false;
        }
        if (!movie->codec_ops || !movie->codec_ops->reset || !movie->codec_ops->reset(movie)) {
            return false;
        }
        movie->decoded_local_frame = -1;
    }

    if (debug_should_collect_metrics()) {
        movie->diag_foreground_direct_decode_count++;
    }
    if (movie_uses_h264(movie)) {
        if (!decode_h264_frame_with_progress(movie, frame_index, true, predicate, hook, userdata)) {
            if (abort_requested && *abort_requested) {
                return false;
            }
            debug_tracef("progress seek frame=%lu retry after h264 recovery", (unsigned long) frame_index);
            if (recover_failed_h264_playback_state(movie) &&
                decode_h264_frame_with_progress(movie, frame_index, true, predicate, hook, userdata)) {
                movie->current_frame = frame_index;
                if(movie->lookahead_enabled && !h264_lookahead_begin(movie))movie->lookahead_enabled=false;
                return true;
            }
            if (abort_requested && *abort_requested) {
                return false;
            }
            return false;
        }
    } else if (movie->codec == MOVIE_CODEC_MPEG4) {
        if (!decode_mpeg4_frame_with_progress(movie, frame_index, true, predicate, hook, userdata)) {
            return false;
        }
    } else {
        return decode_to_frame(movie, frame_index);
    }
    movie->current_frame = frame_index;
    if(movie->lookahead_enabled && !h264_lookahead_begin(movie))movie->lookahead_enabled=false;
    return true;
}

bool playback_prepare_ahead(Movie *movie,uint64_t target_ticks,const PointerState *pointer)
{
    if(!h264_lookahead_active(movie))return false;
    uint64_t now=monotonic_clock_now_ticks();
    uint32_t guard=monotonic_clock_ticks_per_second()*2U/1000U;
    if(target_ticks<=now || target_ticks-now<=guard)return false;
    uint64_t started=now;
    playback_capture_ahead_scope(movie,true);
    bool progressed=h264_lookahead_step(movie,target_ticks-guard);
    playback_capture_ahead_scope(movie,false);
    uint64_t ended=monotonic_clock_now_ticks();
    playback_capture_stage(movie,CAPTURE_AHEAD,started,ended);
    if(!progressed && movie_async_enabled(movie) &&
       h264_lookahead_next_frame(movie)<movie->header.frame_count &&
       target_ticks>ended && target_ticks-ended>guard){
        uint32_t spare=monotonic_clock_ticks_to_ms(target_ticks-ended-guard);
        if(spare>4U)spare=4U;
        if(spare){
            prefetch_tick(movie,false,spare,pointer);
            playback_capture_stage(movie,CAPTURE_PREFETCH,ended,monotonic_clock_now_ticks());
        }
    }
    return progressed;
}
