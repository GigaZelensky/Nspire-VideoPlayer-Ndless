#ifndef NDVIDEO_HEVC_MODULE_RUNTIME_H
#define NDVIDEO_HEVC_MODULE_RUNTIME_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*HevcModuleInitializer)(void);
bool hevc_module_runtime_start(HevcModuleInitializer *, HevcModuleInitializer *);
void hevc_module_runtime_finish(HevcModuleInitializer *, HevcModuleInitializer *);
bool hevc_module_runtime_failed(void);
#ifdef __cplusplus
}
#endif
#endif
