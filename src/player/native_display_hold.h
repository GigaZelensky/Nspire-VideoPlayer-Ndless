#ifndef NDVIDEO_NATIVE_DISPLAY_HOLD_H
#define NDVIDEO_NATIVE_DISPLAY_HOLD_H

#include <stdbool.h>
#include <stdint.h>

enum {
    NATIVE_DISPLAY_HOLD_OK = 0,
    NATIVE_DISPLAY_HOLD_UNSUPPORTED = -1300,
    NATIVE_DISPLAY_HOLD_BAD_STATE = -1301,
    NATIVE_DISPLAY_HOLD_DISABLE_FAILED = -1302,
    NATIVE_DISPLAY_HOLD_RESTORE_FAILED = -1303
};

/* Foreground only. Before ANY API here, the native backend
 * must pass its exact CX II-T/OS 6.2.0.333 firmware gate. Acquire/release also
 * require that backend's APD/input ownership. Module entry fingerprints provide
 * an additional check; they do not replace hardware/OS identification.
 * A context starts with *held=false. Acquire may return an error with *held
 * true after touching OS state: the caller MUST then retry release/cleanup.
 * Release clears *held only when that reference is released and any required
 * original display state has been restored. Failed release retains ownership.
 * The last release belongs after native standby, before APD/input release.
 * Native display control briefly runs with IRQ/FIQ enabled so DMA completion
 * can execute; caller's exact interrupt mask is restored before return. */
int native_display_hold_acquire(bool *held);
int native_display_hold_release(bool *held);
/* Once after native driver resume, retaining the original restore policy. */
int native_display_hold_reassert(void);

#endif
