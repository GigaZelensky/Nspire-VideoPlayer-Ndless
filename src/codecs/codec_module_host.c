#if NDVIDEO_CODEC_MODULES
#include "codec_module_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/reent.h>
#include <math.h>
extern int __xpg_strerror_r(int, char *, size_t);

static void *resolve_host_symbol(const char *name)
{
#define SYMBOL(n)                                                                                  \
    if (!strcmp(name, #n))                                                                         \
    return (void *)(uintptr_t) & n
    SYMBOL(malloc);
    SYMBOL(calloc);
    SYMBOL(realloc);
    SYMBOL(free);
    SYMBOL(memcpy);
    SYMBOL(memmove);
    SYMBOL(memset);
    SYMBOL(memcmp);
    SYMBOL(strlen);
    SYMBOL(strcmp);
    SYMBOL(strncmp);
    SYMBOL(strchr);
    SYMBOL(strrchr);
    SYMBOL(strcspn);
    SYMBOL(vsnprintf);
    SYMBOL(vfprintf);
    SYMBOL(fflush);
    SYMBOL(fwrite);
    SYMBOL(fputc);
    SYMBOL(puts);
    SYMBOL(abort);
    SYMBOL(vsprintf);
    SYMBOL(__xpg_strerror_r);
    SYMBOL(getenv);
    SYMBOL(strtoul);
    SYMBOL(fputs);
#if NDVIDEO_WITH_MPEG4
#ifndef NDVIDEO_XVID_NO_POSTPROC
    SYMBOL(rand);
    SYMBOL(srand);
    SYMBOL(log);
    SYMBOL(sqrt);
#endif
    SYMBOL(vsscanf);
#endif
    SYMBOL(__errno);
#undef SYMBOL
    if (!strcmp(name, "_impure_ptr"))
        return &_impure_ptr;
    return NULL;
}

const CodecHostApi *codec_modules_default_host_api(void)
{
    static const CodecHostApi api = {CODEC_MODULE_ABI_VERSION, sizeof(CodecHostApi),
                                     resolve_host_symbol, NULL};
    return &api;
}
#endif
