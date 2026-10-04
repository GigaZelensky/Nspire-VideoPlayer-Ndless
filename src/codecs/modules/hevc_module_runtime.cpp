/* Module-local C++ termination registry. Never delegates to host atexit. */
#if defined(NDVIDEO_BUILD_HEVC_MODULE)
#include "hevc_module_runtime.h"
#include <stddef.h>
#include <stdint.h>

namespace {
struct ExitEntry {
    union {
        void (*plain)(void);
        void (*object)(void *);
    } function;
    void *argument;
    void *dso;
    uint64_t order;
    bool used;
    bool has_argument;
};
ExitEntry exits[64];
uint64_t next_order;
bool running;
bool failed;

ExitEntry *reserve_exit()
{
    if (running) {
        for (unsigned i = 0; i < sizeof(exits) / sizeof(exits[0]); ++i)
            if (!exits[i].used) {
                exits[i].used = true;
                exits[i].order = ++next_order;
                return &exits[i];
            }
    }
    failed = true;
    return NULL;
}
}

extern "C" int atexit(void (*function)(void)) noexcept
{
    if (!function)
        return -1;
    ExitEntry *entry = reserve_exit();
    if (!entry)
        return -1;
    entry->function.plain = function;
    entry->argument = entry->dso = NULL;
    entry->has_argument = false;
    return 0;
}

extern "C" int __cxa_atexit(void (*function)(void *), void *argument, void *dso) noexcept
{
    if (!function)
        return -1;
    ExitEntry *entry = reserve_exit();
    if (!entry)
        return -1;
    entry->function.object = function;
    entry->argument = argument;
    entry->dso = dso;
    entry->has_argument = true;
    return 0;
}

extern "C" int __aeabi_atexit(void *argument, void (*function)(void *), void *dso) noexcept
{
    return __cxa_atexit(function, argument, dso);
}

extern "C" void __cxa_finalize(void *dso) noexcept
{
    for (;;) {
        ExitEntry *last = NULL;
        for (unsigned i = 0; i < sizeof(exits) / sizeof(exits[0]); ++i)
            if (exits[i].used && (!dso || exits[i].dso == dso) &&
                (!last || exits[i].order > last->order))
                last = &exits[i];
        if (!last)
            break;
        ExitEntry entry = *last;
        last->used = false;
        /* Mark before calling: finalizers may recurse or register another. */
        if (entry.has_argument)
            entry.function.object(entry.argument);
        else
            entry.function.plain();
    }
}

extern "C" bool hevc_module_runtime_start(HevcModuleInitializer *first,
                                          HevcModuleInitializer *last)
{
    if (running)
        return false;
    running = true;
    failed = false;
    next_order = 0;
    for (; first != last && !failed; ++first)
        if (*first)
            (*first)();
    return !failed;
}

extern "C" void hevc_module_runtime_finish(HevcModuleInitializer *first,
                                          HevcModuleInitializer *last)
{
    if (!running)
        return;
    __cxa_finalize(NULL);
    while (last != first) {
        --last;
        if (*last)
            (*last)();
    }
    __cxa_finalize(NULL);
    running = false;
}

extern "C" bool hevc_module_runtime_failed(void)
{
    return failed;
}
#endif
