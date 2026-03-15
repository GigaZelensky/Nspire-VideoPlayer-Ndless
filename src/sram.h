#ifndef NDVIDEO_SRAM_H
#define NDVIDEO_SRAM_H

#include <stdbool.h>
#include <stddef.h>

bool sram_init(void);
void sram_shutdown(void);
void *sram_alloc(size_t size, size_t alignment);
bool sram_is_enabled(void);
size_t sram_bytes_used(void);
size_t sram_bytes_capacity(void);
const char *sram_status_message(void);

#endif
