#ifndef NDVIDEO_NATIVE_SCREEN_POWER_H
#define NDVIDEO_NATIVE_SCREEN_POWER_H

#include <stdbool.h>
#include <stdint.h>

enum {
    NATIVE_SCREEN_POWER_OK = 0,
    NATIVE_SCREEN_POWER_UNSUPPORTED = -1500,
    NATIVE_SCREEN_POWER_BAD_STATE = -1501,
    NATIVE_SCREEN_POWER_OFF_FAILED = -1502,
    NATIVE_SCREEN_POWER_ON_FAILED = -1503
};

typedef struct {
    bool supported, restore_required;
    uint32_t lcd_control, pwm_duty, pwm_period, pwm_control;
    uint32_t display_copy_enabled;
    int native_result, last_status;
} NativeScreenPowerSnapshot;

/* Foreground-only singleton for the one physical display. Performs its own
 * driver-code validation, including when the picker has no background worker.
 * Unsupported hardware is left untouched for the caller's legacy fallback. */
bool native_screen_power_supported(void);
/* Cuts the native LED supply completely while retaining LCD configuration for
 * fast wake. This is not minimum brightness; panel electronics remain ready.
 * Records restore ownership before native mutation. Partial failure retains
 * is_off=true so cleanup must still call on(). Repeated off is safe. */
int native_screen_power_off(void);
/* Wake and immediately set the caller's 0..255 raw brightness before allowing
 * another presentation. Success alone releases off ownership. The caller can
 * refresh its retained frame before beginning a brightness fade. */
int native_screen_power_on(uint32_t brightness_raw);
/* Also true after a partial off/on failure; this means restoration is required,
 * not that an unverified hardware transition is being reported as successful. */
bool native_screen_power_is_off(void);
void native_screen_power_snapshot(NativeScreenPowerSnapshot *snapshot);

#endif
