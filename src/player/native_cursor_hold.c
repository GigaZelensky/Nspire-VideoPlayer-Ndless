#include "native_cursor_hold.h"
#include "native_interrupts.h"
#include <stdint.h>
#include <stddef.h>

static unsigned cursor_references;
enum { CURSOR_BAD_STATE = -1700, CURSOR_BAD_CODE = -1701, CURSOR_HIDE_FAILED = -1702 };

static uint32_t cursor_word(uintptr_t address)
{
    return *(const volatile uint32_t *)address;
}

static int cursor_count(void)
{
    return *(const volatile int16_t *)0x114882B8U;
}

static int cursor_call(uintptr_t address)
{
    return ((int (*)(void))address)();
}

static bool cursor_validate(void)
{
    static const struct {
        uintptr_t address;
        uint32_t words[4];
    } fingerprints[] = {{0x103CE000U, {0xE92D4010U, 0xE59F4044U, 0xE59F3044U, 0xE594001CU}},
                        {0x103CE0B8U, {0xE92D41F0U, 0xE59F41F0U, 0xE59F31F0U, 0xE594101CU}},
                        {0x10009570U, {0xE59F101CU, 0xE52DE004U, 0xE24DD02CU, 0xE3A03000U}},
                        {0x10009518U, {0xE92D4010U, 0xE1A04000U, 0xEBFFFFF1U, 0xE3500000U}},
                        {0x10009D1CU, {0xE92D4070U, 0xE1A04001U, 0xE3E0100EU, 0xE5841008U}}};
    for (unsigned i = 0; i < sizeof(fingerprints) / sizeof(*fingerprints); ++i)
        for (unsigned j = 0; j < 4; ++j)
            if (cursor_word(fingerprints[i].address + j * 4U) != fingerprints[i].words[j])
                return false;
    return cursor_word(0x103CE054U) == 0x114882B8U && cursor_word(0x103CE2C8U) == 0x114882B8U &&
           cursor_word(0x10009594U) == 1005U;
}

int native_cursor_hold_acquire(bool *held)
{
    if (!held || *held || cursor_references == UINT32_MAX)
        return CURSOR_BAD_STATE;
    if (!cursor_references && !cursor_validate())
        return CURSOR_BAD_CODE;
    unsigned saved = native_critical_enter();
    if (!cursor_references) {
        int count = cursor_count();
        if (count > 0 || count <= INT16_MIN) {
            native_critical_leave(saved);
            return CURSOR_BAD_STATE;
        }
        /* Balance exactly one native visibility reference. Touch/contact
         * show/hide pairs then remain below zero while this app owns it. */
        cursor_call(0x103CE000U);
    }
    ++cursor_references;
    *held = true;
    /* HideCursor only calls the device when crossing 0 -> -1. The loader
     * can leave device-visible=1 even with the overlay register hidden.
     * Clear the device state too, so image uploads/resume cannot re-show it. */
    int result = cursor_call(0x10009570U);
    native_critical_enter();
    bool hidden = cursor_count() < 0 && !(cursor_word(0xC0000C00U) & 1U);
    native_critical_leave(saved);
    return !result && hidden ? 0 : CURSOR_HIDE_FAILED;
}

int native_cursor_hold_reassert(void)
{
    if (!cursor_references)
        return CURSOR_BAD_STATE;
    unsigned saved = native_critical_enter();
    int result = cursor_call(0x10009570U);
    native_critical_enter();
    bool hidden = cursor_count() < 0 && !(cursor_word(0xC0000C00U) & 1U);
    native_critical_leave(saved);
    return !result && hidden ? 0 : CURSOR_HIDE_FAILED;
}

int native_cursor_hold_release(bool *held)
{
    if (!held)
        return CURSOR_BAD_STATE;
    if (!*held)
        return 0;
    if (!cursor_references)
        return CURSOR_BAD_STATE;
    unsigned saved = native_critical_enter();
    if (--cursor_references == 0)
        cursor_call(0x103CE0B8U);
    native_critical_enter();
    *held = false;
    native_critical_leave(saved);
    return 0;
}
