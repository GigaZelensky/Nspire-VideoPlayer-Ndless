#include <stdlib.h>
#include <string.h>
#include "raw_player_io.h"
#include "app_task_io.h"
#include "native_interrupts.h"
#include "player_idle.h"
#include "storage_activity.h"
#include "storage_mutation.h"
#include "raw_region_cache.h"
#include "movie_crypto_session.h"
#include "../platform/portable_reader_platform.h"
#include "../storage/raw_file_reader.h"
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
struct RawPlayerIo {
    PortableReaderPlatform platform;
    void *input;
    RawFileReader reader;
    PortableStorageSnapshot snapshot;
    RawFileOverlay overlays[32];
    uint8_t buffer[32768];
    NveReader *crypt;
    bool crypt_restart;
    unsigned overlay_count;
    RawFileOverlay metadata_overlays[PORTABLE_STORAGE_CLEAN_MAX];
    uint32_t metadata_total;
    RawFileCosts costs_total;
    RawRegionCache region_cache;
    uint32_t native_seed[RAW_FILE_REGION_HISTOGRAM_SLOTS], native_seed_other, native_seed_last[3];
    uint64_t offset;
    size_t bytes;
    bool requested, running, ready, view, park, failed, stopping, discard_regions;
    uint32_t started, ended, completed_bytes, refreshes, handoffs, steps, max_step_ticks,
        foreground_ticks, physical_total, regions_total;
    int error;
};
static RawPlayerIo *live;
static int create_error;
static bool native_requested;
typedef struct {
    int error, cache_status;
    uint32_t refreshes, handoffs, steps, foreground_ticks, max_step_ticks, physical, regions,
        cache_nodes, dirty_blocks, metadata_hits;
    RawFileCosts costs;
    RawRegionCacheStats region_cache;
    StorageMutationStats mutations;
    uint32_t mutation_epoch;
    uint32_t native_seed[RAW_FILE_REGION_HISTOGRAM_SLOTS], native_seed_other, native_seed_last[3];
} RawPlayerReport;
static RawPlayerReport last_report;
static void accumulate_reader_costs(RawPlayerIo *ctx)
{
    ctx->physical_total += ctx->reader.page_reads;
    ctx->reader.page_reads = 0;
    ctx->regions_total += ctx->reader.regions_loaded;
    ctx->reader.regions_loaded = 0;
    ctx->metadata_total += ctx->reader.metadata_reads;
    ctx->reader.metadata_reads = 0;
    raw_file_costs_add(&ctx->costs_total, &ctx->reader.costs);
    memset(&ctx->reader.costs, 0, sizeof(ctx->reader.costs));
}
static RawPlayerReport report(const RawPlayerIo *ctx)
{
    RawPlayerReport r = {0};
    r.error = ctx->error;
    r.cache_status = ctx->snapshot.status;
    r.refreshes = ctx->refreshes;
    r.handoffs = ctx->handoffs;
    r.steps = ctx->steps;
    r.foreground_ticks = ctx->foreground_ticks;
    r.max_step_ticks = ctx->max_step_ticks;
    r.physical = ctx->physical_total + ctx->reader.page_reads;
    r.regions = ctx->regions_total + ctx->reader.regions_loaded;
    r.cache_nodes = ctx->snapshot.cache_nodes;
    r.dirty_blocks = ctx->snapshot.dirty_count;
    r.metadata_hits = ctx->metadata_total + ctx->reader.metadata_reads;
    r.costs = ctx->costs_total;
    raw_file_costs_add(&r.costs, &ctx->reader.costs);
    r.region_cache = ctx->region_cache.stats;
    r.mutations = storage_mutation_stats();
    r.mutation_epoch = storage_mutation_epoch();
    memcpy(r.native_seed, ctx->native_seed, sizeof(r.native_seed));
    r.native_seed_other = ctx->native_seed_other;
    memcpy(r.native_seed_last, ctx->native_seed_last, sizeof(r.native_seed_last));
    return r;
}
static uint32_t counter(void) { return REG(0x900c0004U); }
static uint32_t now(void) { return 0U - counter(); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static unsigned le16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t mutation_epoch(void *unused)
{
    (void)unused;
    return storage_mutation_epoch();
}
static uint32_t mutation_generation(void *unused, uint32_t block)
{
    (void)unused;
    return storage_mutation_block_generation(block);
}
static bool initialize_reader(RawPlayerIo *ctx)
{
    const PortableStorageSnapshot *s = &ctx->snapshot;
    ctx->overlay_count = s->dirty_count;
    for (unsigned i = 0; i < s->dirty_count; ++i)
        ctx->overlays[i] =
            (RawFileOverlay){s->dirty[i].number, (const uint8_t *)(uintptr_t)s->dirty[i].address};
    NandPageConfig config = {s->kind, portable_reader_platform_bus(), s->spare};
    if (!raw_file_init(&ctx->reader, s->state_data, sizeof(s->state_data), s->state, s->block_count,
                       s->index_block, s->inode, config, s->layout, ctx->overlays,
                       ctx->overlay_count))
        return false;
    for (unsigned i = 0; i < s->clean_count; ++i)
        ctx->metadata_overlays[i] =
            (RawFileOverlay){s->clean[i].number, (const uint8_t *)(uintptr_t)s->clean[i].address};
    if (!raw_file_metadata_cache(&ctx->reader, ctx->metadata_overlays, s->clean_count))
        return false;
    RawRegionMutations mutations = {NULL, mutation_epoch, mutation_generation};
    raw_region_cache_begin_view(&ctx->region_cache, &ctx->reader.map, &ctx->reader.layout,
                                ctx->reader.state, sizeof(ctx->reader.state), mutations);
    return true;
}
static bool provider_quiescent(const RawPlayerIo *ctx)
{
    return !ctx->reader.initialized || ctx->reader.nand.quiescent;
}
static bool mutation_tracking_observed(const RawPlayerIo *ctx)
{
    /* Only the existing verified SPI writer can call this observer today.
     * A portable reader does not authorize that writer on another platform.
     * Before its first observed callback, conservatively drop cross-handoff
     * map proofs too. Foreground native calls always invalidate separately. */
    return ctx->platform.kind == NAND_PAGE_CX2_SPI && storage_mutation_stats().observed != 0U;
}
static bool release_view(RawPlayerIo *ctx)
{
    bool safe = provider_quiescent(ctx);
    if (!safe)
        return false;
    if (ctx->view) {
        if (ctx->failed || ctx->discard_regions || storage_native_active()) {
            if (ctx->failed || storage_native_active())
                storage_mutation_invalidate();
            raw_region_cache_clear(&ctx->region_cache);
        } else {
            /* Only completed independently owned maps are saved. Borrowed
             * native maps and the loader/partial-copy workspace are excluded.
             * No reader can use a returned cache pointer after this boundary. */
            for (unsigned i = 0; i < FFX_MAP_SLOTS; ++i)
                for (unsigned j = 0; j < FFX_MAP_SLOTS; ++j)
                    if (ctx->reader.map.region[i].data == ctx->reader.cache[j] &&
                        ctx->reader.map.region[i].bytes == FFX_MAP_REGION_BYTES)
                        raw_region_cache_store(&ctx->region_cache, ctx->reader.cache[j],
                                               FFX_MAP_REGION_BYTES);
            raw_region_cache_end_view(&ctx->region_cache);
        }
    }
    ctx->view = false;
    ctx->running = false;
    ++ctx->handoffs;
    return true;
}
static bool capture_view(RawPlayerIo *ctx)
{
    accumulate_reader_costs(ctx);
    int status = portable_reader_platform_capture(&ctx->platform, ctx->input, 0U, &ctx->snapshot);
    if (status) {
        ctx->error = status;
        return false;
    }
    if (!initialize_reader(ctx))
        return false;
    /* Borrow metadata only until the next serialized native filesystem call. */
    for (unsigned i = 0; i < 3; ++i) {
        const PortableStorageRegion *region = &ctx->snapshot.region[i];
        const uint8_t *data = (const uint8_t *)(uintptr_t)region->address;
        ctx->native_seed_last[i] = UINT32_MAX;
        if (region->active && le16(data) && le32(data + 352) == ctx->snapshot.state) {
            ctx->reader.map.region[i] = (FfxMapRegion){data, FFX_MAP_REGION_BYTES};
            ctx->reader.ages[i] = i + 1;
            uint32_t id = le16(data + 4);
            ctx->native_seed_last[i] = id;
            if (id < RAW_FILE_REGION_HISTOGRAM_SLOTS)
                ++ctx->native_seed[id];
            else
                ++ctx->native_seed_other;
        }
    }
    ctx->reader.age = 3;
    ctx->view = true;
    ctx->discard_regions = false;
    ++ctx->refreshes;
    return true;
}
static void restore_cached_region(RawPlayerIo *ctx)
{
    RawFileReader *r = &ctx->reader;
    /* The core discovers a missing region and creates its loader in one
     * step. Intercept that untouched loader before it issues any NAND read. */
    if (r->status != RAW_FILE_PENDING || r->canceled || !r->loading || r->copying || r->physical ||
        r->loader.status != FFX_REGION_MORE || r->loader.pending || r->loader.cursor ||
        r->loader.count)
        return;
    const uint8_t *data = raw_region_cache_lookup(&ctx->region_cache, r->loader.id);
    if (!data)
        return;
    unsigned victim = 0;
    for (unsigned i = 0; i < FFX_MAP_SLOTS; ++i) {
        if (!r->map.region[i].data) {
            victim = i;
            break;
        }
        if (r->ages[i] < r->ages[victim])
            victim = i;
    }
    r->map.region[victim] = (FfxMapRegion){data, FFX_MAP_REGION_BYTES};
    r->ages[victim] = ++r->age;
    r->loading = false;
    if (r->costs.regions_started)
        --r->costs.regions_started;
}
static void service_once(RawPlayerIo *ctx)
{
    if (ctx->failed) {
        /* ERROR is not a controller-ownership release. The CX backend can
         * finish cleanup on later steps after a delayed ready transition. */
        if (!provider_quiescent(ctx)) {
            raw_file_cancel(&ctx->reader);
            raw_file_step(&ctx->reader, now(), 32768U);
        }
        return;
    }
    if (ctx->view && ctx->region_cache.view_active &&
        storage_mutation_epoch() != ctx->region_cache.view_epoch) {
        raw_region_cache_clear(&ctx->region_cache);
        ctx->discard_regions = true;
        ctx->park = true;
    }
    if (ctx->view && storage_native_active())
        ctx->park = true;
    if (ctx->park) {
        if (ctx->reader.status == RAW_FILE_PENDING || !provider_quiescent(ctx)) {
            raw_file_cancel(&ctx->reader);
            raw_file_step(&ctx->reader, now(), 32768U);
            if (ctx->reader.status == RAW_FILE_PENDING || !provider_quiescent(ctx))
                return;
        }
        release_view(ctx);
        ctx->park = false;
        return;
    }
    if (!ctx->requested || ctx->ready)
        return;
    /* A queued writer stops NEW requests, not progress on an existing read.
     * Canceling on every 1 Hz journal write can starve a cold region rebuild. */
    if (native_requested && !ctx->running)
        return;
    if (ctx->crypt) {
        if (ctx->crypt_restart) {
            if (ctx->offset > UINT32_MAX ||
                !nve_reader_begin(ctx->crypt, (uint32_t)ctx->offset, ctx->buffer, (uint32_t)ctx->bytes)) {
                ctx->failed = true; ctx->error = -320; return;
            }
            ctx->crypt_restart = false;
        }
        if (ctx->crypt->phase != NVE_READ_FETCH) {
            nve_reader_step(ctx->crypt);
            if (ctx->crypt->phase == NVE_READ_DONE) {
                ctx->ready = true;
                ctx->ended = counter();
                ctx->completed_bytes = (uint32_t)ctx->bytes;
            } else if (ctx->crypt->phase == NVE_READ_ERROR) {
                ctx->failed = true; ctx->error = -321;
            }
            return;
        }
    }
    if (!ctx->view) {
        if (native_requested || storage_native_active())
            return;
        if (!capture_view(ctx)) {
            if (!ctx->error)
                ctx->error = -305;
            ctx->failed = true;
            storage_mutation_invalidate();
            raw_region_cache_clear(&ctx->region_cache);
            release_view(ctx);
            return;
        }
    }
    if (!ctx->running) {
        uint64_t offset = ctx->crypt ? nve_reader_physical_offset(ctx->crypt) : ctx->offset;
        void *buffer = ctx->crypt ? ctx->crypt->block : ctx->buffer;
        uint32_t bytes = ctx->crypt ? ctx->crypt->unit_bytes + NVE_TAG_BYTES : (uint32_t)ctx->bytes;
        if (!raw_file_begin(&ctx->reader, offset, buffer, bytes)) {
            ctx->failed = true;
            ctx->error = -306;
            return;
        }
        ctx->running = true;
    }
    restore_cached_region(ctx);
    RawFileStatus status = raw_file_step(&ctx->reader, now(), 32768U);
    if (status == RAW_FILE_DONE) {
        ctx->running = false;
        if (ctx->crypt) nve_reader_supplied(ctx->crypt, true);
        else {
            ctx->ready = true;
            ctx->ended = counter();
            ctx->completed_bytes = (uint32_t)ctx->bytes;
        }
    }
    if (status == RAW_FILE_CANCELED)
        ctx->running = false;
    if (status == RAW_FILE_ERROR) {
        ctx->failed = true;
        ctx->error = (int)ctx->reader.error;
        release_view(ctx);
    }
}
static void service(RawPlayerIo *ctx, uint32_t budget)
{
    if (storage_native_active() ||
        (ctx->failed ? provider_quiescent(ctx)
                     : (!ctx->park &&
                        (!ctx->requested || ctx->ready || (native_requested && !ctx->running)))))
        return;
    uint32_t start = counter();
    unsigned mask = native_critical_enter();
    do {
        uint32_t before = counter();
        /* Keep the existing read batching for plain movies. Crypto steps do
         * more CPU work, so check the playback deadline after each one. */
        unsigned batch = ctx->crypt ? 1U : 4U;
        for (unsigned i = 0; i < batch; ++i) {
            service_once(ctx);
            ++ctx->steps;
            if (ctx->failed || ctx->ready || (!ctx->requested && !ctx->park) ||
                (native_requested && !ctx->running))
                break;
        }
        uint32_t ticks = before - counter();
        ctx->foreground_ticks += ticks;
        if (ticks > ctx->max_step_ticks)
            ctx->max_step_ticks = ticks;
        if (ctx->failed || ctx->ready || (!ctx->requested && !ctx->park) ||
            (native_requested && !ctx->running))
            break;
    } while ((uint32_t)(start - counter()) < budget);
    native_critical_leave(mask);
}
RawPlayerIo *raw_player_create(const char *path)
{
    create_error = 0;
    if (live || !path || app_task_io_in_job())
        return NULL;
    unsigned entry_mask = native_interrupt_mask();
    RawPlayerIo *ctx = calloc(1, sizeof(*ctx));
    native_critical_leave(entry_mask);
    if (!ctx)
        return NULL;
    create_error = portable_reader_platform_init(&ctx->platform);
    if (create_error)
        goto fail;
    create_error = -311;
    raw_player_before_native();
    native_critical_leave(entry_mask);
    ctx->input = portable_reader_platform_open(&ctx->platform, path);
    if (!ctx->input)
        goto fail;
    /* fopen establishes the handle/inode without a bootstrap file-data read.
     * Acquire the first validated view before advertising reader availability. */
    unsigned saved = native_critical_enter();
    bool captured = capture_view(ctx);
    native_critical_leave(saved);
    if (!captured) {
        create_error = ctx->error ? ctx->error : -310;
        goto fail;
    }
    const NveKeys *keys = movie_crypto_keys(path);
    if (keys) {
        create_error = -320;
        if (keys->header.physical_bytes != ctx->snapshot.file_bytes ||
            !(ctx->crypt = calloc(1, sizeof(*ctx->crypt))))
            goto fail;
        nve_reader_init(ctx->crypt, keys);
    }
    create_error = 0;
    live = ctx;
    native_critical_leave(entry_mask);
    return ctx;
fail:
    if (ctx->input)
        portable_reader_platform_close(&ctx->platform, ctx->input);
    free(ctx);
    native_critical_leave(entry_mask);
    return NULL;
}
void raw_player_cancel(RawPlayerIo *ctx)
{
    if (!ctx)
        return;
    raw_region_cache_clear(&ctx->region_cache);
    ctx->discard_regions = true;
    ctx->requested = false;
    ctx->ready = false;
    ctx->crypt_restart = true;
    if (ctx->running)
        raw_file_cancel(&ctx->reader);
    ctx->park = true;
}
bool raw_player_prepare_native(void)
{
    native_requested = true;
    RawPlayerIo *ctx = live;
    if (!ctx)
        return true;
    if (ctx->failed) {
        if (!provider_quiescent(ctx))
            service(ctx, 8U);
        if (!provider_quiescent(ctx))
            return false;
        release_view(ctx);
        return true;
    }
    service(ctx, 8U);
    if (ctx->running || !provider_quiescent(ctx))
        return false;
    if (ctx->view && !mutation_tracking_observed(ctx)) {
        /* Unknown native mutation paths cannot retain generation-based map
         * proofs. Do this only at the actual quiescent handoff, never while
         * waiting for a cold read (which would otherwise be starved). */
        storage_mutation_invalidate();
        ctx->discard_regions = true;
    }
    if (ctx->view)
        release_view(ctx);
    ctx->park = false;
    return !ctx->view && !ctx->running && provider_quiescent(ctx);
}
void raw_player_native_released(void) { native_requested = false; }
static void before_foreground_io(bool native)
{
    /* Ordinary foreground file/directory calls cannot reenter a suspended
     * writer. Deliberate lifecycle boundaries drain first; ongoing playback
     * writes use the private contexts instead. */
    if (app_task_io_in_job())
        return;
    /* Calls outside the observed private writer (including standby) are not
     * mutation-complete. Never reuse a region proof across this boundary. */
    if (native)
        storage_mutation_invalidate();
    /* Explicit foreground/lifecycle operations may discard a pending read;
     * background journal/screenshot jobs use the non-canceling handoff above. */
    RawPlayerIo *ctx = live;
    bool wanted = ctx && ctx->requested && !ctx->stopping;
    if (ctx) {
        if (native)
            raw_player_cancel(ctx);
        else {
            ctx->requested = ctx->ready = false;
            ctx->crypt_restart = true;
            raw_file_cancel(&ctx->reader);
            ctx->park = true;
        }
    }
    app_task_io_drain();
    while (!raw_player_prepare_native()) {
    }
    raw_player_native_released();
    if (ctx)
        ctx->requested = wanted;
}
void raw_player_before_native(void) { before_foreground_io(true); }
void raw_player_before_read(void) { before_foreground_io(false); }
void raw_player_service(uint32_t budget)
{
    if (live)
        service(live, budget);
}
void raw_player_destroy(RawPlayerIo *ctx)
{
    if (!ctx)
        return;
    unsigned entry_mask = native_interrupt_mask();
    /* Freeze app-lifetime mutation diagnostics before foreground close/drain
     * invalidations and later CSV writes can inflate the playback evidence. */
    last_report = report(ctx);
    ctx->stopping = true;
    raw_player_before_native();
    native_critical_leave(entry_mask);
    if (live == ctx)
        live = NULL;
    if (ctx->input)
        portable_reader_platform_close(&ctx->platform, ctx->input);
    if (ctx->crypt) {
        nve_wipe(ctx->crypt, sizeof(*ctx->crypt));
        free(ctx->crypt);
        nve_wipe(ctx->buffer, sizeof(ctx->buffer));
    }
    free(ctx);
    native_critical_leave(entry_mask);
}
int raw_player_read(RawPlayerIo *ctx, uint64_t offset, void *destination, size_t bytes, bool wait)
{
    if (!ctx || !destination || !bytes || bytes > sizeof(ctx->buffer) || ctx->failed)
        return -1;
    if (!ctx->requested || ctx->offset != offset || ctx->bytes != bytes) {
        if (ctx->running) {
            raw_file_cancel(&ctx->reader);
            ctx->park = true;
        }
        ctx->offset = offset;
        ctx->bytes = bytes;
        ctx->requested = true;
        ctx->ready = false;
        ctx->crypt_restart = true;
        ctx->started = counter();
    }
    do {
        /* Nonblocking submit/collect must not secretly spend another reader
         * quantum. The caller budgets service explicitly, then polls READY.
         * Foreground joins still drive both reader and writer to completion. */
        if (wait)
            service(ctx, 8U);
        if (ctx->failed)
            return -1;
        if (ctx->ready) {
            memcpy(destination, ctx->buffer, bytes);
            ctx->requested = false;
            ctx->ready = false;
            return 1;
        }
        if (wait)
            app_task_io_service(8U);
    } while (wait);
    return 0;
}
bool raw_player_idle(unsigned milliseconds)
{
    RawPlayerIo *ctx = live;
    if (!ctx && !app_task_io_pending())
        return false;
    uint32_t start = counter(),
             duration = (uint32_t)(((uint64_t)milliseconds * 32768U + 999U) / 1000U);
    do {
        app_task_io_service(8U);
        if (ctx)
            service(ctx, 8U);
        uint32_t elapsed = start - counter();
        if (elapsed >= duration)
            break;
        if (!app_task_io_pending() &&
            (!ctx || ctx->failed || ((!ctx->requested || ctx->ready) && !ctx->park))) {
            unsigned sleep = (unsigned)(((uint64_t)(duration - elapsed) * 1000U) / 32768U);
            if (sleep)
                player_idle_sleep(sleep);
            else
                break;
        }
    } while ((uint32_t)(start - counter()) < duration);
    return true;
}
size_t raw_player_memory_bytes(void)
{
    return sizeof(RawPlayerIo) + (live && live->crypt ? sizeof(NveReader) : 0U);
}
bool raw_player_last_read(RawPlayerIo *ctx, uint32_t *start, uint32_t *end, uint32_t *bytes)
{
    if (!ctx || !ctx->completed_bytes)
        return false;
    *start = ctx->started;
    *end = ctx->ended;
    *bytes = ctx->completed_bytes;
    return true;
}
void raw_player_after_clock_reset(RawPlayerIo *ctx)
{
    storage_mutation_invalidate();
    if (ctx) {
        bool wanted = ctx->requested;
        /* Resume normally follows before_native, but independently retire any
         * live/READY view too. Cancel drains the provider before recapture;
         * keep the outstanding range so a pending prefetch can restart it. */
        raw_player_cancel(ctx);
        ctx->requested = wanted;
        ctx->started = counter();
        ctx->completed_bytes = 0;
    }
}
int raw_player_error(const RawPlayerIo *ctx) { return ctx ? ctx->error : create_error; }
uint32_t raw_player_file_bytes(const RawPlayerIo *ctx)
{
    return ctx ? (ctx->crypt ? ctx->crypt->keys->header.plain_bytes : ctx->snapshot.file_bytes) : 0U;
}
void raw_player_debug(FILE *file, const RawPlayerIo *ctx)
{
    if (!file)
        return;
    RawPlayerReport r = ctx ? report(ctx) : last_report;
    fprintf(
        file,
        "raw_reader error=%d refreshes=%lu handoffs=%lu steps=%lu foreground_ticks=%lu max_batch_ticks=%lu physical_reads=%lu regions=%lu cache_status=%d cache_nodes=%lu dirty_blocks=%lu metadata_hits=%lu\n",
        r.error, (unsigned long)r.refreshes, (unsigned long)r.handoffs, (unsigned long)r.steps,
        (unsigned long)r.foreground_ticks, (unsigned long)r.max_step_ticks,
        (unsigned long)r.physical, (unsigned long)r.regions, r.cache_status,
        (unsigned long)r.cache_nodes, (unsigned long)r.dirty_blocks,
        (unsigned long)r.metadata_hits);
    fprintf(file, "raw_reader_map_cache stores=%lu hits=%lu invalidations=%lu rejected=%lu\n",
            (unsigned long)r.region_cache.stores, (unsigned long)r.region_cache.hits,
            (unsigned long)r.region_cache.invalidations, (unsigned long)r.region_cache.rejected);
    fprintf(
        file,
        "storage_mutations scope=app_until_reader_teardown epoch=%lu observed=%lu programs=%lu erases=%lu unknown=%lu failures=%lu\n",
        (unsigned long)r.mutation_epoch, (unsigned long)r.mutations.observed,
        (unsigned long)r.mutations.programs, (unsigned long)r.mutations.erases,
        (unsigned long)r.mutations.unknown, (unsigned long)r.mutations.failures);
    fprintf(
        file,
        "raw_reader_costs scope=lifetime payload_reads=%lu rel_metadata_reads=%lu ffx_header_reads=%lu ffx_tag_reads=%lu ffx_bitmap_reads=%lu region_starts=%lu region_other=%lu native_seed_other=%lu\n",
        (unsigned long)r.costs.physical[RAW_READ_PAYLOAD],
        (unsigned long)r.costs.physical[RAW_READ_REL_METADATA],
        (unsigned long)r.costs.physical[RAW_READ_FFX_HEADER],
        (unsigned long)r.costs.physical[RAW_READ_FFX_TAG],
        (unsigned long)r.costs.physical[RAW_READ_FFX_BITMAP],
        (unsigned long)r.costs.regions_started, (unsigned long)r.costs.region_other,
        (unsigned long)r.native_seed_other);
    fprintf(file, "raw_reader_native_seed_last=%ld,%ld,%ld\n",
            (long)(r.refreshes ? (int32_t)r.native_seed_last[0] : -1),
            (long)(r.refreshes ? (int32_t)r.native_seed_last[1] : -1),
            (long)(r.refreshes ? (int32_t)r.native_seed_last[2] : -1));
    for (unsigned i = 0; i < RAW_FILE_REGION_HISTOGRAM_SLOTS; ++i)
        if (r.costs.region[i] || r.native_seed[i])
            fprintf(file, "raw_reader_region id=%u rebuilds=%lu native_seeds=%lu\n", i,
                    (unsigned long)r.costs.region[i], (unsigned long)r.native_seed[i]);
}
