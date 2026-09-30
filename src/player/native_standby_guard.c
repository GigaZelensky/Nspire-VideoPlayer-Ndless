#include "native_standby_guard.h"
#include "native_file_io.h"
#include "native_interrupts.h"
#include "native_firmware.h"
#include "native_display_hold.h"
#include "native_busy_hold.h"
#include "native_cursor_hold.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void (*apd_allow)(unsigned);
    void (*dim_disable)(unsigned);
    void (*event_mask_set)(unsigned);
} StandbyGuardApi;
static const StandbyGuardApi guard_api = {(void (*)(unsigned))0x10101EBCU,
                                          (void (*)(unsigned))0x101022A0U,
                                          (void (*)(unsigned))0x1042C0A0U};
static unsigned input_holders, saved_input_bits, saved_tracking_bit;
static uint32_t guard_word(uintptr_t address)
{
    return *(const volatile uint32_t *)address;
}
static bool guard_platform(void)
{
    static const struct {
        uintptr_t address;
        uint32_t word[4];
    } fingerprints[] = {
        {0x10101EBCU, {0xE92D4030U, 0xE59F40B0U, 0xE24DD014U, 0xE5943000U}},
        {0x101022A0U, {0xE92D4010U, 0xE59FC054U, 0xE59FE054U, 0xE24DD008U}},
        {0x1042C0A0U, {0xE59F3004U, 0xE1C300B0U, 0xE12FFF1EU, 0x114882BAU}},
        {0x1042C95CU, {0xE59F1018U, 0xE5D03004U, 0xE1D120F0U, 0xE0123003U}},
        {0x1042BFF0U, {0xE92D4038U, 0xE59F5080U, 0xE1A04000U, 0xE1D530B0U}},
        {0x1042C028U, {0xE3130008U, 0x08BD8038U, 0xE5D40004U, 0xE3100001U}},
        {0x1042CF5CU, {0xE59F300CU, 0xE1D300B0U, 0xE3C02008U, 0xE1C320B0U}},
    };
    for (unsigned i = 0; i < sizeof(fingerprints) / sizeof(*fingerprints); ++i)
        for (unsigned j = 0; j < 4; ++j)
            if (guard_word(fingerprints[i].address + 4U * j) != fingerprints[i].word[j])
                return false;
    return guard_word(0x1042C07CU) == 0x11488388U && guard_word(0x1042CF70U) == 0x11488388U;
}
static unsigned native_event_mask(void)
{
    /* Address is the literal in the fingerprinted native setter above. */
    return *(const volatile unsigned short *)0x114882BAU;
}

static unsigned native_tracking_flags(void)
{
    return *(const volatile unsigned short *)0x11488388U;
}

static void native_tracking_write(unsigned flags)
{
    *(volatile unsigned short *)0x11488388U = (unsigned short)flags;
}

static void hold_native_input(NativeStandbyGuard *backend)
{
    unsigned saved = native_critical_enter();
    if (!input_holders) {
        unsigned mask = native_event_mask();
        saved_input_bits = mask & 0x1FU;
        unsigned tracking = native_tracking_flags();
        saved_tracking_bit = tracking & 8U;
        /* Pointer motion has a direct tracking callback AFTER the event-mask
         * filter. Native TrackCursor(0) clears this exact bit. Own only that
         * bit so existing callback/device configuration survives restoration. */
        native_tracking_write(tracking & ~8U);
        guard_api.event_mask_set(mask & ~0x1FU);
    }
    ++input_holders;
    backend->input_held = true;
    native_critical_leave(saved);
}

static void release_native_input(NativeStandbyGuard *backend)
{
    if (backend->input_held) {
        unsigned saved = native_critical_enter();
        if (input_holders && --input_holders == 0) {
            unsigned mask = native_event_mask();
            /* Preserve non-input changes made by native services during standby. */
            guard_api.event_mask_set((mask & ~0x1FU) | saved_input_bits);
            native_tracking_write((native_tracking_flags() & ~8U) | saved_tracking_bit);
        }
        backend->input_held = false;
        native_critical_leave(saved);
    }
}

static int release_native_ownership(NativeStandbyGuard *backend)
{
    int result;
    if (backend->cursor_held) {
        result = native_cursor_hold_release(&backend->cursor_held);
        if (result)
            return result;
    }
    if (backend->display_held) {
        result = native_display_hold_release(&backend->display_held);
        if (result)
            return result;
    }
    if (backend->busy_held) {
        result = native_busy_hold_release(&backend->busy_held);
        if (result)
            return result;
    }
    if (backend->apd_held) {
        guard_api.apd_allow(1U);
        backend->apd_held = false;
    }
    release_native_input(backend);
    if (backend->active) {
        unsigned saved = native_critical_enter();
        backend->active = false;
        native_critical_leave(saved);
    }
    return 0;
}

