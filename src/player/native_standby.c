/* Returning native standby for the fingerprinted CX II driver layout.
 * The driver requires genuine native SRAM and exact entry/restore state. */
#include <libndls.h>
#include <os.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "native_standby.h"
#include "native_firmware.h"
#include "native_standby_guard.h"
#include "native_display_hold.h"
#include "native_busy_hold.h"
#include "native_cursor_hold.h"
#define SUSPEND_ENTRY 0x10107634U
#define DISPLAY_ON_ENTRY 0x10006A38U
#define BACKLIGHT_DUTY_ADDR 0x90130014U
#define BACKLIGHT_PERIOD_ADDR 0x90130018U
#define BACKLIGHT_CONTROL_ADDR 0x90130020U
#define SRAM_BASE 0xA4000000U
#define OS_L1_TABLE 0xA4004000U
#define SRAM_LEAF 0xA40010C4U
#define SRAM_LEAF_BYTES 0xF4U
#define SRAM_LEAF_FNV 0xF5030C80U
#define RELEASE_TIMEOUT_SECONDS 4U
#define RELEASE_MAX_POLLS 4000000U

typedef struct {
    uintptr_t address;
    uint32_t words[4];
} Fingerprint;

static const Fingerprint fingerprints[] = {
    {SUSPEND_ENTRY, {0xE3A01000U, 0xE92D4070U, 0xE59F0094U, 0xE1A02001U}},
    {0x10107A4CU, {0xE52DE004U, 0xE24DD02CU, 0xE1A0000DU, 0xE3A02064U}},
    {0x10107A20U, {0xE52DE004U, 0xE3A03000U, 0xE24DD02CU, 0xE1A0000DU}},
    {0x10104B8CU, {0xE92D4010U, 0xEBFC4947U, 0xE1A04000U, 0xE3A00011U}},
    {0x10002158U, {0xE59F3020U, 0xE52DE004U, 0xE24DD02CU, 0xE58D000CU}},
    {0x10001198U, {0xE92D4008U, 0xE3510012U, 0x979FF101U, 0xEA000013U}},
    {0x10429200U, {0xE59F3028U, 0xE5930000U, 0xE3500000U, 0x1A000001U}},
    {0x1042AA00U, {0xE92D401FU, 0xE59F0198U, 0xE3A01032U, 0xE3A04601U}},
    {DISPLAY_ON_ENTRY, {0xE59F101CU, 0xE52DE004U, 0xE24DD02CU, 0xE3A03000U}},
    {0x10006940U, {0xE92D4010U, 0xE1A04000U, 0xEBFFFFF1U, 0xE3500000U}},
    {0x10006EF0U, {0xE3E0300EU, 0xE92D4070U, 0xE5813008U, 0xE590001CU}},
    {0x10007094U, {0xEB00247BU, 0xEBFFF6C6U, 0xE5950058U, 0xE3500000U}},
    {0x10010288U, {0xE59F101CU, 0xE52DE004U, 0xE24DD02CU, 0xE3A03000U}},
    {0x10010348U, {0xE92D40F8U, 0xE3E0300EU, 0xE5813008U, 0xE590501CU}},
    {0x10004BB8U, {0xE59F101CU, 0xE52DE004U, 0xE24DD02CU, 0xE3A03000U}},
    {0x10004E48U, {0xE3E0300EU, 0xE92D41F0U, 0xE5813008U, 0xE590001CU}},
    {0x10005000U, {0xE3A01001U, 0xE1A00006U, 0xEBFFF3EEU, 0xE3A00014U}},
    {0x10004C58U, {0xE59F301CU, 0xE3520000U, 0xE3A02000U, 0xE5832020U}}};

typedef struct {
    int gate_result, attempt_result;
} StandbyResult;

enum {
    STANDBY_OK,
    STANDBY_PLATFORM,
    STANDBY_FINGERPRINT,
    STANDBY_TASK,
    STANDBY_CPU,
    STANDBY_MAPPING,
    STANDBY_SRAM,
    STANDBY_WATCHDOG,
    STANDBY_KEY_RELEASE = 9,
    STANDBY_DISPLAY_RESTORE = 11,
    STANDBY_UI_HOLD
};

