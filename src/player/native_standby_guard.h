#ifndef NDVIDEO_NATIVE_STANDBY_GUARD_H
#define NDVIDEO_NATIVE_STANDBY_GUARD_H
#include <stdbool.h>
#include <stdio.h>
/* Used only inside the explicit native-standby bridge with genuine SRAM.
 * Retains ownership after wake until final shutdown, so driver resume cannot
 * repaint the application. Partial acquisition/release requires retry. */
typedef struct {
    bool supported, active, apd_held, input_held, display_held, busy_held, cursor_held;
} NativeStandbyGuard;
const char *native_standby_guard_stage(void);
void native_standby_guard_debug(FILE *file);
int native_standby_guard_acquire(NativeStandbyGuard *);
int native_standby_guard_release(NativeStandbyGuard *);
void native_standby_guard_reassert_input(void);
#endif
