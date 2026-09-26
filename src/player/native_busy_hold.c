#include "native_busy_hold.h"
#include "native_interrupts.h"

#include <stddef.h>

#define BUSY_TIMER_POINTER 0x113C4294U
#define BUSY_SPINNER_ACTIVE 0x113C429CU
#define BUSY_SAVED_VISIBILITY 0x113C4298U
#define BUSY_INTERVAL 0x10C536F4U
#define BUSY_CURSOR_CONTROL 0xC0000C00U
#define BUSY_CALLBACK 0x10020288U
#define BUSY_STOP 0x10020134U
#define BUSY_START 0x1002022CU
#define BUSY_TIMER_MAGIC 0x54494D45U

static uint32_t busy_references;
static uint32_t busy_original_running;
static uint32_t busy_original_cursor;
static bool busy_validated;

static uint32_t busy_word(uintptr_t address)
{
    return *(const volatile uint32_t *)address;
}

static void busy_write_cursor(bool hide)
{
    unsigned saved = native_critical_enter();
    uint32_t value = hide ? busy_original_cursor & ~1U : busy_original_cursor;
    *(volatile uint32_t *)BUSY_CURSOR_CONTROL = value;
    native_critical_leave(saved);
}

static void busy_control(bool start)
{
    unsigned saved = native_interrupt_mask();
    /* The verified direct-callback timer has mode+0x44=0. Its native deletion
     * never takes the queued-event sleep/drain branch. Preserve the loader's
     * IRQ policy; unlike display DMA stop, this operation needs no IRQ span. */
    ((void (*)(void))(uintptr_t)(start ? BUSY_START : BUSY_STOP))();
    native_critical_enter();
    native_critical_leave(saved);
}

static uint32_t busy_section_descriptor(uintptr_t address)
{
#if defined(__arm__)
    uint32_t ttbr;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr));
    return ((const volatile uint32_t *)(uintptr_t)(ttbr & 0xFFFFC000U))[address >> 20];
#else
    (void)address;
    return 0;
#endif
}

static bool busy_timer_memory(uintptr_t timer)
{
    uintptr_t section, last;
    /* CX II has the 64 MiB SDRAM class (Ndless Zehn RAM gate), not the 32 MiB
     * extent of the supplied dump. Also verify every covered live L1 section
     * is identity-mapped RAM writable in the native SVC context. Cacheability
     * is not required: an OS pool may legitimately use uncached RAM. */
    if ((timer & 3U) || timer < 0x10000000U || timer > 0x14000000U - 96U)
        return false;
    section = timer & ~(uintptr_t)0xFFFFFU;
    last = (timer + 95U) & ~(uintptr_t)0xFFFFFU;
    for (;;) {
        uint32_t descriptor = busy_section_descriptor(section);
        if ((descriptor & 3U) != 2U || (descriptor & 0xFFF00000U) != section ||
            !(descriptor & 0xC00U))
            return false;
        if (section == last)
            return true;
        section += 0x100000U;
    }
}

static bool busy_timer_valid(uintptr_t timer)
{
    return busy_timer_memory(timer) && busy_word(timer + 0x0CU) == BUSY_TIMER_MAGIC &&
           busy_word(timer + 0x18U) == BUSY_CALLBACK && busy_word(timer + 0x1CU) == 0U &&
           busy_word(timer + 0x44U) == 0U && (busy_word(timer + 0x20U) & 0xFFU) <= 1U &&
           busy_word(timer + 0x50U) <= 1U;
}

static bool busy_timer_running(uintptr_t timer)
{
    return busy_timer_valid(timer) && (busy_word(timer + 0x20U) & 0xFFU) == 1U &&
           busy_word(timer + 0x50U) == 1U;
}

static int busy_validate(void)
{
    static const struct {
        uintptr_t address;
        uint32_t words[4];
    } fingerprints[] = {{BUSY_STOP, {0xE92D41F0U, 0xE59F50DCU, 0xE24DD008U, 0xE5950004U}},
                        {BUSY_START, {0xE92D4010U, 0xE59F4044U, 0xE5940004U, 0xE3500000U}},
                        {BUSY_CALLBACK, {0xE92D4070U, 0xE59F40C8U, 0xE5943004U, 0xE3530000U}},
                        {0x1002101CU, {0xE92D4010U, 0xE3A01004U, 0xE1A04000U, 0xEB0E1C0EU}},
                        {0x100212D0U, {0xE92D4030U, 0xE2504000U, 0xE24DD00CU, 0x1A000002U}},
                        {0x10021360U, {0xE3510009U, 0xE92D40F0U, 0xE1A05001U, 0xE24DD014U}}};
    unsigned i, j;
    uintptr_t timer;
    busy_validated = false;
    for (i = 0; i < sizeof(fingerprints) / sizeof(fingerprints[0]); ++i)
        for (j = 0; j < 4U; ++j)
            if (busy_word(fingerprints[i].address + j * 4U) != fingerprints[i].words[j])
                return NATIVE_BUSY_HOLD_UNSUPPORTED;
    /* Pointer/policy literals in begin/end establish the globals used below. */
    if (busy_word(0x1002021CU) != 0x113C4290U || busy_word(0x1002027CU) != 0x113C4290U ||
        busy_word(0x10020280U) != BUSY_INTERVAL || busy_word(0x10020284U) != BUSY_CALLBACK)
        return NATIVE_BUSY_HOLD_UNSUPPORTED;
    timer = busy_word(BUSY_TIMER_POINTER);
    if ((timer && !busy_timer_valid(timer)) ||
        (busy_timer_running(timer) &&
         (busy_word(BUSY_INTERVAL) < 10U || busy_word(BUSY_INTERVAL) > INT32_MAX)) ||
        busy_word(BUSY_SPINNER_ACTIVE) > 1U)
        return NATIVE_BUSY_HOLD_BAD_STATE;
    busy_validated = true;
    return NATIVE_BUSY_HOLD_OK;
}

