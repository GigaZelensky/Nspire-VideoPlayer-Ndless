#ifndef NDVIDEO_PERFORMANCE_CLOCK_H
#define NDVIDEO_PERFORMANCE_CLOCK_H
#include <stdbool.h>
#include <stdio.h>

/* Startup/exit and native-standby boundaries only, with original OS SRAM
 * mapped and app I/O drained. Never call from a playback frame or writer. */
bool performance_clock_start(void);
void performance_clock_restore(void);
void performance_clock_debug(FILE *file);
#endif
