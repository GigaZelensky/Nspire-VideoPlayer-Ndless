#ifndef NDVIDEO_NSPIRE_HARDWARE_H
#define NDVIDEO_NSPIRE_HARDWARE_H
#include <stdbool.h>
#include <stdint.h>

/* Original CX units report both forms of the identification register.
 * Preserve the raw value for diagnostics; controller and memory-layout
 * validation remain separate from identifying the calculator family. */
static inline bool nspire_asic_is_cx(uint32_t id)
{
    return id == 0x00000101U || id == 0x10000101U;
}
#endif
