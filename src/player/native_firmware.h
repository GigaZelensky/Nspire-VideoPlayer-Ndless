#ifndef NDVIDEO_NATIVE_FIRMWARE_H
#define NDVIDEO_NATIVE_FIRMWARE_H

#include "native_interrupts.h"
#include "../platform/portable_reader_platform.h"

/* The private writer/power adapters below describe a CX II driver layout,
 * not an OS version. First prove that its code and globals can be read under
 * the current MMU mapping; each adapter then checks its instruction/data
 * fingerprints before using any native address. This grants no capability
 * by itself and never starts the filesystem resolver or an OS service. */
static inline bool native_firmware_memory(void)
{
    if (*(const volatile uint32_t *)(uintptr_t)0x900A0000U != 0x202U)
        return false;
    PortableReaderPlatform mapping = {0};
    unsigned saved = native_critical_enter();
    bool valid = portable_reader_platform_refresh(&mapping);
    if (valid) {
        const PortableView *view = portable_reader_platform_view(&mapping);
        valid = view && view->allow_span(view->context, 0x10000000U, 0x01490000U, PORTABLE_DATA);
    }
    native_critical_leave(saved);
    return valid;
}

#endif