static uint32_t standby_read32(uintptr_t address)
{
    return *(const volatile uint32_t *)address;
}
static uint32_t standby_cpsr(void)
{
    uint32_t value;
    __asm__ volatile("mrs %0, cpsr" : "=r"(value));
    return value;
}
static uint32_t standby_control(void)
{
    uint32_t value;
    __asm__ volatile("mrc p15,0,%0,c1,c0,0" : "=r"(value));
    return value;
}
static uint32_t standby_ttbr(void)
{
    uint32_t value;
    __asm__ volatile("mrc p15,0,%0,c2,c0,0" : "=r"(value));
    return value;
}
static void standby_restore_mask(uint32_t saved)
{
    uint32_t current, bits;
    __asm__ volatile(
        "mrs %0,cpsr\n\tbic %0,%0,#0xc0\n\tand %1,%2,#0xc0\n\torr %0,%0,%1\n\tmsr cpsr_c,%0"
        : "=&r"(current), "=&r"(bits)
        : "r"(saved)
        : "memory", "cc");
}
static void standby_disable_watchdog(void)
{
    *(volatile uint32_t *)0x90060C00U = 0x1ACCE551U;
    *(volatile uint32_t *)0x90060008U = 0U;
    *(volatile uint32_t *)0x90060C00U = 0U;
}
static int standby_native_standby(void)
{
    return ((int (*)(void))(uintptr_t)SUSPEND_ENTRY)();
}
static int standby_native_display_on(void)
{
    return ((int (*)(void))(uintptr_t)DISPLAY_ON_ENTRY)();
}
static void standby_set_backlight_duty(uint32_t duty)
{
    *(volatile uint32_t *)BACKLIGHT_DUTY_ADDR = duty;
}

static bool standby_wait_released(void)
{
    unsigned stable = 0, polls = 0;
    uint32_t first_rtc = standby_read32(0x90090000U);
    for (;;) {
        bool pressed = false;
        /* The CX keypad words used by any_key_pressed, without I2C scans or idle(). */
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t keypad = standby_read32(0x900E0010U + i * 4U);
            pressed = pressed || keypad != 0;
        }
        uint32_t on_gpio = standby_read32(0x90140810U);
        pressed = pressed || !(on_gpio & 0x100U);
        ++polls;
        stable = pressed ? 0 : stable + 1U;
        uint32_t rtc = standby_read32(0x90090000U);
        if (stable >= 32U)
            return true;
        if ((uint32_t)(rtc - first_rtc) >= RELEASE_TIMEOUT_SECONDS || polls >= RELEASE_MAX_POLLS)
            return false;
    }
}

static bool ram_range(uintptr_t address, size_t bytes)
{
    return address >= 0x10000000U && address <= 0x12000000U - bytes;
}

static int standby_code_gate(void)
{
    if (!native_firmware_memory())
        return STANDBY_PLATFORM;
    for (size_t i = 0; i < sizeof(fingerprints) / sizeof(fingerprints[0]); ++i)
        for (unsigned j = 0; j < 4; ++j)
            if (standby_read32(fingerprints[i].address + 4U * j) != fingerprints[i].words[j])
                return STANDBY_FINGERPRINT;
    if (standby_read32(0x10429230U) != 0x1148F060U || standby_read32(0x10C52E4CU) != SRAM_LEAF ||
        standby_read32(0x10001DD8U) != 0x10000080U || standby_read32(0x1042ABA4U) != OS_L1_TABLE ||
        standby_read32(0x10006A5CU) != 1001U || standby_read32(0x100102ACU) != 1001U ||
        standby_read32(0x10004BDCU) != 1002U || standby_read32(0x10004C7CU) != 0x90130000U ||
        standby_read32(0x10004C6CU) != 0xE5830014U)
        return STANDBY_FINGERPRINT;
    return STANDBY_OK;
}

bool native_standby_supported(void)
{
    /* Read-only preflight, also valid before restoring genuine SRAM. The
     * live task, CPU and SRAM leaf are checked again at the actual entry. */
    return standby_code_gate() == STANDBY_OK;
}

