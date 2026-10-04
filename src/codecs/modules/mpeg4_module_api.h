#ifndef NDVIDEO_MPEG4_MODULE_API_H
#define NDVIDEO_MPEG4_MODULE_API_H

#include "../codec_module_api.h"
#include "../mpeg4_xvid.h"

typedef struct Mpeg4ModuleApi {
    bool (*global_init)(void *, unsigned int);
    bool (*create)(void **, int, int);
    void (*destroy)(void *);
    bool (*decode)(void *, const uint8_t *, size_t, uint16_t *, int, int, bool, bool);
    const char *(*last_error)(void);
} Mpeg4ModuleApi;

/* Call after all module contexts have gone, before replacing the SRAM arena. */
bool mpeg4_module_forget_sram(void);

#endif
