#include "native_screen_power.h"
#include "native_interrupts.h"
#include "native_firmware.h"

#include <stddef.h>
#include <os.h>

#define SCREEN_OFF_ENTRY 0x10004BE0U
#define SCREEN_ON_ENTRY 0x10004BB8U
#define SCREEN_LCD_CONTROL 0xC0000018U
#define SCREEN_PWM_DUTY 0x90130014U
#define SCREEN_PWM_PERIOD 0x90130018U
#define SCREEN_PWM_CONTROL 0x90130020U
#define SCREEN_LED_SUPPLY 0x90140800U
#define SCREEN_LED_SUPPLY_BIT 0x10U
#define SCREEN_SUPERVISOR_STATE 0x113C1348U
#define SCREEN_LCD_ENABLE_BITS 0x801U
#define SCREEN_PWM_BLANK 0xFFFFFU

static bool screen_restore_required;
static uint32_t screen_saved_pwm_mode;
static uint32_t screen_saved_lcd_control;
static int screen_last_status, screen_native_result;
static int screen_platform_gate = -1;

static uint32_t screen_word(uintptr_t address)
{
    return *(const volatile uint32_t *)address;
}

static void screen_set_duty(uint32_t value)
{
    *(volatile uint32_t *)SCREEN_PWM_DUTY = value;
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
}

static bool screen_validate_platform(void)
{
    static const struct {
        uintptr_t address;
        uint32_t words[4];
    } fingerprints[] = {{SCREEN_OFF_ENTRY, {0xE59F101CU, 0xE52DE004U, 0xE24DD02CU, 0xE3A03000U}},
                        {SCREEN_ON_ENTRY, {0xE59F101CU, 0xE52DE004U, 0xE24DD02CU, 0xE3A03000U}},
                        {0x10004A74U, {0xE92D4010U, 0xE1A04000U, 0xEBFFFFF1U, 0xE3500000U}},
                        {0x10006EF0U, {0xE3E0300EU, 0xE92D4070U, 0xE5813008U, 0xE590001CU}},
                        {0x10004E48U, {0xE3E0300EU, 0xE92D41F0U, 0xE5813008U, 0xE590001CU}},
                        {0x10004C58U, {0xE59F301CU, 0xE3520000U, 0xE3A02000U, 0xE5832020U}},
                        {0x100012A4U, {0xE59F2028U, 0xE3833010U, 0xE5823800U, 0xE12FFF1EU}},
                        {0x10001330U, {0xE59F2028U, 0xE3C33010U, 0xE5823800U, 0xE12FFF1EU}}};
    unsigned i, j;
    if (!native_firmware_memory())
        return false;
    for (i = 0; i < sizeof(fingerprints) / sizeof(fingerprints[0]); ++i)
        for (j = 0; j < 4U; ++j)
            if (screen_word(fingerprints[i].address + j * 4U) != fingerprints[i].words[j])
                return false;
    return screen_word(0x10004C04U) == 1001U && screen_word(0x10004BDCU) == 1002U &&
           screen_word(0x10C4E55CU) == 0x494F4452U &&
           screen_word(0x10C4E564U) == SCREEN_SUPERVISOR_STATE && screen_word(0x10C4E57CU) == 31U &&
           screen_word(0x10C4E588U) == 0x10006EF0U && screen_word(0x10004C7CU) == 0x90130000U &&
           screen_word(0x100012D4U) == 0x90140000U && screen_word(0x10001360U) == 0x90140000U;
}

bool native_screen_power_supported(void)
{
    /* Firmware and driver code cannot change during this app instance.
     * Keep live state checks per transition, but avoid repeated SDK/ROM scans. */
    if (screen_platform_gate < 0)
        screen_platform_gate = screen_validate_platform() ? 1 : 0;
    return screen_platform_gate != 0;
}

static uint32_t screen_copy_enabled(void)
{
    return screen_word(SCREEN_SUPERVISOR_STATE + 0x58U);
}

static bool screen_state_valid(void)
{
    return screen_word(SCREEN_SUPERVISOR_STATE) == 1U && screen_copy_enabled() <= 1U;
}

static bool screen_copier_postcondition(uint32_t expected_enabled)
{
    if (screen_copy_enabled() != expected_enabled)
        return false;
    /* Backlight commands must leave the existing copier configuration alone.
     * No timer, LCD controller, scanout or cursor is restarted on wake. */
    return !expected_enabled || (screen_word(SCREEN_SUPERVISOR_STATE + 0x1CU) == 0x54494D45U &&
                                 (screen_word(SCREEN_SUPERVISOR_STATE + 0x30U) & 0xFFU) == 1U);
}

static bool screen_off_postcondition(void)
{
    /* Cut LED supply, not merely brightness. Keep panel configuration alive
     * so waking does not reset/reinitialize it for another ~400ms. */
    return !(screen_word(SCREEN_LED_SUPPLY) & SCREEN_LED_SUPPLY_BIT) &&
           screen_word(SCREEN_LCD_CONTROL) == screen_saved_lcd_control &&
           screen_word(SCREEN_PWM_DUTY) == SCREEN_PWM_BLANK &&
           screen_word(SCREEN_PWM_PERIOD) == SCREEN_PWM_BLANK;
}

