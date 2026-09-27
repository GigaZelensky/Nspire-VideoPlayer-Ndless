#ifndef NDVIDEO_SECURE_ZERO_H
#define NDVIDEO_SECURE_ZERO_H
#include <stddef.h>
/* Volatile writes: also used by the vendored primitives to erase workspaces. */
void nve_wipe(void *data, size_t bytes);
#endif