int native_busy_hold_acquire(bool *held)
{
    int status;
    uintptr_t timer;
    if (!held || *held)
        return NATIVE_BUSY_HOLD_BAD_STATE;
    if (busy_references) {
        if (busy_references == UINT32_MAX || busy_word(BUSY_TIMER_POINTER) != 0U ||
            busy_word(BUSY_SPINNER_ACTIVE) != 0U)
            return NATIVE_BUSY_HOLD_BAD_STATE;
        ++busy_references;
        *held = true;
        return NATIVE_BUSY_HOLD_OK;
    }
    status = busy_validate();
    if (status)
        return status;
    timer = busy_word(BUSY_TIMER_POINTER);
    busy_original_running = busy_timer_running(timer);
    busy_original_cursor = busy_word(BUSY_CURSOR_CONTROL);
    busy_references = 1U;
    *held = true;
    if (timer || busy_word(BUSY_SPINNER_ACTIVE))
        busy_control(false);
    /* End restores logical OS cursor visibility, which can differ from the
     * loader's hidden hardware cursor. Hide once while preserving other bits;
     * release restores the complete saved register, including a visible entry. */
    busy_write_cursor(true);
    return busy_word(BUSY_TIMER_POINTER) == 0U && busy_word(BUSY_SPINNER_ACTIVE) == 0U &&
                   busy_word(BUSY_CURSOR_CONTROL) == (busy_original_cursor & ~1U)
               ? NATIVE_BUSY_HOLD_OK
               : NATIVE_BUSY_HOLD_STOP_FAILED;
}

int native_busy_hold_reassert(void)
{
    if (!busy_references || !busy_validated)
        return NATIVE_BUSY_HOLD_BAD_STATE;
    uintptr_t timer = busy_word(BUSY_TIMER_POINTER);
    if (timer && !busy_timer_valid(timer))
        return NATIVE_BUSY_HOLD_BAD_STATE;
    if (timer || busy_word(BUSY_SPINNER_ACTIVE))
        busy_control(false);
    busy_write_cursor(true);
    return !busy_word(BUSY_TIMER_POINTER) && !busy_word(BUSY_SPINNER_ACTIVE)
               ? NATIVE_BUSY_HOLD_OK
               : NATIVE_BUSY_HOLD_STOP_FAILED;
}

int native_busy_hold_release(bool *held)
{
    uintptr_t timer;
    if (!held)
        return NATIVE_BUSY_HOLD_BAD_STATE;
    if (!*held)
        return NATIVE_BUSY_HOLD_OK;
    if (!busy_references || !busy_validated)
        return NATIVE_BUSY_HOLD_BAD_STATE;
    if (busy_references > 1U) {
        --busy_references;
        *held = false;
        return NATIVE_BUSY_HOLD_OK;
    }
    timer = busy_word(BUSY_TIMER_POINTER);
    if (busy_original_running) {
        if (!timer)
            busy_control(true);
        timer = busy_word(BUSY_TIMER_POINTER);
        if (!busy_timer_running(timer))
            return NATIVE_BUSY_HOLD_RESTORE_FAILED;
    } else if (timer) {
        if (!busy_timer_valid(timer))
            return NATIVE_BUSY_HOLD_RESTORE_FAILED;
        busy_control(false);
        if (busy_word(BUSY_TIMER_POINTER))
            return NATIVE_BUSY_HOLD_RESTORE_FAILED;
    }
    /* The hold never writes the OS's interval policy. Native begin uses its
     * current value, preserving any legitimate policy change while held. */
    busy_write_cursor(false);
    if (busy_word(BUSY_CURSOR_CONTROL) != busy_original_cursor)
        return NATIVE_BUSY_HOLD_RESTORE_FAILED;
    busy_references = 0;
    *held = false;
    return NATIVE_BUSY_HOLD_OK;
}
