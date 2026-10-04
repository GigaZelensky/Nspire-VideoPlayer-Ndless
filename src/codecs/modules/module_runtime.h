#ifndef NDVIDEO_MODULE_RUNTIME_H
#define NDVIDEO_MODULE_RUNTIME_H
#include "codecs/codec_module_api.h"
#ifdef __cplusplus
extern "C" {
#endif
bool module_runtime_bind(const CodecHostApi *host);
void module_runtime_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
