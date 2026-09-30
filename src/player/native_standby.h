#ifndef NDVIDEO_NATIVE_STANDBY_H
#define NDVIDEO_NATIVE_STANDBY_H
#include <stdbool.h>
#include <stdio.h>
bool native_standby_supported(void);
int native_standby_preflight_status(void);
void native_standby_debug(FILE *file);
void native_standby_run(void);
int native_standby_status(void);
#endif