static uint32_t acquire_task, acquire_magic, acquire_flags, acquire_slice, acquire_mask;
static void capture_acquire_state(void)
{
    acquire_task = acquire_magic = acquire_flags = acquire_slice = acquire_mask = 0;
    unsigned saved = native_critical_enter();
    PortableReaderPlatform mapping = {0};
    if (!portable_reader_platform_refresh(&mapping)) goto done;
    const PortableView *view = portable_reader_platform_view(&mapping);
    if (!view || !view->allow_span(view->context, 0x1148F060U, 0x24U, PORTABLE_DATA)) goto done;
    acquire_task = guard_word(0x1148F060U);
    acquire_mask = guard_word(0x1148F080U);
    if ((acquire_task & 3U) || !view->allow_span(view->context, acquire_task, 0x48U, PORTABLE_DATA)) goto done;
    acquire_magic = guard_word(acquire_task + 0x0CU);
    acquire_flags = guard_word(acquire_task + 0x18U);
    acquire_slice = guard_word(acquire_task + 0x40U);
done:
    native_critical_leave(saved);
}
void native_standby_guard_debug(FILE *file)
{
    if (file) fprintf(file, "guard_task=%08lx magic=%08lx flags=%08lx slice=%lu kernel_mask=%08lx\n",
        (unsigned long)acquire_task, (unsigned long)acquire_magic, (unsigned long)acquire_flags,
        (unsigned long)acquire_slice, (unsigned long)acquire_mask);
}

static const char *acquire_stage = "not-attempted";
const char *native_standby_guard_stage(void) { return acquire_stage; }

int native_standby_guard_acquire(NativeStandbyGuard *backend)
{
    int result;
    NativeFileIo validation = {0};
    acquire_stage = "entry-state";
    if (!backend || backend->apd_held || backend->input_held || backend->display_held ||
        backend->busy_held || backend->active || backend->cursor_held)
        return NATIVE_FILE_IO_BAD_STATE;
    capture_acquire_state();
    acquire_stage = "file-adapter";
    result = native_file_io_init(&validation);
    if (result)
        return result;
    acquire_stage = "guard-fingerprint";
    if (!guard_platform())
        return NATIVE_FILE_IO_UNSUPPORTED;
    backend->supported = true;
    /* Verified TI_PM_APD_allow boolean/refcount ABI. The OS itself brackets
     * long operations with allow(0)/allow(1). Hold before standby/display control can enable
     * interrupts; this also stops OS autodimming. The player keeps its own
     * idle policy after wake until final shutdown. */
    /* TI_PM_DIM_disable(0), also used by the OS APD callback, clears its dim
     * flag without restoring the OS brightness. The following APD hold's
     * dim-disable(1) therefore cannot undo the player's selected brightness. */
    /* The player polls physical keys and the touchpad. Avoid accumulating OS input
     * events while its GUI task is occupied; only pointer/key classes 1/2/4/8/16 are masked. */
    hold_native_input(backend);
    guard_api.dim_disable(0U);
    guard_api.apd_allow(0U);
    backend->apd_held = true;
    /* Retain ownership while display control temporarily runs the OS. Partial
     * acquisition/restore failures must finish cleanup before release. */
    {
        unsigned saved = native_critical_enter();
        backend->active = true;
        native_critical_leave(saved);
    }
    /* Keep the OS mirror-to-LCD DMA disabled across standby/resume. */
    /* The GUI's current event is this Ndless application. Stop its busy timer
     * before the display-control and standby spans enable OS interrupts. */
    acquire_stage = "busy-cursor";
    result = native_busy_hold_acquire(&backend->busy_held);
    if (!result) {
        acquire_stage = "display-copier";
        result = native_display_hold_acquire(&backend->display_held);
    }
    if (!result) {
        acquire_stage = "pointer-cursor";
        result = native_cursor_hold_acquire(&backend->cursor_held);
    }
    if (result) {
        int cleanup = release_native_ownership(backend);
        return cleanup ? cleanup : result;
    }
    acquire_stage = "ready";
    return 0;
}

void native_standby_guard_reassert_input(void)
{
    unsigned saved = native_critical_enter();
    if (input_holders) {
        guard_api.event_mask_set(native_event_mask() & ~0x1FU);
        native_tracking_write(native_tracking_flags() & ~8U);
    }
    native_critical_leave(saved);
}

int native_standby_guard_release(NativeStandbyGuard *backend)
{
    if (!backend)
        return NATIVE_FILE_IO_BAD_STATE;
    int result = release_native_ownership(backend);
    if (!result)
        backend->supported = false;
    return result;
}
