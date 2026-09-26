#ifndef NDVIDEO_NATIVE_STANDBY_H
#define NDVIDEO_NATIVE_STANDBY_H
#include <stdbool.h>
bool native_standby_supported(void);
void native_standby_run(void);
int native_standby_status(void);
#endif
