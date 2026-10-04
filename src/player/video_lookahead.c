#include "player_internal.h"
#include "video_lookahead.h"
#include "native_runtime_stats.h"
#include "frame_view_api.h"

typedef struct {
    uint16_t *pixels;
    uint8_t *allocation;
    uint32_t frame;
    int chunk;
    uint32_t idr_first, idr_end;
} VideoLookaheadFrame;

typedef struct {
    uint8_t *pixels, *allocation;
    uint64_t serial;
    bool repeats_previous;
    uint32_t frame;
    int chunk;
    uint32_t idr_first, idr_end;
} VideoLookaheadPackedFrame;

/* Planar decoder-owned planes are copied before release. Front promotion
 * reads these immutable snapshots independently of any later held picture. */
typedef struct {
    VideoLookaheadPackedFrame frames[VIDEO_LOOKAHEAD_MAX_FRAMES];
    size_t frame_bytes, promote_row;
    const uint16_t *repeat_pixels;
    unsigned head, count, capacity, allocated_slots, rgb_capacity;
    uint32_t copy_guard;
    bool promote_flat, have_copy_sample, checked_layout;
} VideoLookaheadPacked;

#define VIDEO_LOOKAHEAD_RGB_FRONT 4U

struct VideoLookahead {
    VideoLookaheadFrame frames[VIDEO_LOOKAHEAD_MAX_FRAMES];
    VideoLookaheadPacked *packed;
    VideoLookaheadStats stats;
    size_t frame_bytes;
    size_t storage_bound;
    unsigned head, count, allocated_slots;
    uint32_t prepared_frame;
    unsigned prepared_depth;
    bool have_prepared_frame;
    bool realtime_pending;
    uint32_t realtime_target;
    uint32_t next_frame, tick_hz;
    int idr_chunk;
    uint32_t idr_first, idr_end;
    uint32_t margin_ticks, slice_ceiling_ticks, wide_floor_ticks, foreground_slice_ticks;
    size_t consumed;
    unsigned zero_advance_retries;
    uint8_t *access_unit;
    size_t access_unit_size;
    uint32_t working_local_frame;
    VideoLookaheadFrame *output_slot;
    uint8_t *picture;
    size_t color_row;
    bool color_flat;
    const uint16_t *repeat_pixels;
    uint64_t previous_rgb_serial;
    uint32_t previous_rgb_frame;
    bool have_previous_rgb;
    uint32_t pump_guard, finish_guard, color_guard;
    uint32_t start_guard[2];
    bool have_start_sample[2];
    uint32_t pump_ticks_per_mb_q8;
    uint32_t color_ticks_per_row_q8;
    uint32_t color_tail_guard;
    bool decoder_touched;
    bool recovering, producer_failed;
    int recovery_chunk;
    size_t recovery_offset;
    uint32_t recovery_target;
    const char *failure_stage;
    bool have_pump_sample, have_finish_sample, have_color_sample;
    bool have_color_tail_sample;
    bool packed_layout_rejected;
    bool output_packed; /* Chosen at AU begin; held-picture output never switches tiers. */
};

static inline bool ahead_has_packed(const struct VideoLookahead *ahead)
{
    return (NDVIDEO_WITH_H264 || NDVIDEO_WITH_HEVC || NDVIDEO_WITH_AV1) && ahead->packed;
}

static inline bool ahead_output_packed(const struct VideoLookahead *ahead)
{
    return ahead_has_packed(ahead) && ahead->output_packed;
}

static bool ahead_should_pack(const struct VideoLookahead *ahead)
{
    return ahead_has_packed(ahead) &&
        (ahead->packed->count || ahead->count >= ahead->packed->rgb_capacity);
}

static unsigned ahead_rgb_capacity(const struct VideoLookahead *ahead)
{
    return ahead_has_packed(ahead) ? ahead->packed->rgb_capacity : ahead->stats.capacity;
}

static unsigned ahead_total_queued(const struct VideoLookahead *ahead)
{
    return ahead->count + (ahead_has_packed(ahead) ? ahead->packed->count : 0U);
}

static void ahead_queue_stats(struct VideoLookahead *ahead)
{
    ahead->stats.rgb_ready = ahead->count;
    ahead->stats.packed_ready = ahead_has_packed(ahead) ? ahead->packed->count : 0U;
    ahead->stats.packed_capacity = ahead_has_packed(ahead) ? ahead->packed->capacity : 0U;
    ahead->stats.queued = ahead_total_queued(ahead);
    if (ahead->stats.queued > ahead->stats.peak_queued)
        ahead->stats.peak_queued = ahead->stats.queued;
}

static void ahead_free_packed(struct VideoLookahead *ahead)
{
    if (!ahead_has_packed(ahead)) return;
    VideoLookaheadPacked *packed = ahead->packed;
    for (unsigned i = 0; i < packed->allocated_slots; ++i) {
        player_free_aligned(packed->frames[i].pixels, packed->frames[i].allocation);
        ahead->stats.allocated_bytes -= packed->frame_bytes + PLAYER_CACHE_LINE_SIZE;
    }
    ahead->stats.allocated_bytes -= sizeof(*packed);
    free(packed);
    ahead->packed = NULL;
}

static uint32_t ahead_ticks_ms(const struct VideoLookahead *ahead, unsigned ms)
{
    return (uint32_t)(((uint64_t)ahead->tick_hz * ms + 999U) / 1000U);
}

