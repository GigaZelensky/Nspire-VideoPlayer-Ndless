#ifndef NDVIDEO_NATIVE_BUSY_HOLD_H
#define NDVIDEO_NATIVE_BUSY_HOLD_H

#include <stdbool.h>
#include <stdint.h>

enum {
    NATIVE_BUSY_HOLD_OK = 0,
    NATIVE_BUSY_HOLD_UNSUPPORTED = -1400,
    NATIVE_BUSY_HOLD_BAD_STATE = -1401,
    NATIVE_BUSY_HOLD_STOP_FAILED = -1402,
    NATIVE_BUSY_HOLD_RESTORE_FAILED = -1403
};

/* Foreground only. The backend's exact hardware/OS gate is required before
 * every API here. APD/input ownership must precede acquire.
 * Acquire precedes display hold (which can enable interrupts). Last release
 * follows display restoration, before APD/input release.
 * A context initializes *held=false. Any error with *held=true retains ownership
 * and requires release/cleanup retry. Only successful release clears it.
 * Native begin/end use the caller's interrupt policy and restore its exact mask.
 * Originally stopped timers stay stopped. Restart uses the unchanged native
 * interval policy and a fresh initial delay, matching the OS begin operation. */
int native_busy_hold_acquire(bool *held);
int native_busy_hold_release(bool *held);
int native_busy_hold_reassert(void);

#endif
