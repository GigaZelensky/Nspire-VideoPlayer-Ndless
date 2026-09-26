#include "native_display_hold.h"
#include "native_interrupts.h"

#include <stddef.h>

#define DISPLAY_DESCRIPTOR 0x10C4E550U
#define DISPLAY_STATE_POINTER (DISPLAY_DESCRIPTOR + 0x14U)
#define DISPLAY_CONTROL 0x100069E4U
#define DISPLAY_TIMER_MAGIC 0x54494D45U
#define DISPLAY_HISR_MAGIC 0x48495352U

static uintptr_t display_state;
static uint32_t hold_references;
static uint32_t original_enabled;
static bool validated;

static uint32_t display_word(uintptr_t address)
{
    return *(const volatile uint32_t *)address;
}

static int display_control(unsigned enable)
{
    int result;
    unsigned saved = native_critical_enter();
    /* The OS stop routine waits for DMA completion flags. Run its native
     * control with interrupts enabled, never through the Ndless SWI veneer. */
    native_critical_leave(0);
    result = ((int (*)(unsigned))(uintptr_t)DISPLAY_CONTROL)(enable);
    native_critical_enter();
    native_critical_leave(saved);
    return result;
}

static uint32_t current_enabled(void)
{
    return display_word(display_state + 0x58U);
}

static bool disabled_postcondition(void)
{
    return current_enabled() == 0U && (display_word(display_state + 0x54U) & 0xFFFFU) == 0U &&
           display_word(display_state + 0x1CU) == 0U && display_word(display_state + 0x90U) == 0U;
}

static bool enabled_postcondition(void)
{
    /* Verified native creation layouts: timer ID+0xC, active byte+0x20,
     * callback+0x18; HISR ID+0xC and entry+0x44. Timer/HISR objects start at
     * supervisor state+0x10/+0x84. enabled alone hides creation failures. */
    return current_enabled() == 1U && display_word(display_state + 0x1CU) == DISPLAY_TIMER_MAGIC &&
           (display_word(display_state + 0x30U) & 0xFFU) == 1U &&
           display_word(display_state + 0x28U) == 0x10006DE4U &&
           display_word(display_state + 0x90U) == DISPLAY_HISR_MAGIC &&
           display_word(display_state + 0xC8U) == 0x10006D7CU;
}

static int validate_display(void)
{
    static const struct {
        uintptr_t address;
        uint32_t words[4];
    } fingerprints[] = {{0x100069E4U, {0xE59F1020U, 0xE52DE004U, 0xE24DD02CU, 0xE58D000CU}},
                        {0x10006940U, {0xE92D4010U, 0xE1A04000U, 0xEBFFFFF1U, 0xE3500000U}},
                        {0x10006EF0U, {0xE3E0300EU, 0xE92D4070U, 0xE5813008U, 0xE590001CU}},
                        {0x10006D18U, {0xE92D4010U, 0xE5903058U, 0xE24DD010U, 0xE3530000U}},
                        {0x10006C90U, {0xE92D4010U, 0xE59F406CU, 0xE1A0C000U, 0xE24DD010U}},
                        {0x10006E44U, {0xE92D4070U, 0xE5903058U, 0xE1A04000U, 0xE3530000U}},
                        {0x10006E0CU, {0xE92D4010U, 0xE1A04000U, 0xE24DD008U, 0xE3A00016U}}};
    unsigned i, j;
    uintptr_t state;
    validated = false;
    for (i = 0; i < sizeof(fingerprints) / sizeof(fingerprints[0]); ++i)
        for (j = 0; j < 4U; ++j)
            if (display_word(fingerprints[i].address + j * 4U) != fingerprints[i].words[j])
                return NATIVE_DISPLAY_HOLD_UNSUPPORTED;
    if (display_word(DISPLAY_DESCRIPTOR + 0x0CU) != 0x494F4452U ||
        display_word(DISPLAY_DESCRIPTOR + 0x2CU) != 31U ||
        display_word(DISPLAY_DESCRIPTOR + 0x38U) != 0x10006EF0U)
        return NATIVE_DISPLAY_HOLD_UNSUPPORTED;
    state = display_word(DISPLAY_STATE_POINTER);
    if ((state & 3U) || state < 0x10000000U || state > 0x12000000U - (0xDCU + 2048U))
        return NATIVE_DISPLAY_HOLD_BAD_STATE;
    display_state = state;
    if (display_word(state) != 1U || display_word(state + 0x0CU) != 153600U ||
        current_enabled() > 1U)
        return NATIVE_DISPLAY_HOLD_BAD_STATE;
    if (current_enabled() && !enabled_postcondition())
        return NATIVE_DISPLAY_HOLD_BAD_STATE;
    if (!current_enabled() && !disabled_postcondition())
        return NATIVE_DISPLAY_HOLD_BAD_STATE;
    validated = true;
    return NATIVE_DISPLAY_HOLD_OK;
}

int native_display_hold_acquire(bool *held)
{
    int status;
    if (!held || *held)
        return NATIVE_DISPLAY_HOLD_BAD_STATE;
    if (hold_references) {
        if (!disabled_postcondition() || hold_references == UINT32_MAX)
            return NATIVE_DISPLAY_HOLD_BAD_STATE;
        ++hold_references;
        *held = true;
        return NATIVE_DISPLAY_HOLD_OK;
    }
    status = validate_display();
    if (status)
        return status;
    original_enabled = current_enabled();
    /* Ownership precedes mutation. Even partial native failure must pass
     * through release before the caller may drop its APD/input reservation. */
    hold_references = 1U;
    *held = true;
    if (original_enabled)
        display_control(0U);
    return disabled_postcondition() ? NATIVE_DISPLAY_HOLD_OK : NATIVE_DISPLAY_HOLD_DISABLE_FAILED;
}

int native_display_hold_reassert(void)
{
    if (!hold_references || !validated)
        return NATIVE_DISPLAY_HOLD_BAD_STATE;
    if (disabled_postcondition())
        return NATIVE_DISPLAY_HOLD_OK;
    if (!enabled_postcondition())
        return NATIVE_DISPLAY_HOLD_BAD_STATE;
    display_control(0U);
    return disabled_postcondition() ? NATIVE_DISPLAY_HOLD_OK : NATIVE_DISPLAY_HOLD_DISABLE_FAILED;
}

int native_display_hold_release(bool *held)
{
    if (!held)
        return NATIVE_DISPLAY_HOLD_BAD_STATE;
    if (!*held)
        return NATIVE_DISPLAY_HOLD_OK;
    if (!hold_references || !validated)
        return NATIVE_DISPLAY_HOLD_BAD_STATE;
    if (hold_references > 1U) {
        --hold_references;
        *held = false;
        return NATIVE_DISPLAY_HOLD_OK;
    }
    if (original_enabled) {
        if (disabled_postcondition())
            display_control(1U);
        /* Do not retry creation over a half-created native timer/HISR. Keep
         * ownership on an invalid enabled state rather than corrupt OS lists. */
        if (!enabled_postcondition())
            return NATIVE_DISPLAY_HOLD_RESTORE_FAILED;
    } else if (!disabled_postcondition()) {
        return NATIVE_DISPLAY_HOLD_RESTORE_FAILED;
    }
    hold_references = 0;
    *held = false;
    return NATIVE_DISPLAY_HOLD_OK;
}