static int standby_gate(void)
{
    uint32_t ttbr, descriptor, cpsr;
    uintptr_t table, task;
    int code_status = standby_code_gate();
    if (code_status)
        return code_status;
    task = standby_read32(0x1148F060U);
    if ((task & 3U) || !ram_range(task, 0x48) || standby_read32(task + 0x0c) != 0x5441534BU)
        return STANDBY_TASK;
    char task_name[9];
    for (unsigned i = 0; i < 8; ++i)
        task_name[i] = (char)(standby_read32(task + 0x10 + (i & ~3U)) >> (8U * (i & 3U)));
    task_name[8] = '\0';
    uint32_t flags = standby_read32(task + 0x18);
    uint32_t task_slice = standby_read32(task + 0x40);
    if (strcmp(task_name, "gui") || ((flags >> 16) & 255U) != 50U || !((flags >> 24) & 255U) ||
        !task_slice)
        return STANDBY_TASK;
    cpsr = standby_cpsr();
    ttbr = standby_ttbr();
    uint32_t control = standby_control();
    /* The leaf restores these ARM control bits to enabled on its return.
     * Require the standard SVC, IRQ-masked Ndless entry and matching bits. */
    if ((cpsr & 0x1fU) != 0x13U || !(cpsr & 0x80U) || (control & 0x1007U) != 0x1007U)
        return STANDBY_CPU;
    table = ttbr & 0xFFFFC000U;
    /* The OS initializer at 1042AA00 explicitly installs A4004000 as TTBR.
     * Accept this exact native SRAM table as well as a bounded SDRAM table;
     * never extend the read allowlist to arbitrary SRAM/MMIO addresses. */
    if (table != OS_L1_TABLE && !ram_range(table, 16384))
        return STANDBY_MAPPING;
    descriptor = standby_read32(table + (SRAM_BASE >> 20) * 4U);
    if ((descriptor & 3U) != 2U || (descriptor & 0xFFF00000U) != SRAM_BASE)
        return STANDBY_MAPPING;
    /* Verify the actual identity-mapped SRAM instructions, not just the ROM
     * source. A player-owned SRAM clone/remap must never pass this gate. */
    uint32_t hash = 2166136261U;
    for (unsigned i = 0; i < SRAM_LEAF_BYTES; i += 4U) {
        uint32_t word = standby_read32(SRAM_LEAF + i);
        for (unsigned j = 0; j < 4; ++j)
            hash = (hash ^ (uint8_t)(word >> (j * 8U))) * 16777619U;
    }
    return hash == SRAM_LEAF_FNV ? STANDBY_OK : STANDBY_SRAM;
}

static StandbyResult last_result;

static void standby_write(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
}

/* Called with genuine SRAM mapped and app-owned I/O drained. No SDK call,
 * file I/O, allocation or pool access spans the native standby invocation. */
void native_standby_run(void)
{
    StandbyResult *r = &last_result;
    memset(r, 0, sizeof(*r));
    unsigned saved = standby_cpsr();
    r->gate_result = standby_gate();
    if (r->gate_result) {
        standby_restore_mask(saved);
        return;
    }
    uint32_t cursor = standby_read32(0xC0000C00U);
    uint32_t watchdog = standby_read32(0x90060008U);
    uint32_t backlight_mode = standby_read32(BACKLIGHT_CONTROL_ADDR);
    if (watchdog & 3U) {
        r->attempt_result = STANDBY_WATCHDOG;
        standby_restore_mask(saved);
        return;
    }
    if (!standby_wait_released()) {
        r->attempt_result = STANDBY_KEY_RELEASE;
        standby_restore_mask(saved);
        return;
    }
    uint32_t lcd[7];
    for (unsigned i = 0; i < 7; ++i)
        lcd[i] = standby_read32(0xC0000000U + i * 4U);
    standby_restore_mask(0U);
    (void)standby_native_standby(); /* Its return is the old mask, not a status. */
    standby_restore_mask(0xC0U);
    if (standby_read32(0x90060008U) & 3U)
        standby_disable_watchdog();
    native_standby_guard_reassert_input();
    int busy_status = native_busy_hold_reassert();
    int display_status = native_display_hold_reassert();
    int cursor_status = native_cursor_hold_reassert();
    if (busy_status || display_status || cursor_status)
        r->attempt_result = STANDBY_UI_HOLD;
    /* Driver 101 restores the controller but leaves the LED supply off.
     * Native supervisor ON completes wake; the held display copier stays off. */
    bool display_ok = false;
    for (unsigned i = 0; i < 2; ++i) {
        standby_restore_mask(0U);
        int result = standby_native_display_on();
        standby_restore_mask(0xC0U);
        standby_set_backlight_duty(255U);
        uint32_t lcd_control = standby_read32(0xC0000018U);
        uint32_t period = standby_read32(BACKLIGHT_PERIOD_ADDR);
        uint32_t mode = standby_read32(BACKLIGHT_CONTROL_ADDR);
        display_ok = result == 0 && (lcd_control & 0x801U) == 0x801U && period == 255U &&
                     (mode & 1U) == (backlight_mode & 1U);
        if (display_ok)
            break;
    }
    /* Native resume resets timings, scanout and cursor mode. Restore the
     * captured app configuration while dark, before any app frame is shown. */
    standby_write(0xC0000018U, standby_read32(0xC0000018U) & ~0x801U);
    for (unsigned i = 0; i < 6; ++i)
        standby_write(0xC0000000U + i * 4U, lcd[i]);
    standby_write(0xC0000018U, lcd[6]);
    standby_write(0xC0000C00U, cursor & ~1U);
    if (standby_read32(0x90060008U) & 3U)
        standby_disable_watchdog();
    if (!display_ok)
        r->attempt_result = STANDBY_DISPLAY_RESTORE;
    /* The UI suppresses held ON until release, so the frame appears at once. */
    standby_restore_mask(saved);
}

int native_standby_status(void)
{
    return last_result.gate_result ? last_result.gate_result : last_result.attempt_result;
}
