#include "player_internal.h"
#include "h264_lookahead.h"
#include "native_runtime_stats.h"

typedef struct {
    uint16_t *pixels;
    uint8_t *allocation;
    uint32_t frame;
    int chunk;
    uint32_t idr_first, idr_end;
} H264LookaheadFrame;

struct H264Lookahead {
    H264LookaheadFrame frames[H264_LOOKAHEAD_MAX_FRAMES];
    H264LookaheadStats stats;
    size_t frame_bytes;
    size_t storage_bound;
    unsigned head, count, allocated_slots;
    uint32_t prepared_frame;
    unsigned prepared_depth;
    bool have_prepared_frame;
    uint32_t next_frame, tick_hz;
    int idr_chunk;
    uint32_t idr_first, idr_end;
    uint32_t margin_ticks, slice_ceiling_ticks, wide_floor_ticks;
    size_t consumed;
    unsigned zero_advance_retries;
    uint8_t *access_unit;
    size_t access_unit_size;
    uint32_t working_local_frame;
    H264LookaheadFrame *output_slot;
    uint8_t *picture;
    size_t color_row;
    bool color_flat;
    uint32_t pump_guard, finish_guard, color_guard;
    uint32_t pump_ticks_per_mb_q8;
    uint32_t color_ticks_per_row_q8;
    uint32_t color_tail_guard;
    bool decoder_touched;
    bool have_pump_sample, have_finish_sample, have_color_sample;
    bool have_color_tail_sample;
};

static uint32_t ahead_ticks_ms(const struct H264Lookahead *ahead, unsigned ms)
{
    return (uint32_t)(((uint64_t)ahead->tick_hz * ms + 999U) / 1000U);
}

