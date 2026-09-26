#ifndef NDVIDEO_NATIVE_CURSOR_HOLD_H
#define NDVIDEO_NATIVE_CURSOR_HOLD_H
#include <stdbool.h>
/* Requires the exact native-task firmware gate and display-copy ownership.
 * One logical HideCursor reference is shared by all app contexts. */
int native_cursor_hold_acquire(bool *held);
int native_cursor_hold_release(bool *held);
int native_cursor_hold_reassert(void);
#endif