static int screen_control(bool on, uint32_t raw)
{
    unsigned saved = native_critical_enter();
    int result;
    /* Native backlight commands do not reset the LCD or change its scanout,
     * copier or timing. They gate the LED supply and PWM peripheral. Replace
     * the OS-default duty immediately, preserving the caller's exact mask. */
    result = ((int (*)(void))(uintptr_t)(on ? SCREEN_ON_ENTRY : SCREEN_OFF_ENTRY))();
    native_critical_enter();
    if (on)
        screen_set_duty(raw);
    native_critical_leave(saved);
    return result;
}

int native_screen_power_off(void)
{
    uint32_t copy_enabled;
    if (!native_screen_power_supported())
        return screen_last_status = NATIVE_SCREEN_POWER_UNSUPPORTED;
    if (!screen_state_valid())
        return screen_last_status = NATIVE_SCREEN_POWER_BAD_STATE;
    copy_enabled = screen_copy_enabled();
    if (screen_restore_required && !screen_native_result && screen_off_postcondition() &&
        screen_copier_postcondition(copy_enabled))
        return screen_last_status = NATIVE_SCREEN_POWER_OK;
    if (!screen_restore_required &&
        (screen_word(SCREEN_LCD_CONTROL) & SCREEN_LCD_ENABLE_BITS) != SCREEN_LCD_ENABLE_BITS)
        return screen_last_status = NATIVE_SCREEN_POWER_BAD_STATE;
    if (!screen_restore_required && copy_enabled &&
        (screen_word(SCREEN_SUPERVISOR_STATE + 0x1CU) != 0x54494D45U ||
         (screen_word(SCREEN_SUPERVISOR_STATE + 0x30U) & 0xFFU) != 1U))
        return screen_last_status = NATIVE_SCREEN_POWER_BAD_STATE;
    /* Native backlight control uses bit0 as a configured operating mode, not
     * proof that the LED supply is on. A visible display can legitimately use
     * zero (as in the device crash journal). Save before the first OFF, never
     * replace it with the state left by a partial transition or a retry. */
    if (!screen_restore_required) {
        screen_saved_pwm_mode = screen_word(SCREEN_PWM_CONTROL) & 1U;
        screen_saved_lcd_control = screen_word(SCREEN_LCD_CONTROL);
    }
    screen_restore_required = true;
    screen_native_result = screen_control(false, 0);
    return screen_last_status = !screen_native_result && screen_off_postcondition() &&
                                        screen_copier_postcondition(copy_enabled)
                                    ? NATIVE_SCREEN_POWER_OK
                                    : NATIVE_SCREEN_POWER_OFF_FAILED;
}

int native_screen_power_on(uint32_t brightness_raw)
{
    uint32_t copy_enabled;
    if (brightness_raw > 255U)
        return screen_last_status = NATIVE_SCREEN_POWER_BAD_STATE;
    if (!screen_restore_required)
        return screen_last_status = NATIVE_SCREEN_POWER_OK;
    if (!native_screen_power_supported())
        return screen_last_status = NATIVE_SCREEN_POWER_UNSUPPORTED;
    if (!screen_state_valid())
        return screen_last_status = NATIVE_SCREEN_POWER_BAD_STATE;
    copy_enabled = screen_copy_enabled();
    screen_native_result = screen_control(true, brightness_raw);
    if (screen_native_result || screen_word(SCREEN_LCD_CONTROL) != screen_saved_lcd_control ||
        !(screen_word(SCREEN_LED_SUPPLY) & SCREEN_LED_SUPPLY_BIT) ||
        screen_word(SCREEN_PWM_DUTY) != brightness_raw || screen_word(SCREEN_PWM_PERIOD) != 255U ||
        (screen_word(SCREEN_PWM_CONTROL) & 1U) != screen_saved_pwm_mode ||
        !screen_copier_postcondition(copy_enabled))
        return screen_last_status = NATIVE_SCREEN_POWER_ON_FAILED;
    screen_restore_required = false;
    return screen_last_status = NATIVE_SCREEN_POWER_OK;
}

bool native_screen_power_is_off(void)
{
    return screen_restore_required;
}

void native_screen_power_snapshot(NativeScreenPowerSnapshot *snapshot)
{
    bool supported;
    unsigned saved;
    if (!snapshot)
        return;
    supported = native_screen_power_supported();
    saved = native_critical_enter();
    snapshot->supported = supported;
    snapshot->restore_required = screen_restore_required;
    snapshot->lcd_control = supported ? screen_word(SCREEN_LCD_CONTROL) : 0;
    snapshot->pwm_duty = supported ? screen_word(SCREEN_PWM_DUTY) : 0;
    snapshot->pwm_period = supported ? screen_word(SCREEN_PWM_PERIOD) : 0;
    snapshot->pwm_control = supported ? screen_word(SCREEN_PWM_CONTROL) : 0;
    snapshot->display_copy_enabled = supported ? screen_copy_enabled() : 0;
    snapshot->native_result = screen_native_result;
    snapshot->last_status = screen_last_status;
    native_critical_leave(saved);
}