static uint32_t ahead_elapsed(uint64_t begin, uint64_t end)
{
    uint64_t elapsed = end - begin;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

/* A slowly decaying high-water estimate avoids treating one cold-cache sample
 * as a permanent prohibition on using spare time. Lifetime maxima remain in
 * diagnostics. Each conversion band/deblocking operation is indivisible,
 * so its measured cost plus a guard must fit before starting it. */
static void ahead_update_guard(struct H264Lookahead *ahead, uint32_t *guard, bool *have_sample,
                               uint32_t elapsed)
{
    uint32_t margin = elapsed / 8U;
    uint32_t minimum = ahead->margin_ticks;
    uint32_t sample;
    if (margin < minimum)
        margin = minimum;
    sample = elapsed > UINT32_MAX - margin ? UINT32_MAX : elapsed + margin;
    if (!*have_sample) {
        *guard = sample;
        *have_sample = true;
    } else {
        uint32_t decayed = *guard - *guard / 16U;
        *guard = sample > decayed ? sample : decayed;
    }
}

static bool ahead_time_fits(uint64_t deadline, uint32_t needed)
{
    uint64_t now = monotonic_clock_now_ticks();
    return deadline > now && deadline - now >= needed;
}

static uint32_t ahead_four_mb_guard(const struct H264Lookahead *ahead)
{
    uint64_t cost = ((uint64_t)ahead->pump_ticks_per_mb_q8 + 63U) >> 6;
    uint64_t margin = cost / 8U;
    uint32_t minimum = ahead->margin_ticks;
    if (margin < minimum)
        margin = minimum;
    return cost + margin > UINT32_MAX ? UINT32_MAX : (uint32_t)(cost + margin);
}

static unsigned ahead_macroblock_budget(struct H264Lookahead *ahead, uint32_t total_mbs,
                                        uint32_t decoded_mbs, uint64_t deadline)
{
    uint32_t remaining = total_mbs > decoded_mbs ? total_mbs - decoded_mbs : 1U;
    if (remaining <= 8U) {
        if (ahead_time_fits(deadline, ahead->finish_guard))
            return 8U;
        /* Advance part of the final eight MBs only if this cannot complete
         * the picture. The final/deblocking operation keeps its own guard. */
        return remaining > 4U && ahead_time_fits(deadline, ahead_four_mb_guard(ahead)) ? 4U : 0U;
    }
    uint64_t now = monotonic_clock_now_ticks();
    if (deadline <= now)
        return 0U;
    uint64_t available = deadline - now;
    uint32_t margin_floor = ahead->margin_ticks;
    uint32_t slice_ceiling = ahead->slice_ceiling_ticks;
    /* Large batches stay away from the final macroblocks/deblocking. Their
     * guards use measured work per actual decoded MB, not the requested
     * budget (which headers or short slices can exhaust early). */
    for (unsigned budget = 32U; budget >= 16U; budget /= 2U) {
        if (remaining <= budget)
            continue;
        /* Cheap preceding blocks cannot prove the next block is cheap. Keep
         * wider batches out of the final slack even after a very low sample:
         * 32 MB needs at least 8 ms available; 16 MB needs at least 4 ms. */
        if (available < (budget == 32U ? ahead->wide_floor_ticks : ahead->slice_ceiling_ticks))
            continue;
        uint64_t cost = ((uint64_t)ahead->pump_ticks_per_mb_q8 * budget + 255U) >> 8;
        uint64_t margin = cost / 8U;
        if (margin < margin_floor)
            margin = margin_floor;
        if (cost + margin <= slice_ceiling && cost + margin <= available)
            return budget;
    }
    if (available >= ahead->pump_guard)
        return 8U;
    return available >= ahead_four_mb_guard(ahead) ? 4U : 0U;
}

static void ahead_memory_sample(Movie *movie, struct H264Lookahead *ahead, bool beginning)
{
    NativeRuntimeStats memory;
    size_t current = movie_lookahead_storage_bytes(movie);
    size_t growth = ahead->storage_bound > current ? ahead->storage_bound - current : 0U;
    native_runtime_stats_snapshot(&memory);
    ++ahead->stats.memory_checks;
    ahead->stats.memory_known = (memory.valid_flags & NATIVE_RUNTIME_STATS_DYNAMIC_POOL) != 0;
    ahead->stats.free_bytes_last =
        ahead->stats.memory_known ? memory.dynamic_pool_available_bytes : 0U;
    ahead->stats.reserve_bytes = growth > SIZE_MAX - H264_LOOKAHEAD_HEADROOM_BYTES
                                     ? SIZE_MAX
                                     : H264_LOOKAHEAD_HEADROOM_BYTES + growth;
    if (beginning) {
        ahead->stats.memory_known_at_begin = ahead->stats.memory_known;
        ahead->stats.free_bytes_at_begin = ahead->stats.free_bytes_last;
    }
}

static void ahead_select_capacity(Movie *movie, struct H264Lookahead *ahead)
{
    size_t slot_charge =
        ahead->frame_bytes + PLAYER_CACHE_LINE_SIZE + H264_LOOKAHEAD_ALLOCATION_ALLOWANCE;
    uint64_t owned_charge = ahead->stats.allocated_bytes;
    uint64_t budget;
    unsigned capacity;
    ahead_memory_sample(movie, ahead, true);
    if (ahead->storage_bound == SIZE_MAX || ahead->stats.reserve_bytes == SIZE_MAX)
        budget = 0U;
    else if (!ahead->stats.memory_known)
        budget = H264_LOOKAHEAD_FALLBACK_BYTES;
    else {
        /* Retained queue buffers already reduced the free-pool counter.
         * Credit them when choosing a TOTAL queue budget on a later seek. */
        uint64_t available = (uint64_t)ahead->stats.free_bytes_last + owned_charge;
        budget =
            available > ahead->stats.reserve_bytes ? available - ahead->stats.reserve_bytes : 0U;
    }
    if (budget > H264_LOOKAHEAD_MAX_BYTES)
        budget = H264_LOOKAHEAD_MAX_BYTES;
    ahead->stats.budget_bytes = (size_t)budget;
    capacity = budget > sizeof(*ahead) ? (unsigned)((budget - sizeof(*ahead)) / slot_charge) : 0U;
    if (capacity > H264_LOOKAHEAD_MAX_FRAMES)
        capacity = H264_LOOKAHEAD_MAX_FRAMES;
    /* Only begin/rebegin calls this with no queued/partial output. Swapping
     * the display buffer never leaves it owned by a queue slot, so trimming
     * these unused high slots cannot free the currently displayed image. */
    for (unsigned i = capacity; i < ahead->allocated_slots; ++i) {
        player_free_aligned(ahead->frames[i].pixels, ahead->frames[i].allocation);
        memset(&ahead->frames[i], 0, sizeof(ahead->frames[i]));
        ahead->stats.allocated_bytes -= ahead->frame_bytes + PLAYER_CACHE_LINE_SIZE;
    }
    if (ahead->allocated_slots > capacity)
        ahead->allocated_slots = capacity;
    ahead->stats.capacity = capacity;
    if (!capacity)
        ++ahead->stats.memory_denials;
}

static bool ahead_allocate_slot(Movie *movie, struct H264Lookahead *ahead, unsigned slot)
{
    H264LookaheadFrame *frame = &ahead->frames[slot];
    if (frame->pixels)
        return true;
    size_t slot_bytes = ahead->frame_bytes + PLAYER_CACHE_LINE_SIZE;
    size_t slot_charge = slot_bytes + H264_LOOKAHEAD_ALLOCATION_ALLOWANCE;
    ahead_memory_sample(movie, ahead, false);
    bool denied = ahead->storage_bound == SIZE_MAX || ahead->stats.reserve_bytes == SIZE_MAX;
    if (!denied && ahead->stats.memory_known)
        denied = ahead->stats.free_bytes_last < ahead->stats.reserve_bytes ||
                 slot_charge > ahead->stats.free_bytes_last - ahead->stats.reserve_bytes;
    else if (!denied) {
        /* If the validated counter disappears, stop growth beyond the old
         * conservative budget; never discard already queued pictures. */
        uint64_t charge = (uint64_t)ahead->stats.allocated_bytes +
                          (uint64_t)ahead->allocated_slots * H264_LOOKAHEAD_ALLOCATION_ALLOWANCE;
        denied = charge > H264_LOOKAHEAD_FALLBACK_BYTES ||
                 slot_charge > H264_LOOKAHEAD_FALLBACK_BYTES - charge;
    }
    if (denied) {
        ++ahead->stats.memory_denials;
        return false;
    }
    frame->pixels =
        player_malloc_aligned(ahead->frame_bytes, PLAYER_CACHE_LINE_SIZE, &frame->allocation);
    if (!frame->pixels) {
        ++ahead->stats.allocation_failures;
        return false;
    }
    ++ahead->allocated_slots;
    ahead->stats.allocated_bytes += ahead->frame_bytes + PLAYER_CACHE_LINE_SIZE;
    return true;
}

static unsigned ahead_color_rows(struct H264Lookahead *ahead, size_t remaining, uint64_t deadline)
{
    uint64_t now = monotonic_clock_now_ticks();
    if (deadline <= now)
        return 0U;
    uint64_t available = deadline - now;
    uint32_t margin_floor = ahead->margin_ticks;
    uint32_t slice_ceiling = ahead->slice_ceiling_ticks;
    for (unsigned rows = 64U; rows >= 32U; rows /= 2U) {
        if (remaining < rows ||
            available < (rows == 64U ? ahead->wide_floor_ticks : ahead->slice_ceiling_ticks))
            continue;
        uint64_t cost = ((uint64_t)ahead->color_ticks_per_row_q8 * rows + 255U) >> 8;
        uint64_t margin = cost / 8U;
        if (margin < margin_floor)
            margin = margin_floor;
        if (cost + margin <= slice_ceiling && cost + margin <= available)
            return rows;
    }
    uint32_t guard = remaining < H264_LOOKAHEAD_COLOR_ROWS && ahead->have_color_tail_sample
                         ? ahead->color_tail_guard
                         : ahead->color_guard;
    if (available < guard)
        return 0U;
    return remaining < H264_LOOKAHEAD_COLOR_ROWS ? (unsigned)remaining : H264_LOOKAHEAD_COLOR_ROWS;
}

bool h264_lookahead_active(const Movie *movie)
{
    return movie && movie->h264_lookahead && movie->h264_lookahead->stats.active;
}

bool h264_lookahead_begin(Movie *movie)
{
    struct H264Lookahead *ahead;
    uint64_t bytes;
    const ChunkIndexEntry *entry;

    if (h264_lookahead_active(movie))
        return true;
    if (!movie || movie->codec != MOVIE_CODEC_H264 || !movie->h264.decoder ||
        !movie->h264.decoder_initialized || movie->h264.decoder_failed || !movie->framebuffer ||
        !movie->chunk_index || movie->loaded_chunk < 0 ||
        (uint32_t)movie->loaded_chunk >= movie->header.chunk_count ||
        movie->current_frame >= movie->header.frame_count)
        return false;
    entry = &movie->chunk_index[movie->loaded_chunk];
    if (movie->current_frame < entry->first_frame ||
        movie->current_frame - entry->first_frame >= entry->frame_count ||
        movie->decoded_local_frame != (int)(movie->current_frame - entry->first_frame))
        return false;

    bytes = (uint64_t)movie->header.video_width * movie->header.video_height * sizeof(uint16_t);
    if (!bytes || bytes > H264_LOOKAHEAD_MAX_BYTES - sizeof(*ahead) - PLAYER_CACHE_LINE_SIZE)
        return false;
    ahead = movie->h264_lookahead;
    if (!ahead) {
        ahead = calloc(1U, sizeof(*ahead));
        if (!ahead)
            return false;
        ahead->frame_bytes = (size_t)bytes;
        ahead->storage_bound = movie_lookahead_storage_bound(movie);
        ahead->stats.allocated_bytes = sizeof(*ahead);
        ahead->tick_hz = monotonic_clock_ticks_per_second();
        if (!ahead->tick_hz)
            ahead->tick_hz = TIMER_TICKS_PER_SEC;
        /* The clock frequency is fixed for the Movie. Keep ceil-rounded
         * thresholds instead of repeating ARM's 64-bit /1000 helper in each
         * decode/conversion quantum. Guard values and rounding are unchanged. */
        ahead->margin_ticks = ahead_ticks_ms(ahead, 1U);
        ahead->slice_ceiling_ticks = ahead_ticks_ms(ahead, 4U);
        ahead->wide_floor_ticks = ahead_ticks_ms(ahead, 8U);
        ahead->pump_guard = ahead_ticks_ms(ahead, 2U);
        ahead->pump_ticks_per_mb_q8 = ahead->pump_guard * 32U;
        ahead->finish_guard = ahead_ticks_ms(ahead, 6U);
        ahead->color_guard = ahead->margin_ticks;
        ahead->color_ticks_per_row_q8 = ahead->color_guard * 16U;
        movie->h264_lookahead = ahead;
    }
    if (ahead->frame_bytes != bytes)
        return false;
    ahead->head = ahead->count = 0U;
    ahead->have_prepared_frame = false;
    ahead->consumed = 0U;
    ahead->zero_advance_retries = 0U;
    ahead->picture = NULL;
    ahead->access_unit = NULL;
    ahead->output_slot = NULL;
    ahead->color_row = 0U;
    ahead->next_frame = movie->current_frame + 1U;
    ahead->idr_chunk = -1;
    movie->debug_idr_cache_valid =
        movie_h264_idr_bounds(movie, movie->current_frame, &movie->debug_idr_cache_start_local,
                              &movie->debug_idr_cache_end_local);
    movie->debug_idr_cache_chunk = movie->loaded_chunk;
    ahead->decoder_touched = false;
    ahead->stats.partial = false;
    ahead->stats.active = true;
    ahead->stats.queued = 0U;
    ahead_select_capacity(movie, ahead);
    if (!ahead->stats.capacity) {
        ahead->stats.active = false;
        return false;
    }
    return true;
}

/* 1 means work happened, 0 means yield/full/end/not-ready, -1 means failure.
 * A ready DPB picture may survive between calls, but no later access unit is
 * decoded until it has been converted into a queue-owned RGB565 buffer. */
static int ahead_produce(Movie *movie, uint64_t deadline, bool foreground)
{
    struct H264Lookahead *ahead = movie->h264_lookahead;
    H264LookaheadFrame *slot;
    const ChunkIndexEntry *entry;
    uint32_t local, total_mbs, decoded_mbs;
    size_t start, end;
    unsigned tail;
    uint64_t started, finished;
    uint32_t elapsed;
    unsigned macroblock_budget;
    bool picture_ready = false, pending = false;
    uint8_t *picture = NULL;

    if (ahead->count >= ahead->stats.capacity || ahead->next_frame >= movie->header.frame_count)
        return 0;
    if (!foreground) {
        uint32_t initial_guard;
        if (ahead->picture) {
            size_t remaining_rows = movie->header.video_height - ahead->color_row;
            initial_guard =
                remaining_rows < H264_LOOKAHEAD_COLOR_ROWS && ahead->have_color_tail_sample
                    ? ahead->color_tail_guard
                    : ahead->color_guard;
        } else {
            uint32_t small_guard = ahead_four_mb_guard(ahead);
            initial_guard = small_guard < ahead->pump_guard ? small_guard : ahead->pump_guard;
        }
        if (!ahead_time_fits(deadline, initial_guard))
            return 0;
    }
    if (!ahead->access_unit) {
        int chunk = movie_chunk_for_frame(movie, ahead->next_frame);
        int previous_chunk = movie->loaded_chunk;
        int ready;
        if (chunk < 0)
            return -1;
        ready = foreground ? (load_chunk(movie, chunk) ? 1 : -1) : load_ready_chunk(movie, chunk);
        if (!ready) {
            ++ahead->stats.chunk_waits;
            return 0;
        }
        if (ready < 0)
            return -1;
        if (previous_chunk != movie->loaded_chunk)
            ahead->decoder_touched = true;
        if (movie->loaded_chunk < 0 || (uint32_t)movie->loaded_chunk >= movie->header.chunk_count ||
            !movie->frame_offsets || !movie->chunk_bytes || !movie->h264.decoder)
            return -1;
        entry = &movie->chunk_index[movie->loaded_chunk];
        if (ahead->next_frame < entry->first_frame)
            return -1;
        local = ahead->next_frame - entry->first_frame;
        if (local >= entry->frame_count)
            return -1;
        /* Keep presentation metadata with the queued pixels. The compressed
         * chunk can be replaced before these frames reach the screen. */
        if (ahead->idr_chunk != movie->loaded_chunk || local < ahead->idr_first ||
            local >= ahead->idr_end) {
            ahead->idr_chunk = movie->loaded_chunk;
            if (!movie_h264_idr_bounds(movie, ahead->next_frame, &ahead->idr_first,
                                       &ahead->idr_end)) {
                ahead->idr_chunk = -1;
                ahead->idr_first = ahead->idr_end = 0;
            }
        }
        start = movie->frame_offsets[local];
        end =
            local + 1U < entry->frame_count ? movie->frame_offsets[local + 1U] : movie->chunk_size;
        if (end <= start || end > movie->chunk_size)
            return -1;

        tail = (ahead->head + ahead->count) % ahead->stats.capacity;
        if (!foreground && !ahead->frames[tail].pixels) {
            /* Heap growth is not an eight-macroblock operation. Only allocate
             * with the original full conversion reserve still available; keep
             * the cheaper measured pump guard for already allocated slots. */
            uint32_t allocation_guard = ahead->wide_floor_ticks;
            if (ahead->color_guard > allocation_guard)
                allocation_guard = ahead->color_guard;
            if (!ahead_time_fits(deadline, allocation_guard))
                return 0;
        }
        if (!ahead_allocate_slot(movie, ahead, tail)) {
            /* Slots are first allocated in ascending order before the first wrap.
             * Retain every completed frame and continue with the smaller ring. */
            ahead->stats.capacity = ahead->allocated_slots;
            if (!ahead->stats.capacity) {
                h264_lookahead_cancel(movie);
                return 0;
            }
            ahead->head %= ahead->stats.capacity;
            if (ahead->count >= ahead->stats.capacity)
                return 0;
            tail = (ahead->head + ahead->count) % ahead->stats.capacity;
        }
        /* Consuming queue entries advances head and reduces count together,
         * so this unpublished tail slot stays fixed until the AU is complete.
         * The loaded compressed storage cannot change during partial work. */
        ahead->access_unit = movie->chunk_bytes + start;
        ahead->access_unit_size = end - start;
        ahead->working_local_frame = local;
        ahead->output_slot = &ahead->frames[tail];
    }
    slot = ahead->output_slot;

    if (!ahead->picture) {
        total_mbs = h264_incremental_total_mbs(movie, movie->h264.decoder);
        decoded_mbs = ahead->stats.partial ? movie->h264.decoder->slice->numDecodedMbs : 0U;
        /* The final batch can also perform full-picture deblocking and DPB
         * bookkeeping. Reserve its independently measured high-water cost. */
        macroblock_budget =
            foreground ? 0U : ahead_macroblock_budget(ahead, total_mbs, decoded_mbs, deadline);
        if (!foreground && !macroblock_budget)
            return 0;
        ahead->stats.partial = true;
        ahead->decoder_touched = true;
        movie->h264.chunk_dirty = true;
        started = monotonic_clock_now_ticks();
        if (!pump_h264_access_unit(movie, movie->h264.decoder, ahead->access_unit,
                                   ahead->access_unit_size, &ahead->consumed,
                                   &ahead->zero_advance_retries, macroblock_budget, true,
                                   "lookahead", &picture_ready, &pending, &picture))
            return -1;
        finished = monotonic_clock_now_ticks();
        elapsed = ahead_elapsed(started, finished);
        ahead->stats.decode_ticks += elapsed;
        ++ahead->stats.decode_slices;
        if (elapsed > ahead->stats.max_pump_ticks)
            ahead->stats.max_pump_ticks = elapsed;
        if (!foreground) {
            if (macroblock_budget == 32U)
                ++ahead->stats.background_slices_32;
            else if (macroblock_budget == 16U)
                ++ahead->stats.background_slices_16;
            else if (macroblock_budget == 8U)
                ++ahead->stats.background_slices_8;
            else
                ++ahead->stats.background_slices_4;
            if (elapsed > ahead->stats.max_background_pump_ticks)
                ahead->stats.max_background_pump_ticks = elapsed;
            if (picture_ready) {
                uint32_t final_mbs = total_mbs > decoded_mbs ? total_mbs - decoded_mbs : 8U;
                uint64_t normalized = elapsed;
                /* A final four-MB call includes deblocking but less decode
                 * work than a final eight-MB call. Keep one conservative
                 * eight-MB reference instead of learning its shorter time. */
                if (final_mbs < 8U)
                    normalized +=
                        ((uint64_t)ahead->pump_ticks_per_mb_q8 * (8U - final_mbs) + 255U) >> 8;
                ahead_update_guard(ahead, &ahead->finish_guard, &ahead->have_finish_sample,
                                   normalized > UINT32_MAX ? UINT32_MAX : (uint32_t)normalized);
            } else {
                uint32_t after_mbs = movie->h264.decoder->slice->numDecodedMbs;
                if (after_mbs > decoded_mbs) {
                    uint32_t actual_mbs = after_mbs - decoded_mbs;
                    uint64_t normalized, sample_q8;
                    if (actual_mbs == 4U) {
                        normalized = (uint64_t)elapsed << 1;
                        sample_q8 = (uint64_t)elapsed << 6;
                    } else if (actual_mbs == 8U) {
                        normalized = elapsed;
                        sample_q8 = (uint64_t)elapsed << 5;
                    } else if (actual_mbs == 16U) {
                        normalized = ((uint64_t)elapsed + 1U) >> 1;
                        sample_q8 = (uint64_t)elapsed << 4;
                    } else if (actual_mbs == 32U) {
                        normalized = ((uint64_t)elapsed + 3U) >> 2;
                        sample_q8 = (uint64_t)elapsed << 3;
                    } else {
                        normalized = ((uint64_t)elapsed * 8U + actual_mbs - 1U) / actual_mbs;
                        sample_q8 = ((uint64_t)elapsed * 256U + actual_mbs - 1U) / actual_mbs;
                    }
                    uint32_t sample = sample_q8 > UINT32_MAX ? UINT32_MAX : (uint32_t)sample_q8;
                    if (!ahead->have_pump_sample)
                        ahead->pump_ticks_per_mb_q8 = sample;
                    else {
                        uint32_t decayed =
                            ahead->pump_ticks_per_mb_q8 - ahead->pump_ticks_per_mb_q8 / 16U;
                        ahead->pump_ticks_per_mb_q8 = sample > decayed ? sample : decayed;
                    }
                    ahead_update_guard(ahead, &ahead->pump_guard, &ahead->have_pump_sample,
                                       normalized > UINT32_MAX ? UINT32_MAX : (uint32_t)normalized);
                }
            }
        }
        if (!picture_ready)
            return pending ? 1 : -1;
        if (!picture)
            return -1;
        ahead->picture = picture;
        movie->decoded_local_frame = (int)ahead->working_local_frame;
        /* Conversion gets its own quantum: return to input/presentation after
         * this macroblock batch even when there is more deadline slack. */
        if (!foreground)
            return 1;
    }
    size_t rows = movie->header.video_height - ahead->color_row;
    if (!foreground)
        rows = ahead_color_rows(ahead, rows, deadline);
    if (!rows)
        return 0;
    started = monotonic_clock_now_ticks();
    if (!blit_h264_picture_rows_to_target(movie, ahead->picture, slot->pixels,
                                          movie->header.video_width, ahead->color_row, rows,
                                          &ahead->color_flat))
        return -1;
    finished = monotonic_clock_now_ticks();
    elapsed = ahead_elapsed(started, finished);
    ahead->stats.color_ticks += elapsed;
    if (elapsed > ahead->stats.max_color_ticks)
        ahead->stats.max_color_ticks = elapsed;
    if (!foreground && elapsed > ahead->stats.max_background_color_ticks)
        ahead->stats.max_background_color_ticks = elapsed;
    if (!foreground) {
        if (rows == 64U)
            ++ahead->stats.background_color_bands_64;
        else if (rows == 32U)
            ++ahead->stats.background_color_bands_32;
        else if (rows == 16U)
            ++ahead->stats.background_color_bands_16;
        else {
            ++ahead->stats.background_color_bands_tail;
            if (elapsed > ahead->stats.max_background_tail_ticks)
                ahead->stats.max_background_tail_ticks = elapsed;
            /* Band sizes are multiples of 16, so this Movie's final tail
             * always has the same row count. Learn that exact operation
             * separately without reducing the common 1 ms guard margin. */
            ahead_update_guard(ahead, &ahead->color_tail_guard, &ahead->have_color_tail_sample,
                               elapsed);
        }
        /* Short tails contain fixed call/setup work too. Do not magnify it
         * into a per-row estimate for a later full band; the separate tail
         * estimate above measures that fixed operation directly. */
        if (rows >= 16U) {
            uint32_t shift = rows == 64U ? 2U : rows == 32U ? 1U : 0U;
            uint64_t normalized = ((uint64_t)elapsed + ((1U << shift) - 1U)) >> shift;
            uint64_t sample_q8 = (uint64_t)elapsed << (4U - shift);
            uint32_t sample = sample_q8 > UINT32_MAX ? UINT32_MAX : (uint32_t)sample_q8;
            if (!ahead->have_color_sample)
                ahead->color_ticks_per_row_q8 = sample;
            else {
                uint32_t decayed =
                    ahead->color_ticks_per_row_q8 - ahead->color_ticks_per_row_q8 / 16U;
                ahead->color_ticks_per_row_q8 = sample > decayed ? sample : decayed;
            }
            ahead_update_guard(ahead, &ahead->color_guard, &ahead->have_color_sample,
                               (uint32_t)normalized);
        }
    }
    ahead->color_row += rows;
    if (ahead->color_row < movie->header.video_height)
        return 1;
    slot->frame = ahead->next_frame++;
    slot->chunk = ahead->idr_chunk;
    slot->idr_first = ahead->idr_first;
    slot->idr_end = ahead->idr_end;
    ++ahead->count;
    ahead->stats.queued = ahead->count;
    if (ahead->count > ahead->stats.peak_queued)
        ahead->stats.peak_queued = ahead->count;
    if (foreground)
        ++ahead->stats.foreground_frames;
    else
        ++ahead->stats.background_frames;
    ahead->consumed = 0U;
    ahead->zero_advance_retries = 0U;
    ahead->picture = NULL;
    ahead->access_unit = NULL;
    ahead->output_slot = NULL;
    ahead->color_row = 0U;
    ahead->stats.partial = false;
    return 1;
}

bool h264_lookahead_step(Movie *movie, uint64_t deadline_ticks)
{
    struct H264Lookahead *ahead;
    int result;
    if (!h264_lookahead_active(movie))
        return false;
    ahead = movie->h264_lookahead;
    result = ahead_produce(movie, deadline_ticks, false);
    if (result < 0) {
        ++ahead->stats.failures;
        movie->h264.decoder_failed = true;
        h264_lookahead_cancel(movie);
    }
    return result > 0;
}

static bool ahead_take(Movie *movie, uint32_t target_frame, bool queued_hit)
{
    struct H264Lookahead *ahead;
    H264LookaheadFrame *slot;
    uint16_t *old_pixels;
    uint8_t *old_allocation;
    if (!h264_lookahead_active(movie))
        return false;
    ahead = movie->h264_lookahead;
    if (!ahead->count || ahead->frames[ahead->head].frame != target_frame)
        return false;
    slot = &ahead->frames[ahead->head];
    old_pixels = movie->framebuffer;
    old_allocation = movie->framebuffer_allocation;
    movie->framebuffer = slot->pixels;
    movie->framebuffer_allocation = slot->allocation;
    slot->pixels = old_pixels;
    slot->allocation = old_allocation;
    if (movie->frame_surface)
        movie->frame_surface->pixels = movie->framebuffer;
    movie->current_frame = target_frame;
    movie->debug_idr_cache_valid = slot->chunk >= 0 && slot->idr_end > slot->idr_first;
    movie->debug_idr_cache_chunk = slot->chunk;
    movie->debug_idr_cache_start_local = slot->idr_first;
    movie->debug_idr_cache_end_local = slot->idr_end;
    ahead->head = (ahead->head + 1U) % ahead->stats.capacity;
    --ahead->count;
    ahead->stats.queued = ahead->count;
    if (ahead->have_prepared_frame && ahead->prepared_frame == target_frame) {
        if (!ahead->prepared_depth)
            queued_hit = false;
        ahead->have_prepared_frame = false;
    }
    if (queued_hit)
        ++ahead->stats.queue_hits;
    if (!ahead->count && !ahead->stats.partial && ahead->next_frame == target_frame + 1U)
        ahead->decoder_touched = false;
    return true;
}

bool h264_lookahead_take(Movie *movie, uint32_t target_frame)
{
    return ahead_take(movie, target_frame, true);
}

int h264_lookahead_prepare_target(Movie *movie, uint32_t target_frame)
{
    struct H264Lookahead *ahead;
    int result;
    if (!h264_lookahead_active(movie))
        return 0;
    ahead = movie->h264_lookahead;
    if (target_frame != movie->current_frame + 1U)
        return 0;
    if (!ahead->have_prepared_frame || ahead->prepared_frame != target_frame) {
        ahead->prepared_frame = target_frame;
        ahead->prepared_depth = ahead->count;
        ahead->have_prepared_frame = true;
    }
    if (ahead->count && ahead->frames[ahead->head].frame == target_frame)
        return 1;
    if (target_frame != movie->current_frame + 1U || ahead->count ||
        ahead->next_frame != target_frame)
        return 0;
    ++ahead->stats.queue_misses;
    result = ahead_produce(movie, 0U, true);
    if (!ahead->stats.active)
        return 0;
    if (result > 0 && ahead->count && ahead->frames[ahead->head].frame == target_frame)
        return 1;
    ++ahead->stats.failures;
    movie->h264.decoder_failed = true;
    h264_lookahead_cancel(movie);
    return -1;
}

unsigned h264_lookahead_prepared_depth(const Movie *movie, uint32_t target_frame)
{
    const struct H264Lookahead *ahead;
    if (!h264_lookahead_active(movie))
        return 0U;
    ahead = movie->h264_lookahead;
    return ahead->have_prepared_frame && ahead->prepared_frame == target_frame
               ? ahead->prepared_depth
               : ahead->count;
}

int h264_lookahead_finish_target(Movie *movie, uint32_t target_frame)
{
    int ready = h264_lookahead_prepare_target(movie, target_frame);
    if (ready <= 0)
        return ready;
    if (h264_lookahead_take(movie, target_frame))
        return 1;
    return -1;
}

void h264_lookahead_cancel(Movie *movie)
{
    struct H264Lookahead *ahead;
    bool touched;
    if (!movie || !(ahead = movie->h264_lookahead))
        return;
    touched = ahead->decoder_touched;
    if (ahead->stats.active)
        ++ahead->stats.cancellations;
    ahead->stats.active = false;
    ahead->have_prepared_frame = false;
    ahead->stats.partial = false;
    ahead->head = ahead->count = 0U;
    ahead->stats.queued = 0U;
    ahead->consumed = 0U;
    ahead->zero_advance_retries = 0U;
    ahead->picture = NULL;
    ahead->access_unit = NULL;
    ahead->output_slot = NULL;
    ahead->color_row = 0U;
    ahead->decoder_touched = false;
    if (touched) {
        /* The decoder removes emulation-prevention bytes in place. Replaying
         * any partly/fully decoded AU requires a fresh compressed chunk. */
        if (!reset_h264_decoder(movie))
            movie->h264.decoder_failed = true;
        invalidate_loaded_chunk_state(movie);
    }
}

void h264_lookahead_destroy(Movie *movie)
{
    struct H264Lookahead *ahead;
    unsigned i;
    if (!movie || !(ahead = movie->h264_lookahead))
        return;
    for (i = 0U; i < H264_LOOKAHEAD_MAX_FRAMES; ++i) {
        if (ahead->frames[i].pixels)
            player_free_aligned(ahead->frames[i].pixels, ahead->frames[i].allocation);
    }
    free(ahead);
    movie->h264_lookahead = NULL;
}

uint32_t h264_lookahead_next_frame(const Movie *movie)
{
    return h264_lookahead_active(movie) ? movie->h264_lookahead->next_frame
           : movie                      ? movie->current_frame + 1U
                                        : 0U;
}

unsigned h264_lookahead_queued(const Movie *movie)
{
    return h264_lookahead_active(movie) ? movie->h264_lookahead->count : 0U;
}

size_t h264_lookahead_memory_bytes(const Movie *movie)
{
    return movie && movie->h264_lookahead ? movie->h264_lookahead->stats.allocated_bytes : 0U;
}

void h264_lookahead_get_stats(const Movie *movie, H264LookaheadStats *out)
{
    if (!out)
        return;
    if (movie && movie->h264_lookahead) {
        const struct H264Lookahead *ahead = movie->h264_lookahead;
        *out = ahead->stats;
        out->color_tail_guard_ticks =
            ahead->have_color_tail_sample ? ahead->color_tail_guard : ahead->color_guard;
    } else
        memset(out, 0, sizeof(*out));
}
