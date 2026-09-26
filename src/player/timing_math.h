#ifndef NDVIDEO_TIMING_MATH_H
#define NDVIDEO_TIMING_MATH_H
#include <stdint.h>

static inline void player_reduce_fps(uint16_t *numerator, uint16_t *denominator)
{
    uint32_t a = *numerator, b = *denominator;
    if (!a || !b)
        return;
    while (b) {
        uint32_t remainder = a % b;
        a = b;
        b = remainder;
    }
    *numerator = (uint16_t)(*numerator / a);
    *denominator = (uint16_t)(*denominator / a);
}

/* Common movie rates (16fps at 1x/2x) and the 32768Hz hardware clock reduce
 * to powers of two. Avoid ARM software 64-bit division in these hot paths. */
static inline uint64_t player_div_u64(uint64_t value, uint64_t divisor)
{
    if (!divisor)
        return 0;
    if ((divisor & (divisor - 1U)) == 0) {
        return value >> __builtin_ctzll(divisor);
    }
    return value / divisor;
}
#endif
