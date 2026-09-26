#include "native_runtime_stats.h"
#include "native_interrupts.h"
#include "../platform/portable_reader_platform.h"
#include "portable_heap_profile.h"
#include <stddef.h>

static PortableReaderPlatform runtime_platform;
static int runtime_gate = -1;

static bool code_span(const PortableView *v, uint32_t address, uint32_t bytes)
{
    return !(address & 3U) && bytes && address >= v->code_begin &&
           bytes <= v->code_end - v->code_begin && address <= v->code_end - bytes &&
           v->allow_span(v->context, address, bytes, PORTABLE_CODE);
}
static bool profile(const PortableView *v, uint32_t address, const uint32_t *pattern, unsigned n)
{
    if (!code_span(v, address, n * 4U))
        return false;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t actual, mask = UINT32_MAX, expected = pattern[i];
        if (!v->read_word(v->context, address + 4U * i, &actual))
            return false;
        if ((expected & 0x0f000000U) == 0x0b000000U)
            mask = 0xff000000U;
        else if ((expected & 0x0f7f0000U) == 0x051f0000U)
            mask = 0xfffff000U;
        if ((actual & mask) != (expected & mask))
            return false;
    }
    return true;
}
static bool supported(void)
{
    if (runtime_gate >= 0)
        return runtime_gate != 0;
    /* Heap support is independent of the portable file/writer capabilities.
     * Even if an unrelated file family is unsupported, only the independently
     * proven malloc/pool capability is used here. No task fields are sampled. */
    (void)portable_reader_platform_init(&runtime_platform);
    unsigned saved = native_critical_enter();
    runtime_gate = 0;
    if ((runtime_platform.exports.capabilities & PORTABLE_HEAP_GLOBAL) &&
        portable_reader_platform_refresh(&runtime_platform)) {
        const PortableView *v = portable_reader_platform_view(&runtime_platform);
        uint32_t malloc_entry = runtime_platform.anchors.malloc, instruction;
        if (v && malloc_entry <= UINT32_MAX - 0x40U && code_span(v, malloc_entry + 0x40U, 4U) &&
            v->read_word(v->context, malloc_entry + 0x40U, &instruction) &&
            (instruction & 0xff000000U) == 0xeb000000U) {
            int64_t target = (int64_t)malloc_entry + 0x48U + ((int32_t)(instruction << 8) >> 6);
            /* The adjacent creation function's full body proves pool start,
             * total and free fields; the allocator proves the free counter's
             * byte accounting. Relative placement is checked by code content. */
            if (target >= 0x14cU && target <= UINT32_MAX &&
                profile(v, (uint32_t)target, heap_allocate, sizeof(heap_allocate) / 4U) &&
                profile(v, (uint32_t)target - 0x14cU, heap_create, sizeof(heap_create) / 4U))
                runtime_gate = 1;
        }
    }
    native_critical_leave(saved);
    return runtime_gate != 0;
}
void native_runtime_stats_snapshot(NativeRuntimeStats *snapshot)
{
    if (!snapshot)
        return;
    *snapshot = (NativeRuntimeStats){0};
    snapshot->status = NATIVE_RUNTIME_STATS_UNSUPPORTED;
    if (!supported())
        return;
    unsigned saved = native_critical_enter();
    snapshot->status = NATIVE_RUNTIME_STATS_PARTIAL;
    if (portable_reader_platform_refresh(&runtime_platform)) {
        const PortableView *v = portable_reader_platform_view(&runtime_platform);
        uint32_t pool;
        if (v && v->read_word(v->context, runtime_platform.exports.heap_global, &pool)) {
            snapshot->pool_address = pool;
            if (!(pool & 3U) && v->allow_span(v->context, pool, 48U, PORTABLE_DATA)) {
                const volatile uint32_t *p = (const volatile uint32_t *)(uintptr_t)pool;
                if (p[5] == 0x44594e41U) {
                    uint32_t start = p[8], total = p[9], available = p[11];
                    if (start >= v->ram_begin && start < v->ram_end && total &&
                        total <= v->ram_end - start && available <= total) {
                        snapshot->dynamic_pool_total_bytes = total;
                        snapshot->dynamic_pool_available_bytes = available;
                        snapshot->valid_flags |= NATIVE_RUNTIME_STATS_DYNAMIC_POOL;
                        snapshot->status = NATIVE_RUNTIME_STATS_OK;
                    }
                }
            }
        }
    }
    native_critical_leave(saved);
}