static uint32_t ahead_elapsed(uint64_t begin, uint64_t end)
{
    uint64_t elapsed = end - begin;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void ahead_capture_planar_pump(VideoLookaheadStats *stats, uint32_t elapsed,
                                     uint32_t before_mbs, uint32_t after_mbs,
                                     unsigned unit_mbs, bool submission)
{
    /* Planar progress is expressed in whole CTUs/SBs, scaled to equivalent
     * 16x16 units by video_decoder_done_units(). Submission resets progress,
     * so its before_mbs is zero even if the preceding picture completed. */
    uint32_t decoded_mbs = after_mbs >= before_mbs ? after_mbs - before_mbs : 0U;
    unsigned units = decoded_mbs / unit_mbs;
    if (!stats->captured_planar_pumps || elapsed > stats->captured_max_pump_ticks) {
        stats->captured_max_pump_ticks = elapsed;
        stats->captured_max_pump_units = units;
        stats->captured_max_pump_unit_mbs = unit_mbs;
        stats->captured_max_pump_submission = submission;
    }
    ++stats->captured_planar_pumps;
    if (submission) {
        if (!stats->captured_submission_pumps || elapsed > stats->captured_max_submission_ticks) {
            stats->captured_max_submission_ticks = elapsed;
            stats->captured_max_submission_units = units;
        }
        ++stats->captured_submission_pumps;
    } else if (units == 1U) {
        ++stats->captured_single_no_submit_pumps;
        if (elapsed > stats->captured_max_single_no_submit_ticks)
            stats->captured_max_single_no_submit_ticks = elapsed;
    } else if (units > 1U) {
        ++stats->captured_multi_no_submit_pumps;
        if (elapsed > stats->captured_max_multi_no_submit_ticks)
            stats->captured_max_multi_no_submit_ticks = elapsed;
    }
}

/* A slowly decaying high-water estimate avoids treating one cold-cache sample
 * as a permanent prohibition on using spare time. Lifetime maxima remain in
 * diagnostics. Each conversion band/deblocking operation is indivisible,
 * so its measured cost plus a guard must fit before starting it. */
static void ahead_update_guard(struct VideoLookahead *ahead, uint32_t *guard, bool *have_sample,
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

static uint32_t ahead_four_mb_guard(const struct VideoLookahead *ahead)
{
    uint64_t cost = ((uint64_t)ahead->pump_ticks_per_mb_q8 + 63U) >> 6;
    uint64_t margin = cost / 8U;
    uint32_t minimum = ahead->margin_ticks;
    if (margin < minimum)
        margin = minimum;
    return cost + margin > UINT32_MAX ? UINT32_MAX : (uint32_t)(cost + margin);
}

static unsigned ahead_macroblock_budget(struct VideoLookahead *ahead, uint32_t total_mbs,
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

static void ahead_memory_sample(Movie *movie, struct VideoLookahead *ahead, bool beginning)
{
    NativeRuntimeStats memory;
    size_t current = movie_lookahead_storage_bytes(movie);
    size_t growth = ahead->storage_bound > current ? ahead->storage_bound - current : 0U;
    native_runtime_stats_snapshot(&memory);
    ++ahead->stats.memory_checks;
    ahead->stats.memory_known = (memory.valid_flags & NATIVE_RUNTIME_STATS_DYNAMIC_POOL) != 0;
    ahead->stats.free_bytes_last =
        ahead->stats.memory_known ? memory.dynamic_pool_available_bytes : 0U;
    ahead->stats.reserve_bytes = growth > SIZE_MAX - VIDEO_LOOKAHEAD_HEADROOM_BYTES
                                     ? SIZE_MAX
                                     : VIDEO_LOOKAHEAD_HEADROOM_BYTES + growth;
    if (beginning) {
        ahead->stats.memory_known_at_begin = ahead->stats.memory_known;
        ahead->stats.free_bytes_at_begin = ahead->stats.free_bytes_last;
    }
}

static void ahead_select_capacity(Movie *movie, struct VideoLookahead *ahead)
{
    size_t slot_charge =
        ahead->frame_bytes + PLAYER_CACHE_LINE_SIZE + VIDEO_LOOKAHEAD_ALLOCATION_ALLOWANCE;
    uint64_t owned_charge = ahead->stats.allocated_bytes;
    uint64_t budget;
    unsigned capacity;
    ahead_memory_sample(movie, ahead, true);
    if (ahead->storage_bound == SIZE_MAX || ahead->stats.reserve_bytes == SIZE_MAX)
        budget = 0U;
    else if (!ahead->stats.memory_known)
        budget = VIDEO_LOOKAHEAD_FALLBACK_BYTES;
    else {
        /* Retained queue buffers already reduced the free-pool counter.
         * Credit them when choosing a TOTAL queue budget on a later seek. */
        uint64_t available = (uint64_t)ahead->stats.free_bytes_last + owned_charge;
        budget =
            available > ahead->stats.reserve_bytes ? available - ahead->stats.reserve_bytes : 0U;
    }
    if (budget > VIDEO_LOOKAHEAD_MAX_BYTES)
        budget = VIDEO_LOOKAHEAD_MAX_BYTES;
    ahead->stats.budget_bytes = (size_t)budget;
    if (ahead_has_packed(ahead)) {
        VideoLookaheadPacked *packed = ahead->packed;
        uint64_t fixed = sizeof(*ahead) + sizeof(*packed) +
                         VIDEO_LOOKAHEAD_RGB_FRONT * (uint64_t)slot_charge;
        size_t packed_charge = packed->frame_bytes + PLAYER_CACHE_LINE_SIZE +
                               VIDEO_LOOKAHEAD_ALLOCATION_ALLOWANCE;
        unsigned packed_capacity = budget > fixed
            ? (unsigned)((budget - fixed) / packed_charge) : 0U;
        if (packed_capacity > VIDEO_LOOKAHEAD_MAX_FRAMES - VIDEO_LOOKAHEAD_RGB_FRONT)
            packed_capacity = VIDEO_LOOKAHEAD_MAX_FRAMES - VIDEO_LOOKAHEAD_RGB_FRONT;
        if (!packed_capacity) {
            ahead_free_packed(ahead);
        } else {
            for (unsigned i = packed_capacity; i < packed->allocated_slots; ++i) {
                player_free_aligned(packed->frames[i].pixels, packed->frames[i].allocation);
                memset(&packed->frames[i], 0, sizeof(packed->frames[i]));
                ahead->stats.allocated_bytes -= packed->frame_bytes + PLAYER_CACHE_LINE_SIZE;
            }
            if (packed->allocated_slots > packed_capacity)
                packed->allocated_slots = packed_capacity;
            packed->capacity = packed_capacity;
            packed->rgb_capacity = VIDEO_LOOKAHEAD_RGB_FRONT;
        }
    }
    capacity = ahead_has_packed(ahead) ? ahead->packed->rgb_capacity :
        (budget > sizeof(*ahead) ? (unsigned)((budget - sizeof(*ahead)) / slot_charge) : 0U);
    if (capacity > VIDEO_LOOKAHEAD_MAX_FRAMES)
        capacity = VIDEO_LOOKAHEAD_MAX_FRAMES;
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
    ahead->stats.capacity = capacity + (ahead_has_packed(ahead) ? ahead->packed->capacity : 0U);
    if (!capacity)
        ++ahead->stats.memory_denials;
    ahead_queue_stats(ahead);
}

static bool ahead_allocate_slot(Movie *movie, struct VideoLookahead *ahead, unsigned slot)
{
    VideoLookaheadFrame *frame = &ahead->frames[slot];
    if (frame->pixels)
        return true;
    size_t slot_bytes = ahead->frame_bytes + PLAYER_CACHE_LINE_SIZE;
    size_t slot_charge = slot_bytes + VIDEO_LOOKAHEAD_ALLOCATION_ALLOWANCE;
    ahead_memory_sample(movie, ahead, false);
    bool denied = ahead->storage_bound == SIZE_MAX || ahead->stats.reserve_bytes == SIZE_MAX;
    if (!denied && ahead->stats.memory_known)
        denied = ahead->stats.free_bytes_last < ahead->stats.reserve_bytes ||
                 slot_charge > ahead->stats.free_bytes_last - ahead->stats.reserve_bytes;
    else if (!denied) {
        /* If the validated counter disappears, stop growth beyond the old
         * conservative budget; never discard already queued pictures. */
        uint64_t charge = (uint64_t)ahead->stats.allocated_bytes +
                          (uint64_t)(ahead->allocated_slots +
                              (ahead_has_packed(ahead) ? ahead->packed->allocated_slots : 0U)) *
                          VIDEO_LOOKAHEAD_ALLOCATION_ALLOWANCE;
        denied = charge > VIDEO_LOOKAHEAD_FALLBACK_BYTES ||
                 slot_charge > VIDEO_LOOKAHEAD_FALLBACK_BYTES - charge;
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

static bool ahead_allocate_packed_slot(Movie *movie, struct VideoLookahead *ahead, unsigned slot)
{
    VideoLookaheadPacked *packed = ahead->packed;
    VideoLookaheadPackedFrame *frame = &packed->frames[slot];
    if (frame->pixels) return true;
    size_t bytes = packed->frame_bytes + PLAYER_CACHE_LINE_SIZE;
    size_t charge = bytes + VIDEO_LOOKAHEAD_ALLOCATION_ALLOWANCE;
    ahead_memory_sample(movie, ahead, false);
    bool denied = ahead->storage_bound == SIZE_MAX || ahead->stats.reserve_bytes == SIZE_MAX;
    if (!denied && ahead->stats.memory_known)
        denied = ahead->stats.free_bytes_last < ahead->stats.reserve_bytes ||
                 charge > ahead->stats.free_bytes_last - ahead->stats.reserve_bytes;
    else if (!denied) {
        uint64_t used = (uint64_t)ahead->stats.allocated_bytes +
            (ahead->allocated_slots + packed->allocated_slots) * VIDEO_LOOKAHEAD_ALLOCATION_ALLOWANCE;
        denied = used > VIDEO_LOOKAHEAD_FALLBACK_BYTES ||
                 charge > VIDEO_LOOKAHEAD_FALLBACK_BYTES - used;
    }
    if (denied) { ++ahead->stats.memory_denials; return false; }
    frame->pixels = player_malloc_aligned(packed->frame_bytes, PLAYER_CACHE_LINE_SIZE,
                                         &frame->allocation);
    if (!frame->pixels) { ++ahead->stats.allocation_failures; return false; }
    ++packed->allocated_slots;
    ahead->stats.allocated_bytes += bytes;
    return true;
}

static unsigned ahead_color_rows(struct VideoLookahead *ahead, size_t remaining, uint64_t deadline)
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
    uint32_t guard = remaining < VIDEO_LOOKAHEAD_COLOR_ROWS && ahead->have_color_tail_sample
                         ? ahead->color_tail_guard
                         : ahead->color_guard;
    if (available < guard)
        return 0U;
    return remaining < VIDEO_LOOKAHEAD_COLOR_ROWS ? (unsigned)remaining : VIDEO_LOOKAHEAD_COLOR_ROWS;
}

bool video_lookahead_active(const Movie *movie)
{
    return movie && movie->video_lookahead && movie->video_lookahead->stats.active;
}

/* An empty planar reserve can use the existing bounded foreground quantum
 * immediately. Limit this to a ready compressed chunk: admitting a synchronous
 * read or recovery earlier is a separate scheduling decision. */
bool video_lookahead_can_prepare_early(const Movie *movie)
{
    if (!video_lookahead_active(movie) || !movie_uses_planar_decoder(movie))
        return false;
    const struct VideoLookahead *ahead = movie->video_lookahead;
    uint32_t target = movie->current_frame + 1U;
    if (target >= movie->header.frame_count || ahead_total_queued(ahead) ||
        ahead->recovering || ahead->producer_failed || ahead->realtime_pending ||
        ahead->next_frame != target ||
        movie->loaded_chunk < 0 || (uint32_t)movie->loaded_chunk >= movie->header.chunk_count)
        return false;
    const ChunkIndexEntry *entry = &movie->chunk_index[movie->loaded_chunk];
    return target >= entry->first_frame && target - entry->first_frame < entry->frame_count;
}

bool video_lookahead_begin(Movie *movie)
{
    struct VideoLookahead *ahead;
    uint64_t bytes;
    const ChunkIndexEntry *entry;

    if (video_lookahead_active(movie))
        return true;
    if (!movie || !video_decoder_ready(movie) || !movie->framebuffer ||
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
    if (!bytes || bytes > VIDEO_LOOKAHEAD_MAX_BYTES - sizeof(*ahead) - PLAYER_CACHE_LINE_SIZE)
        return false;
    ahead = movie->video_lookahead;
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
        /* A late HEVC frame has no presentation slack left. Up to 8 ms of
         * useful work per input turn avoids repeating the full UI scan for
         * every CTU, while still checking keys more than once per UI frame. */
        ahead->foreground_slice_ticks = ahead_ticks_ms(ahead, 8U);
        ahead->pump_guard = ahead_ticks_ms(ahead, 2U);
        ahead->pump_ticks_per_mb_q8 = ahead->pump_guard * 32U;
        ahead->finish_guard = ahead_ticks_ms(ahead, 6U);
        ahead->start_guard[0] = ahead->pump_guard;
        ahead->start_guard[1] = ahead->wide_floor_ticks;
        ahead->color_guard = ahead->margin_ticks;
        ahead->color_ticks_per_row_q8 = ahead->color_guard * 16U;
        movie->video_lookahead = ahead;
    }
    if (ahead->frame_bytes != bytes)
        return false;
    if (!ahead_has_packed(ahead) && !ahead->packed_layout_rejected &&
        movie_uses_decode_ahead(movie) &&
        !(movie->header.video_width & 7U) && !(movie->header.video_height & 1U)) {
        ahead->packed = calloc(1U, sizeof(*ahead->packed));
        if (ahead_has_packed(ahead)) {
            ahead->packed->frame_bytes = (size_t)(bytes / 2U * 3U / 2U);
            ahead->packed->copy_guard = ahead->margin_ticks;
            ahead->stats.allocated_bytes += sizeof(*ahead->packed);
        }
    }
    if (ahead_has_packed(ahead)) {
        ahead->packed->head = ahead->packed->count = 0U;
        ahead->packed->promote_row = 0U;
        ahead->packed->checked_layout = false;
    }
    ahead->head = ahead->count = 0U;
    ahead->repeat_pixels = NULL;
    ahead->have_previous_rgb = false;
    if (ahead_has_packed(ahead)) ahead->packed->repeat_pixels = NULL;
    ahead->recovering = ahead->producer_failed = false;
    ahead->recovery_chunk = -1;
    ahead->have_prepared_frame = false;
    ahead->realtime_pending = false;
    movie->foreground_pending_ticks = 0;
    ahead->consumed = 0U;
    ahead->zero_advance_retries = 0U;
    ahead->picture = NULL;
    ahead->access_unit = NULL;
    ahead->output_slot = NULL;
    ahead->output_packed = false;
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
    ahead_queue_stats(ahead);
    ahead_select_capacity(movie, ahead);
    if (!ahead->stats.capacity) {
        ahead->stats.active = false;
        return false;
    }
    return true;
}

bool video_lookahead_reloading(const Movie *movie)
{
    return video_lookahead_active(movie) && movie->video_lookahead->recovery_chunk >= 0;
}

static void ahead_note_failure(Movie *movie, struct VideoLookahead *ahead)
{
    ++ahead->stats.failures;
    if (ahead->recovering) return; /* Retain the original decoder failure if recovery also fails. */
    ahead->stats.failed_frame = ahead->next_frame;
    ahead->stats.failure_visible_frame = movie->current_frame;
    ahead->stats.failure_chunk = movie->loaded_chunk;
    ahead->stats.failure_queued = ahead_total_queued(ahead);
    snprintf(ahead->stats.failure_reason, sizeof(ahead->stats.failure_reason), "%s: %s",
        ahead->failure_stage ? ahead->failure_stage : "unknown", debug_last_error());
}

static void ahead_recover(Movie *movie, struct VideoLookahead *ahead)
{
    ahead_note_failure(movie, ahead);
    ahead->repeat_pixels = NULL;
    ahead->have_previous_rgb = false;
    if (ahead_has_packed(ahead)) ahead->packed->repeat_pixels = NULL;
    int chunk = movie_chunk_for_frame(movie, ahead->next_frame);
    /* Never retry indefinitely. Keep valid RGB frames even if rebuilding the
     * decoder fails; the existing foreground fallback handles their end. */
    if (ahead->recovering || chunk < 0 || !video_decoder_reset(movie)) {
        ahead->producer_failed = true;
        ahead->recovery_chunk = -1;
        return;
    }
    ahead->recovering = true;
    ahead->recovery_target = ahead->next_frame;
    const ChunkIndexEntry *entry = &movie->chunk_index[chunk];
    /* The decoder was fully reset. Start at the chunk's parameter sets,
     * rather than assuming an interior IDR repeats SPS/PPS headers. */
    ahead->next_frame = entry->first_frame;
    ahead->recovery_chunk = chunk;
    ahead->recovery_offset = 0;
    ahead->consumed = 0;
    ahead->zero_advance_retries = 0;
    ahead->access_unit = NULL;
    ahead->picture = NULL;
    ahead->output_slot = NULL;
    ahead->output_packed = false;
    ahead->color_row = 0;
    ahead->stats.partial = false;
    ahead->decoder_touched = true;
    invalidate_loaded_chunk_state(movie);
    debug_tracef("lookahead recovery visible=%lu target=%lu restart=%lu queued=%u reason=%s",
        (unsigned long)movie->current_frame, (unsigned long)ahead->recovery_target,
        (unsigned long)ahead->next_frame, ahead->count, ahead->stats.failure_reason);
}

static void ahead_discard_picture(Movie *movie, struct VideoLookahead *ahead)
{
    video_decoder_release_picture(movie);
    ahead->repeat_pixels = NULL;
    ahead->have_previous_rgb = false;
    if (ahead_has_packed(ahead)) ahead->packed->repeat_pixels = NULL;
    movie->decoded_local_frame = (int)ahead->working_local_frame;
    if (ahead->recovering && ahead->next_frame < ahead->recovery_target)
        ++ahead->stats.recovery_frames;
    ++ahead->next_frame;
    ahead->consumed = 0;
    ahead->zero_advance_retries = 0;
    ahead->access_unit = NULL;
    ahead->picture = NULL;
    ahead->output_slot = NULL;
    ahead->output_packed = false;
    ahead->color_row = 0;
    ahead->stats.partial = false;
}

/* Publish only a fully owned snapshot. The producer and promoter have separate
 * row cursors, so a held next picture cannot change a snapshot being colored. */
static int ahead_pack_picture(Movie *movie, uint64_t deadline, bool foreground)
{
    struct VideoLookahead *ahead = movie->video_lookahead;
    VideoLookaheadPacked *packed = ahead->packed;
    unsigned tail = (packed->head + packed->count) % packed->capacity;
    VideoLookaheadPackedFrame *slot = &packed->frames[tail];
    size_t rows = movie->header.video_height - ahead->color_row;
    if (rows > 16U) rows = 16U;
    if (!foreground && !ahead_time_fits(deadline, packed->copy_guard)) return 0;
    VideoFrame view;
    if (!video_decoder_get_frame_view(movie, ahead->picture, &view)) return -1;
    uint64_t started = monotonic_clock_now_ticks();
    ahead->failure_stage = "packed copy";
    if (!video_frame_pack_rows(&view, slot->pixels, packed->frame_bytes,
                              (unsigned)ahead->color_row, (unsigned)rows)) return -1;
    uint32_t elapsed = ahead_elapsed(started, monotonic_clock_now_ticks());
    ahead->stats.packed_copy_ticks += elapsed;
    ++ahead->stats.packed_copy_bands;
    if (elapsed > ahead->stats.max_packed_copy_ticks)
        ahead->stats.max_packed_copy_ticks = elapsed;
    ahead_update_guard(ahead, &packed->copy_guard, &packed->have_copy_sample, elapsed);
    ahead->color_row += rows;
    if (ahead->color_row < movie->header.video_height) return 1;
    slot->frame = ahead->next_frame;
    slot->chunk = ahead->idr_chunk;
    slot->idr_first = ahead->idr_first;
    slot->idr_end = ahead->idr_end;
    slot->serial = 0;
    slot->repeats_previous = false;
    if (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1) {
        slot->serial = av1_frame_serial(movie->av1.decoder);
        slot->repeats_previous = av1_frame_repeats_previous(movie->av1.decoder);
    }
    video_decoder_release_picture(movie);
    if (ahead->recovering) { ahead->recovering = false; ++ahead->stats.recoveries; }
    ++ahead->next_frame;
    ++packed->count;
    ++ahead->stats.packed_frames;
    if (foreground) ++ahead->stats.foreground_frames;
    else ++ahead->stats.background_frames;
    ahead->consumed = 0;
    ahead->zero_advance_retries = 0;
    ahead->picture = NULL;
    ahead->access_unit = NULL;
    ahead->output_slot = NULL;
    ahead->output_packed = false;
    ahead->color_row = 0;
    ahead->stats.partial = false;
    ahead_queue_stats(ahead);
    return 1;
}

/* A certified repeat can use only the immediately preceding prepared image.
 * Its pixels may move from the queue into the display while copying, but cannot
 * be recycled until this next picture is ready. Seek/discard clears this view. */
static const uint16_t *ahead_repeat_source(const Movie *movie, const struct VideoLookahead *ahead,
                                           uint32_t frame, uint64_t serial, bool repeats_previous)
{
    if (!NDVIDEO_WITH_AV1 || movie->codec != MOVIE_CODEC_AV1 || !ahead->have_previous_rgb ||
        ahead->previous_rgb_frame + 1U != frame ||
        !repeats_previous ||
        serial != ahead->previous_rgb_serial + 1U)
        return NULL;
    if (ahead->count) {
        unsigned tail = (ahead->head + ahead->count - 1U) % ahead_rgb_capacity(ahead);
        return ahead->frames[tail].frame == ahead->previous_rgb_frame
                   ? ahead->frames[tail].pixels : NULL;
    }
    return movie->current_frame == ahead->previous_rgb_frame ? movie->framebuffer : NULL;
}

static int ahead_promote_packed(Movie *movie, uint64_t deadline, bool foreground)
{
    struct VideoLookahead *ahead = movie->video_lookahead;
    if (!ahead_has_packed(ahead)) return 0;
    VideoLookaheadPacked *packed = ahead->packed;
    if (!packed->count || ahead->count >= packed->rgb_capacity) return 0;
    unsigned tail = (ahead->head + ahead->count) % packed->rgb_capacity;
    if (!ahead->frames[tail].pixels) {
        if (!foreground && !ahead_time_fits(deadline, ahead->wide_floor_ticks)) return 0;
        if (!ahead_allocate_slot(movie, ahead, tail)) {
            if (!ahead->allocated_slots) { video_lookahead_cancel(movie); return 0; }
            packed->rgb_capacity = ahead->allocated_slots;
            ahead->stats.capacity = packed->rgb_capacity + packed->capacity;
            ahead->head %= packed->rgb_capacity;
            if (ahead->count >= packed->rgb_capacity) return 0;
            tail = (ahead->head + ahead->count) % packed->rgb_capacity;
        }
    }
    size_t rows = movie->header.video_height - packed->promote_row;
    if (foreground && movie_uses_planar_decoder(movie) && rows > 16U) rows = 16U;
    if (!foreground) rows = ahead_color_rows(ahead, rows, deadline);
    if (!rows) return 0;
    VideoFrame view;
    VideoLookaheadPackedFrame *source = &packed->frames[packed->head];
    VideoLookaheadFrame *slot = &ahead->frames[tail];
    if (!packed->promote_row) {
        /* A repeat can switch to ordinary conversion after producer recovery.
         * It has not run the row-zero flat probe, so keep that fallback general. */
        packed->promote_flat = false;
        packed->repeat_pixels = ahead_repeat_source(movie, ahead, source->frame,
                                                    source->serial, source->repeats_previous);
    }
    if (!video_frame_packed_view(source->pixels, packed->frame_bytes,
                                movie->header.video_width, movie->header.video_height, &view)) return -1;
    uint64_t started = monotonic_clock_now_ticks();
    ahead->failure_stage = "packed color";
    if (packed->repeat_pixels) {
        size_t offset = packed->promote_row * movie->header.video_width;
        size_t bytes = rows * movie->header.video_width * sizeof(uint16_t);
        player_copy_maybe_fast(slot->pixels + offset, packed->repeat_pixels + offset, bytes);
    } else if (!blit_planar_picture_rows(movie, &view, slot->pixels, movie->header.video_width,
                                        packed->promote_row, rows, &packed->promote_flat)) return -1;
    uint64_t finished = monotonic_clock_now_ticks();
    uint32_t elapsed = ahead_elapsed(started, finished);
    if (packed->repeat_pixels)
        playback_capture_stage(movie, CAPTURE_COLOR, started, finished);
    ahead->stats.color_ticks += elapsed;
    if (elapsed > ahead->stats.max_color_ticks) ahead->stats.max_color_ticks = elapsed;
    if (!foreground && elapsed > ahead->stats.max_background_color_ticks)
        ahead->stats.max_background_color_ticks = elapsed;
    if (!foreground) {
        if (rows == 64U) ++ahead->stats.background_color_bands_64;
        else if (rows == 32U) ++ahead->stats.background_color_bands_32;
        else if (rows == 16U) ++ahead->stats.background_color_bands_16;
        else {
            ++ahead->stats.background_color_bands_tail;
            if (elapsed > ahead->stats.max_background_tail_ticks) ahead->stats.max_background_tail_ticks = elapsed;
            if (!packed->repeat_pixels)
                ahead_update_guard(ahead, &ahead->color_tail_guard, &ahead->have_color_tail_sample, elapsed);
        }
        if (rows >= 16U && !packed->repeat_pixels) {
            unsigned shift = rows == 64U ? 2U : rows == 32U ? 1U : 0U;
            uint64_t normalized = ((uint64_t)elapsed + ((1U << shift) - 1U)) >> shift;
            uint64_t sample_q8 = (uint64_t)elapsed << (4U - shift);
            uint32_t sample = sample_q8 > UINT32_MAX ? UINT32_MAX : (uint32_t)sample_q8;
            if (!ahead->have_color_sample) ahead->color_ticks_per_row_q8 = sample;
            else {
                uint32_t decayed = ahead->color_ticks_per_row_q8 - ahead->color_ticks_per_row_q8 / 16U;
                ahead->color_ticks_per_row_q8 = sample > decayed ? sample : decayed;
            }
            ahead_update_guard(ahead, &ahead->color_guard, &ahead->have_color_sample, (uint32_t)normalized);
        }
    }
    packed->promote_row += rows;
    if (packed->promote_row < movie->header.video_height) return 1;
    slot->frame = source->frame;
    slot->chunk = source->chunk;
    slot->idr_first = source->idr_first;
    slot->idr_end = source->idr_end;
    if (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1) {
        ahead->previous_rgb_serial = source->serial;
        ahead->previous_rgb_frame = source->frame;
        ahead->have_previous_rgb = true;
        if (packed->repeat_pixels) ++ahead->stats.rgb_repeat_frames;
    }
    packed->repeat_pixels = NULL;
    ++ahead->count;
    packed->head = (packed->head + 1U) % packed->capacity;
    --packed->count;
    packed->promote_row = 0;
    ahead_queue_stats(ahead);
    return 1;
}

/* 1 means work happened, 0 means yield/full/end/not-ready, -1 means failure.
 * Finish converting a DPB picture before decoding another, unless recovery
 * or realtime catch-up explicitly discards that picture's display output. */
static int ahead_produce(Movie *movie, uint64_t deadline, bool foreground, uint32_t discard_before)
{
    struct VideoLookahead *ahead = movie->video_lookahead;
    VideoLookaheadFrame *slot;
    const ChunkIndexEntry *entry;
    uint32_t local, total_mbs, decoded_mbs;
    size_t start, end;
    unsigned tail;
    uint64_t started, finished;
    uint32_t elapsed;
    unsigned macroblock_budget;
    bool picture_ready = false, pending = false;
    uint8_t *picture = NULL;

    if (ahead->producer_failed) return -1;
    if (ahead->recovery_chunk >= 0) {
        if (foreground) return -1; /* No reserve left: use the ordinary fallback. */
        if (!ahead_time_fits(deadline, ahead_ticks_ms(ahead, 5U))) return 0;
        ahead->failure_stage = "recovery read";
        int loaded = reload_h264_chunk_step(movie, ahead->recovery_chunk,
            &ahead->recovery_offset, ahead_ticks_ms(ahead, 3U));
        if (loaded < 0) return -1;
        if (loaded > 0) ahead->recovery_chunk = -1;
        return 1;
    }
    bool output_packed = ahead->access_unit ? ahead_output_packed(ahead) : ahead_should_pack(ahead);
    if ((output_packed ? ahead->packed->count >= ahead->packed->capacity
                      : ahead->count >= ahead_rgb_capacity(ahead)) ||
        ahead->next_frame >= movie->header.frame_count) return 0;
    if (ahead->picture && ahead->next_frame < discard_before) {
        ahead_discard_picture(movie, ahead);
        return 1;
    }
    if (!foreground) {
        uint32_t initial_guard;
        if (ahead->picture && ahead_output_packed(ahead)) {
            initial_guard = ahead->packed->copy_guard;
        } else if (ahead->picture) {
            size_t remaining_rows = movie->header.video_height - ahead->color_row;
            initial_guard =
                remaining_rows < VIDEO_LOOKAHEAD_COLOR_ROWS && ahead->have_color_tail_sample
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
        ahead->failure_stage = "chunk";
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
            !movie->frame_offsets || !movie->chunk_bytes || !video_decoder_ready(movie))
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
        ahead->failure_stage = "access unit";
        start = movie->frame_offsets[local];
        end =
            local + 1U < entry->frame_count ? movie->frame_offsets[local + 1U] : movie->chunk_size;
        if (end <= start || end > movie->chunk_size)
            return -1;

        /* Keep the RGB front ready without an intermediate copy. Once older
         * packed frames exist, all new output joins that FIFO to preserve order. */
        ahead->output_packed = ahead_should_pack(ahead);
        if (ahead_output_packed(ahead)) {
            VideoLookaheadPacked *packed = ahead->packed;
            tail = (packed->head + packed->count) % packed->capacity;
            if (!packed->frames[tail].pixels && !foreground &&
                !ahead_time_fits(deadline, ahead->wide_floor_ticks)) return 0;
            if (!ahead_allocate_packed_slot(movie, ahead, tail)) {
                if (!packed->allocated_slots) {
                    /* Direct front frames may already wrap around this ring.
                     * Keep its modulus while those pixels are queued; a later
                     * rebegin may retry compact storage or choose a larger ring. */
                    unsigned rgb_capacity = packed->rgb_capacity;
                    ahead_free_packed(ahead);
                    ahead->output_packed = false;
                    if (ahead->count) {
                        ahead->stats.capacity = rgb_capacity;
                        ahead_queue_stats(ahead);
                        return 0;
                    }
                    ahead_select_capacity(movie, ahead);
                    if (!ahead->stats.capacity) { video_lookahead_cancel(movie); return 0; }
                } else {
                    packed->capacity = packed->allocated_slots;
                    packed->head %= packed->capacity;
                    ahead->stats.capacity = packed->rgb_capacity + packed->capacity;
                    ahead_queue_stats(ahead);
                    if (packed->count >= packed->capacity) return 0;
                }
            }
        }
        if (!ahead_output_packed(ahead)) {
            tail = (ahead->head + ahead->count) % ahead_rgb_capacity(ahead);
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
                unsigned rgb_capacity = ahead->allocated_slots;
                if (ahead_has_packed(ahead)) {
                    ahead->packed->rgb_capacity = rgb_capacity;
                    ahead->stats.capacity = rgb_capacity + ahead->packed->capacity;
                } else ahead->stats.capacity = rgb_capacity;
                ahead_queue_stats(ahead);
                if (!rgb_capacity) {
                    video_lookahead_cancel(movie);
                    return 0;
                }
                ahead->head %= rgb_capacity;
                if (ahead->count >= rgb_capacity)
                    return 0;
                tail = (ahead->head + ahead->count) % rgb_capacity;
            }
        }
        /* Consuming queue entries advances head and reduces count together,
         * so this unpublished tail slot stays fixed until the AU is complete.
         * The loaded compressed storage cannot change during partial work. */
        ahead->access_unit = movie->chunk_bytes + start;
        ahead->access_unit_size = end - start;
        ahead->working_local_frame = local;
        ahead->output_slot = ahead_output_packed(ahead) ? NULL : &ahead->frames[tail];
    }
    slot = ahead->output_slot;

    if (!ahead->picture) {
        total_mbs = video_decoder_total_units(movie);
        decoded_mbs = ahead->stats.partial ? video_decoder_done_units(movie) : 0U;
        bool planar_start = movie_uses_planar_decoder(movie) && !ahead->stats.partial;
        unsigned start_kind = ahead->working_local_frame == 0U;
        /* The first planar pump can parse headers and prepare an entire
         * picture before decoding its first CTU/SB. Give it one coding unit
         * and its own guard, instead of learning that setup cost per block.
         * Chunk starts keep a separate estimate from ordinary frame starts. */
        if (!foreground && planar_start) {
            if (!ahead_time_fits(deadline, ahead->start_guard[start_kind]))
                return 0;
            macroblock_budget = video_decoder_min_units(movie);
        } else {
            /* The final batch keeps its separate finishing/deblocking guard. */
            macroblock_budget = foreground ? 0U :
                ahead_macroblock_budget(ahead, total_mbs, decoded_mbs, deadline);
        }
        if (!foreground && !macroblock_budget)
            return 0;
        if (!foreground && movie_uses_planar_decoder(movie)) {
            unsigned minimum = video_decoder_min_units(movie);
            /* A CTU cannot yield internally. Never admit one using a cost
             * estimate for a smaller batch of 16x16 equivalent units. */
            if (macroblock_budget < minimum) {
                uint64_t cost = ((uint64_t)ahead->pump_ticks_per_mb_q8 * minimum + 255U) >> 8;
                uint64_t margin = cost / 8U;
                if (margin < ahead->margin_ticks) margin = ahead->margin_ticks;
                cost += margin;
                if (total_mbs - decoded_mbs <= minimum && cost < ahead->finish_guard)
                    cost = ahead->finish_guard;
                if (cost > UINT32_MAX || !ahead_time_fits(deadline, (uint32_t)cost)) return 0;
                macroblock_budget = minimum;
            }
        }
        bool capture_planar_pump = playback_capture_active(movie) && movie_uses_planar_decoder(movie);
        const uint64_t *submission_count = NULL;
        uint64_t submissions_before = 0U;
        uint32_t captured_before_mbs = decoded_mbs;
        uint32_t captured_after_mbs = 0U;
        if (capture_planar_pump) {
            submission_count = movie->codec == MOVIE_CODEC_AV1 ?
                &movie->av1.submitted_frames : &movie->hevc.submitted_frames;
            submissions_before = *submission_count;
            /* A seek may resume an already submitted picture with consumed=0. */
            if (!ahead->stats.partial)
                captured_before_mbs = video_decoder_done_units(movie);
        }
        ahead->stats.partial = true;
        ahead->decoder_touched = true;
        started = monotonic_clock_now_ticks();
        ahead->failure_stage = "decode";
        uint64_t pump_deadline = 0U;
        if (!foreground && movie_uses_planar_decoder(movie)) {
            /* Keep a costly CTU/SB batch from consuming the next presentation
             * slot. Deadlines are checked between whole coding units; a unit
             * already in progress and picture finalization must finish. */
            pump_deadline = started + ahead->slice_ceiling_ticks;
            if (pump_deadline > deadline) pump_deadline = deadline;
        }
        if (!video_decoder_pump(movie, ahead->access_unit, ahead->access_unit_size,
                                   &ahead->consumed, &ahead->zero_advance_retries,
                                   macroblock_budget, pump_deadline, &picture_ready, &pending, &picture))
            return -1;
        finished = monotonic_clock_now_ticks();
        elapsed = ahead_elapsed(started, finished);
        if (capture_planar_pump) {
            bool pump_submission = *submission_count != submissions_before;
            captured_after_mbs = video_decoder_done_units(movie);
            ahead_capture_planar_pump(&ahead->stats, elapsed, pump_submission ? 0U : captured_before_mbs,
                                       captured_after_mbs, video_decoder_min_units(movie),
                                       pump_submission);
        }
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
        }
        /* Planar foreground calls still decode one indivisible CTU/SB. Keep
         * learning its cost when a stale guard temporarily excludes background
         * work. H264 foreground calls may decode an entire picture instead. */
        if (planar_start) {
            ahead_update_guard(ahead, &ahead->start_guard[start_kind],
                               &ahead->have_start_sample[start_kind], elapsed);
        } else if (!foreground || movie_uses_planar_decoder(movie)) {
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
                uint32_t after_mbs = capture_planar_pump ? captured_after_mbs : video_decoder_done_units(movie);
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
        if ((ahead->recovering && ahead->next_frame < ahead->recovery_target) ||
            ahead->next_frame < discard_before) {
            ahead_discard_picture(movie, ahead);
            return 1; /* Reconstruct references without replacing queued RGB. */
        }
        ahead->picture = picture;
        movie->decoded_local_frame = (int)ahead->working_local_frame;
        if (ahead_has_packed(ahead) && !ahead->packed->checked_layout) {
            VideoFrame view;
            bool favorable = video_decoder_get_frame_view(movie, picture, &view);
            if (favorable) {
                for (unsigned p = 0; p < 3U; ++p)
                    if (((uintptr_t)view.plane[p] | (unsigned)view.stride[p]) & 3U)
                        favorable = false;
            }
            if (favorable) ahead->packed->checked_layout = true;
            else {
                /* Before the first snapshot only: an unfavorable cropped
                 * layout keeps the existing direct-to-RGB path. Never change
                 * representation while a packed queue is populated. */
                ahead->packed_layout_rejected = true;
                ahead->output_packed = false;
                ahead_free_packed(ahead);
                ahead_select_capacity(movie, ahead);
                if (!ahead->stats.capacity) { video_lookahead_cancel(movie); return 0; }
                ahead->output_slot = NULL;
                ahead_queue_stats(ahead);
            }
        }
        ahead->repeat_pixels = NULL;
        if (!ahead_output_packed(ahead) && NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1)
            ahead->repeat_pixels = ahead_repeat_source(movie, ahead, ahead->next_frame,
                av1_frame_serial(movie->av1.decoder), av1_frame_repeats_previous(movie->av1.decoder));
        /* Conversion gets its own quantum: return to input/presentation after
         * this macroblock batch even when there is more deadline slack. */
        if (!foreground)
            return 1;
    }
    if (ahead_output_packed(ahead)) return ahead_pack_picture(movie, deadline, foreground);
    if (!ahead->output_slot) {
        /* First-picture layout fallback can retain an already held picture;
         * acquire its RGB destination cooperatively without resubmitting it. */
        unsigned rgb_tail = (ahead->head + ahead->count) % ahead_rgb_capacity(ahead);
        if (!ahead->frames[rgb_tail].pixels && !foreground &&
            !ahead_time_fits(deadline, ahead->wide_floor_ticks)) return 0;
        if (!ahead_allocate_slot(movie, ahead, rgb_tail)) {
            video_lookahead_cancel(movie);
            return 0;
        }
        ahead->output_slot = &ahead->frames[rgb_tail];
    }
    slot = ahead->output_slot;
    size_t rows = movie->header.video_height - ahead->color_row;
    if (foreground && movie_uses_planar_decoder(movie) && rows > 16U) rows = 16U;
    if (!foreground)
        rows = ahead_color_rows(ahead, rows, deadline);
    if (!rows)
        return 0;
    started = monotonic_clock_now_ticks();
    ahead->failure_stage = "color";
    if (ahead->repeat_pixels) {
        size_t offset = ahead->color_row * movie->header.video_width;
        size_t bytes = rows * movie->header.video_width * sizeof(uint16_t);
        player_copy_maybe_fast(slot->pixels + offset, ahead->repeat_pixels + offset, bytes);
    } else if (!video_blit_picture_rows(movie, ahead->picture, slot->pixels,
                                       movie->header.video_width, ahead->color_row, rows,
                                       &ahead->color_flat))
        return -1;
    finished = monotonic_clock_now_ticks();
    elapsed = ahead_elapsed(started, finished);
    if (ahead->repeat_pixels)
        playback_capture_stage(movie, CAPTURE_COLOR, started, finished);
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
            if (!ahead->repeat_pixels)
                ahead_update_guard(ahead, &ahead->color_tail_guard, &ahead->have_color_tail_sample,
                                   elapsed);
        }
        /* Short tails contain fixed call/setup work too. Do not magnify it
         * into a per-row estimate for a later full band; the separate tail
         * estimate above measures that fixed operation directly. */
        if (rows >= 16U && !ahead->repeat_pixels) {
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
    if (NDVIDEO_WITH_AV1 && movie->codec == MOVIE_CODEC_AV1) {
        ahead->previous_rgb_serial = av1_frame_serial(movie->av1.decoder);
        ahead->previous_rgb_frame = ahead->next_frame;
        ahead->have_previous_rgb = true;
        if (ahead->repeat_pixels) ++ahead->stats.rgb_repeat_frames;
    }
    ahead->repeat_pixels = NULL;
    video_decoder_release_picture(movie);
    if (ahead->recovering) {
        ahead->recovering = false;
        ++ahead->stats.recoveries;
    }
    slot->frame = ahead->next_frame++;
    slot->chunk = ahead->idr_chunk;
    slot->idr_first = ahead->idr_first;
    slot->idr_end = ahead->idr_end;
    ++ahead->count;
    ahead_queue_stats(ahead);
    if (foreground)
        ++ahead->stats.foreground_frames;
    else
        ++ahead->stats.background_frames;
    ahead->consumed = 0U;
    ahead->zero_advance_retries = 0U;
    ahead->picture = NULL;
    ahead->access_unit = NULL;
    ahead->output_slot = NULL;
    ahead->output_packed = false;
    ahead->color_row = 0U;
    ahead->stats.partial = false;
    return 1;
}

static int ahead_compact_work(Movie *movie, uint64_t deadline, bool foreground,
                              uint32_t discard_before)
{
    struct VideoLookahead *ahead = movie->video_lookahead;
    if (ahead_has_packed(ahead) && ahead->packed->count &&
        ahead->count < ahead->packed->rgb_capacity)
        return ahead_promote_packed(movie, deadline, foreground);
    return ahead_produce(movie, deadline, foreground, discard_before);
}

bool video_lookahead_step(Movie *movie, uint64_t deadline_ticks)
{
    struct VideoLookahead *ahead;
    int result;
    if (!video_lookahead_active(movie))
        return false;
    ahead = movie->video_lookahead;
    if (ahead->producer_failed && (!ahead_has_packed(ahead) || !ahead->packed->count)) return false;
    result = ahead_compact_work(movie, deadline_ticks, false, 0);
    if (result < 0) ahead_recover(movie, ahead);
    return result > 0;
}

static bool ahead_take(Movie *movie, uint32_t target_frame, bool queued_hit)
{
    struct VideoLookahead *ahead;
    VideoLookaheadFrame *slot;
    uint16_t *old_pixels;
    uint8_t *old_allocation;
    if (!video_lookahead_active(movie))
        return false;
    ahead = movie->video_lookahead;
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
    ahead->head = (ahead->head + 1U) % ahead_rgb_capacity(ahead);
    --ahead->count;
    ahead_queue_stats(ahead);
    if (ahead->have_prepared_frame && ahead->prepared_frame == target_frame) {
        if (!ahead->prepared_depth)
            queued_hit = false;
        ahead->have_prepared_frame = false;
    }
    if (queued_hit)
        ++ahead->stats.queue_hits;
    if (!ahead_total_queued(ahead) && !ahead->stats.partial && ahead->next_frame == target_frame + 1U)
        ahead->decoder_touched = false;
    return true;
}

bool video_lookahead_take(Movie *movie, uint32_t target_frame)
{
    return ahead_take(movie, target_frame, true);
}

static int ahead_prepare_packed(Movie *movie, uint32_t target_frame)
{
    struct VideoLookahead *ahead = movie->video_lookahead;
    if (ahead->count)
        return ahead->frames[ahead->head].frame == target_frame ? 1 : 0;
    if (ahead->packed->count) {
        if (ahead->packed->frames[ahead->packed->head].frame != target_frame) return 0;
    } else if (ahead->next_frame != target_frame) return 0;
    /* Keep H.264's synchronous foreground preparation; HEVC/AV1 yield to
     * the input loop between their longer decode/conversion slices. */
    uint64_t quantum_end = movie_uses_planar_decoder(movie)
        ? monotonic_clock_now_ticks() + ahead->foreground_slice_ticks : 0;
    int result;
    do {
        result = ahead_compact_work(movie, 0, true, 0);
    } while (result > 0 && !ahead->count && ahead->stats.active &&
             (!quantum_end || monotonic_clock_now_ticks() < quantum_end));
    if (!ahead->stats.active) return 0;
    if (ahead->count && ahead->frames[ahead->head].frame == target_frame) return 1;
    if (result > 0) return 2;
    if (!ahead->producer_failed) ahead_note_failure(movie, ahead);
    video_decoder_mark_failed(movie);
    video_lookahead_cancel(movie);
    return -1;
}

int video_lookahead_prepare_target(Movie *movie, uint32_t target_frame)
{
    struct VideoLookahead *ahead;
    int result;
    if (!video_lookahead_active(movie))
        return 0;
    ahead = movie->video_lookahead;
    if (target_frame != movie->current_frame + 1U)
        return 0;
    if (!ahead->have_prepared_frame || ahead->prepared_frame != target_frame) {
        ahead->prepared_frame = target_frame;
        ahead->prepared_depth = ahead_total_queued(ahead);
        ahead->have_prepared_frame = true;
        if (!ahead_total_queued(ahead)) ++ahead->stats.queue_misses;
    }
    if (ahead_has_packed(ahead)) return ahead_prepare_packed(movie, target_frame);
    if (ahead->count && ahead->frames[ahead->head].frame == target_frame)
        return 1;
    if (target_frame != movie->current_frame + 1U || ahead->count ||
        ahead->next_frame != target_frame)
        return 0;
    uint64_t quantum_end = movie_uses_planar_decoder(movie)
        ? monotonic_clock_now_ticks() + ahead->foreground_slice_ticks : 0;
    do {
        result = ahead_produce(movie, 0U, true, 0);
    } while (movie_uses_planar_decoder(movie) && result > 0 && !ahead->count &&
             ahead->stats.active && monotonic_clock_now_ticks() < quantum_end);
    if (!ahead->stats.active)
        return 0;
    if (result > 0 && ahead->count && ahead->frames[ahead->head].frame == target_frame)
        return 1;
    if (movie_uses_planar_decoder(movie) && result > 0) return 2;
    if (!ahead->producer_failed) ahead_note_failure(movie, ahead);
    video_decoder_mark_failed(movie);
    video_lookahead_cancel(movie);
    return -1;
}

unsigned video_lookahead_prepared_depth(const Movie *movie, uint32_t target_frame)
{
    const struct VideoLookahead *ahead;
    if (!video_lookahead_active(movie))
        return 0U;
    ahead = movie->video_lookahead;
    return ahead->have_prepared_frame && ahead->prepared_frame == target_frame
               ? ahead->prepared_depth
               : ahead_total_queued(ahead);
}

int video_lookahead_finish_target(Movie *movie, uint32_t target_frame)
{
    int ready = video_lookahead_prepare_target(movie, target_frame);
    if (ready != 1)
        return ready;
    if (video_lookahead_take(movie, target_frame))
        return 1;
    return -1;
}

bool video_lookahead_pending_realtime_target(const Movie *movie, uint32_t *target)
{
    if (!video_lookahead_active(movie) || !movie->video_lookahead->realtime_pending)
        return false;
    *target = movie->video_lookahead->realtime_target;
    return true;
}

static int ahead_finish_packed_realtime(Movie *movie, uint32_t target_frame)
{
    struct VideoLookahead *ahead = movie->video_lookahead;
    VideoLookaheadPacked *packed = ahead->packed;
    ahead->have_prepared_frame = false;
    while (ahead->count && ahead->frames[ahead->head].frame < target_frame) {
        ahead->head = (ahead->head + 1U) % packed->rgb_capacity;
        --ahead->count;
    }
    while (packed->count && packed->frames[packed->head].frame < target_frame) {
        packed->head = (packed->head + 1U) % packed->capacity;
        --packed->count;
        packed->promote_row = 0;
        packed->repeat_pixels = NULL;
        ahead->have_previous_rgb = false;
    }
    ahead_queue_stats(ahead);
    if (ahead->count) {
        bool taken = ahead_take(movie, target_frame, true);
        if (taken) ahead->realtime_pending = false;
        return taken ? 1 : 0;
    }
    if (packed->count && packed->frames[packed->head].frame > target_frame) return 0;
    if (!packed->count && ahead->next_frame > target_frame) return 0;
    if (!ahead->realtime_pending) {
        if (!packed->count) ++ahead->stats.queue_misses;
        ahead->realtime_pending = true;
        ahead->realtime_target = target_frame;
    }
    uint64_t quantum_end = movie_uses_planar_decoder(movie)
        ? monotonic_clock_now_ticks() + ahead->foreground_slice_ticks : 0;
    while (!ahead->count) {
        int result = ahead_compact_work(movie, 0, true, target_frame);
        if (!ahead->stats.active) return 0;
        if (result <= 0) {
            if (!ahead->producer_failed) ahead_note_failure(movie, ahead);
            video_decoder_mark_failed(movie);
            video_lookahead_cancel(movie);
            return -1;
        }
        if (!ahead->count && quantum_end && monotonic_clock_now_ticks() >= quantum_end) return 2;
    }
    bool taken = ahead_take(movie, target_frame, false);
    if (taken) ahead->realtime_pending = false;
    return taken ? 1 : 0;
}

int video_lookahead_finish_realtime_target(Movie *movie, uint32_t target_frame)
{
    if (!video_lookahead_active(movie)) return 0;
    struct VideoLookahead *ahead = movie->video_lookahead;
    /* Block decoders return to the input loop between coding units. Finish the chosen image
     * even if the clock passes it meanwhile, otherwise an overloaded decoder
     * can discard every picture without ever refreshing the display. */
    if (ahead->realtime_pending) target_frame = ahead->realtime_target;
    if (target_frame <= movie->current_frame || target_frame >= movie->header.frame_count) return 0;
    if (ahead_has_packed(ahead)) return ahead_finish_packed_realtime(movie, target_frame);
    ahead->have_prepared_frame = false;
    while (ahead->count && ahead->frames[ahead->head].frame < target_frame) {
        ahead->head = (ahead->head + 1U) % ahead_rgb_capacity(ahead);
        --ahead->count;
    }
    ahead_queue_stats(ahead);
    if (ahead->count) {
        bool taken = ahead_take(movie, target_frame, true);
        if (taken) ahead->realtime_pending = false;
        return taken ? 1 : 0;
    }
    if (ahead->next_frame > target_frame) return 0;
    if (!ahead->realtime_pending) ++ahead->stats.queue_misses;
    if (movie_uses_planar_decoder(movie)) {
        ahead->realtime_pending = true;
        ahead->realtime_target = target_frame;
    }
    uint64_t quantum_end = movie_uses_planar_decoder(movie)
        ? monotonic_clock_now_ticks() + ahead->foreground_slice_ticks : 0;
    while (!ahead->count) {
        int result = ahead_produce(movie, 0U, true, target_frame);
        if (result <= 0) {
            if (!ahead->producer_failed) ahead_note_failure(movie, ahead);
            video_decoder_mark_failed(movie);
            video_lookahead_cancel(movie);
            return -1;
        }
        if (movie_uses_planar_decoder(movie) && !ahead->count &&
            monotonic_clock_now_ticks() >= quantum_end) return 2;
    }
    bool taken = ahead_take(movie, target_frame, false);
    if (taken) ahead->realtime_pending = false;
    return taken ? 1 : 0;
}

void video_lookahead_cancel(Movie *movie)
{
    struct VideoLookahead *ahead;
    bool touched;
    if (!movie || !(ahead = movie->video_lookahead))
        return;
    touched = ahead->decoder_touched;
    ahead->repeat_pixels = NULL;
    ahead->have_previous_rgb = false;
    if (ahead_has_packed(ahead)) ahead->packed->repeat_pixels = NULL;
    /* An inactive queue does not own a picture held by a suspended seek. */
    if (ahead->picture) video_decoder_release_picture(movie);
    if (ahead->stats.active)
        ++ahead->stats.cancellations;
    ahead->stats.active = false;
    ahead->recovering = ahead->producer_failed = false;
    ahead->recovery_chunk = -1;
    ahead->have_prepared_frame = false;
    ahead->realtime_pending = false;
    movie->foreground_pending_ticks = 0;
    ahead->stats.partial = false;
    ahead->head = ahead->count = 0U;
    if (ahead_has_packed(ahead)) {
        ahead->packed->head = ahead->packed->count = 0U;
        ahead->packed->promote_row = 0U;
    }
    ahead_queue_stats(ahead);
    ahead->consumed = 0U;
    ahead->zero_advance_retries = 0U;
    ahead->picture = NULL;
    ahead->access_unit = NULL;
    ahead->output_slot = NULL;
    ahead->output_packed = false;
    ahead->color_row = 0U;
    ahead->decoder_touched = false;
    if (touched) {
        /* The decoder removes emulation-prevention bytes in place. Replaying
         * any partly/fully decoded AU requires a fresh compressed chunk. */
        if (!video_decoder_reset(movie))
            video_decoder_mark_failed(movie);
        invalidate_loaded_chunk_state(movie);
    }
}

void video_lookahead_destroy(Movie *movie)
{
    struct VideoLookahead *ahead;
    unsigned i;
    if (!movie || !(ahead = movie->video_lookahead))
        return;
    video_decoder_release_picture(movie);
    for (i = 0U; i < VIDEO_LOOKAHEAD_MAX_FRAMES; ++i) {
        if (ahead->frames[i].pixels)
            player_free_aligned(ahead->frames[i].pixels, ahead->frames[i].allocation);
    }
    ahead_free_packed(ahead);
    free(ahead);
    movie->video_lookahead = NULL;
}

uint32_t video_lookahead_next_frame(const Movie *movie)
{
    return video_lookahead_active(movie) ? movie->video_lookahead->next_frame
           : movie                      ? movie->current_frame + 1U
                                        : 0U;
}

unsigned video_lookahead_queued(const Movie *movie)
{
    return video_lookahead_active(movie) ? ahead_total_queued(movie->video_lookahead) : 0U;
}

unsigned video_lookahead_ready(const Movie *movie)
{
    return video_lookahead_active(movie) ? movie->video_lookahead->count : 0U;
}

size_t video_lookahead_memory_bytes(const Movie *movie)
{
    return movie && movie->video_lookahead ? movie->video_lookahead->stats.allocated_bytes : 0U;
}

void video_lookahead_get_stats(const Movie *movie, VideoLookaheadStats *out)
{
    if (!out)
        return;
    if (movie && movie->video_lookahead) {
        const struct VideoLookahead *ahead = movie->video_lookahead;
        *out = ahead->stats;
        out->planar_frame_start_guard_ticks = ahead->start_guard[0];
        out->planar_chunk_start_guard_ticks = ahead->start_guard[1];
        out->color_tail_guard_ticks =
            ahead->have_color_tail_sample ? ahead->color_tail_guard : ahead->color_guard;
    } else
        memset(out, 0, sizeof(*out));
}
