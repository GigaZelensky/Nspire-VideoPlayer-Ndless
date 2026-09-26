#ifndef NDVIDEO_NATIVE_RUNTIME_STATS_H
#define NDVIDEO_NATIVE_RUNTIME_STATS_H

#include <stdint.h>

enum { NATIVE_RUNTIME_STATS_DYNAMIC_POOL = 1U << 0 };
enum {
    NATIVE_RUNTIME_STATS_OK = 0,
    NATIVE_RUNTIME_STATS_UNSUPPORTED = -1600,
    NATIVE_RUNTIME_STATS_PARTIAL = -1601
};

typedef struct {
    uint32_t valid_flags;
    uint32_t pool_address;
    uint32_t dynamic_pool_total_bytes, dynamic_pool_available_bytes;
    int status;
} NativeRuntimeStats;

/* Foreground only. Proves the heap-layout code family once, then takes a constant-size
 * raw IRQ/FIQ-protected read. No allocation, pool traversal or native service
 * call is made by sampling. Missing fields have clear validity bits.
 * The DYNA counter excludes the separate small fixed-partition pools; it is
 * neither total free RAM nor the largest allocation that can succeed. */
void native_runtime_stats_snapshot(NativeRuntimeStats *snapshot);

#endif
