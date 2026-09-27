#include "performance_clock.h"
#include "native_interrupts.h"
#include <stdint.h>

#define CLOCK_WORD(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define CLOCK_PLL 0x90140030U
#define CLOCK_DIVIDER 0x90140020U
#define CLOCK_PENDING 0x90140024U
#define CLOCK_SOURCE 0x90140810U
#define CLOCK_PLL_FIELDS 0xFF1F0011U
#define CLOCK_DIVIDER_FIELDS 0x00F00000U
#define CLOCK_TARGET_PLL ((41U << 24) | (2U << 16) | 1U)
#define CLOCK_TARGET_HZ 492000000U
#define CLOCK_POLL_LIMIT 200000U
#define CX_CLOCK_LOAD 0x900B0000U
#define CX_CLOCK_APPLY 0x900B000CU
#define CX_CLOCK_PENDING 0x900B0014U
#define CX_CLOCK_CURRENT 0x900B0024U
#define CX_CLOCK_FIELDS 0xC07FF1FEU
#define CX_TARGET_CLOCK ((40U << 15) | (1U << 21) | (1U << 1) | (3U << 12))
#define CX_TARGET_HZ 240000000U

static struct {
    bool supported, owned, active;
    uint32_t asic, target_hz;
    uint32_t saved_pll, saved_divider, entry_hz, requests, polls;
    int status;
} clock_state;

static uint32_t cx_cpu_hz(uint32_t config)
{
    if (config & 0x100U)
        return 48000000U >> (config >> 30);
    uint32_t divider = config & 0xFEU;
    /* Nover 3's CX CPU mode: mode 1 halves the ordinary even divisor.
     * Multiplier 40, divisor 1 => 240 MHz; AHB /4 => 60 MHz (not 120).
     * This mode is omitted by older emulator clock-rate calculations. */
    if (((config >> 21) & 3U) == 1U)
        divider >>= 1;
    return divider ? (6000000U * ((config >> 15) & 63U)) / divider : 0U;
}

static uint32_t cpu_hz(void)
{
    if (clock_state.asic == 0x101U)
        return cx_cpu_hz(CLOCK_WORD(CX_CLOCK_CURRENT));
    uint32_t pll = CLOCK_WORD(CLOCK_PLL), divider = CLOCK_WORD(CLOCK_DIVIDER);
    if (!(CLOCK_WORD(CLOCK_SOURCE) & 0x10U)) return 48000000U;
    if (pll & 0x10U) return 24000000U;
    uint32_t input_divider = (pll >> 16) & 31U;
    if (!(pll & 1U) || !input_divider) return 0;
    /* The OS CPU query uses a 24 MHz reference. Nover II's BASE=12 display
     * reports a different rate; 41 / 2 with no final divider is 492 MHz CPU. */
    return (24000000U / input_divider) * ((pll >> 24) & 63U) /
           (((divider >> 20) & 15U) + 1U);
}

static bool native_mapping(void)
{
    uint32_t table;
    __asm__ volatile("mrc p15,0,%0,c2,c0,0" : "=r"(table));
    table &= 0xFFFFC000U;
    if (!((table >= 0x10000000U && table <= 0x13FFC000U) ||
          (table >= 0xA4000000U && table <= 0xA400C000U)))
        return false;
    uint32_t sram = CLOCK_WORD(table + 0xA40U * 4U);
    return (sram & 3U) == 2U && (sram & 0xFFF00000U) == 0xA4000000U;
}

static bool await_clock_irq(uint32_t pending_address, uint32_t pending_mask)
{
    /* Nover II enables interrupts to complete the PMU request. SDK msleep()
     * temporarily masks other IRQ lines, so don't use it to await this IRQ.
     * Poll the hardware acknowledgement with a finite CPU bound instead. */
    native_critical_leave(0);
    for (unsigned i = 0; i < CLOCK_POLL_LIMIT; ++i) {
        ++clock_state.polls;
        if (!(CLOCK_WORD(pending_address) & pending_mask))
            return true;
    }
    return false;
}

static void set_divider(uint32_t fields)
{
    uint32_t current = CLOCK_WORD(CLOCK_DIVIDER);
    if ((current & CLOCK_DIVIDER_FIELDS) != fields)
        CLOCK_WORD(CLOCK_DIVIDER) = (current & ~(CLOCK_DIVIDER_FIELDS | 2U)) | fields;
}

static bool apply_cx2(uint32_t pll_fields, uint32_t divider_fields)
{
    unsigned saved = native_critical_enter();
    bool ok = false;
    if (!native_mapping()) {
        clock_state.status = -1;
        goto done;
    }
    if (CLOCK_WORD(CLOCK_DIVIDER) & 2U) {
        clock_state.status = -2;
        goto done;
    }
    /* Raise a divider before raising the PLL; lower it only after the clock
     * interrupt has completed. Avoid a transient overspeed during restore. */
    uint32_t current_divider = CLOCK_WORD(CLOCK_DIVIDER) & CLOCK_DIVIDER_FIELDS;
    if (divider_fields > current_divider)
        set_divider(divider_fields);
    uint32_t current_pll = CLOCK_WORD(CLOCK_PLL);
    if ((current_pll & CLOCK_PLL_FIELDS) != pll_fields) {
        CLOCK_WORD(CLOCK_PLL) = (current_pll & ~CLOCK_PLL_FIELDS) | pll_fields;
        ++clock_state.requests;
    }
    __asm__ volatile("mcr p15,0,%0,c7,c10,4" :: "r"(0) : "memory");
    if (!await_clock_irq(CLOCK_PENDING, 1U)) {
        clock_state.status = -3;
        goto done;
    }
    native_critical_enter();
    set_divider(divider_fields);
    __asm__ volatile("mcr p15,0,%0,c7,c10,4" :: "r"(0) : "memory");
    if (!await_clock_irq(CLOCK_PENDING, 1U)) {
        clock_state.status = -3;
        goto done;
    }
    native_critical_enter();
    ok = (CLOCK_WORD(CLOCK_PLL) & CLOCK_PLL_FIELDS) == pll_fields &&
         (CLOCK_WORD(CLOCK_DIVIDER) & CLOCK_DIVIDER_FIELDS) == divider_fields;
    clock_state.status = ok ? 0 : -4;
done:
    native_critical_leave(saved);
    return ok;
}

static bool apply_cx(uint32_t fields)
{
    unsigned saved = native_critical_enter();
    bool ok = false;
    if (!native_mapping()) {
        clock_state.status = -1;
        goto done;
    }
    uint32_t requested = (CLOCK_WORD(CX_CLOCK_LOAD) & ~CX_CLOCK_FIELDS) | fields;
    if ((CLOCK_WORD(CX_CLOCK_CURRENT) & CX_CLOCK_FIELDS) != fields ||
        (CLOCK_WORD(CX_CLOCK_LOAD) & CX_CLOCK_FIELDS) != fields) {
        /* Stage CPU and bus dividers together, then issue only the clock
         * command. In particular, never copy power/sleep command bits here. */
        CLOCK_WORD(CX_CLOCK_LOAD) = requested;
        CLOCK_WORD(CX_CLOCK_APPLY) = 4U;
        ++clock_state.requests;
    }
    __asm__ volatile("mcr p15,0,%0,c7,c10,4" :: "r"(0) : "memory");
    /* CURRENT is the latched value, unlike LOAD. Readback of LOAD alone
     * would falsely report success when application waits until exit. Allow
     * delayed hardware latching as well as a delayed interrupt acknowledgement. */
    native_critical_leave(0);
    for (unsigned i = 0; i < CLOCK_POLL_LIMIT; ++i) {
        ++clock_state.polls;
        if (!(CLOCK_WORD(CX_CLOCK_PENDING) & 2U) &&
            (CLOCK_WORD(CX_CLOCK_CURRENT) & CX_CLOCK_FIELDS) == fields &&
            (CLOCK_WORD(CX_CLOCK_LOAD) & CX_CLOCK_FIELDS) == fields) {
            ok = true;
            break;
        }
    }
    native_critical_enter();
    clock_state.status = ok ? 0 : (CLOCK_WORD(CX_CLOCK_PENDING) & 2U) ? -3 : -4;
done:
    native_critical_leave(saved);
    return ok;
}

static bool apply(uint32_t pll_fields, uint32_t divider_fields)
{
    return clock_state.asic == 0x101U ? apply_cx(pll_fields)
                                    : apply_cx2(pll_fields, divider_fields);
}

bool performance_clock_start(void)
{
    clock_state.asic = CLOCK_WORD(0x900A0000U);
    bool cx = clock_state.asic == 0x101U;
    clock_state.supported = cx || clock_state.asic == 0x202U;
    if (!clock_state.supported) return false;
    clock_state.target_hz = cx ? CX_TARGET_HZ : CLOCK_TARGET_HZ;
    if (!clock_state.owned) {
        clock_state.saved_pll = cx ? CLOCK_WORD(CX_CLOCK_CURRENT) & CX_CLOCK_FIELDS
                                  : CLOCK_WORD(CLOCK_PLL) & CLOCK_PLL_FIELDS;
        clock_state.saved_divider = cx ? 0U : CLOCK_WORD(CLOCK_DIVIDER) & CLOCK_DIVIDER_FIELDS;
        clock_state.entry_hz = cpu_hz();
        clock_state.owned = true;
    }
    uint32_t target = cx ? CX_TARGET_CLOCK | (clock_state.saved_pll & 0xC0000000U) : CLOCK_TARGET_PLL;
    clock_state.active = apply(target, 0U) && cpu_hz() == clock_state.target_hz;
    if (!clock_state.active) {
        int failure = clock_state.status ? clock_state.status : -5;
        performance_clock_restore();
        clock_state.status = failure;
    }
    return clock_state.active;
}

void performance_clock_restore(void)
{
    if (!clock_state.owned) return;
    if (apply(clock_state.saved_pll, clock_state.saved_divider)) {
        clock_state.owned = false;
        clock_state.active = false;
    }
}

void performance_clock_debug(FILE *file)
{
    if (!file) return;
    bool cx = clock_state.asic == 0x101U;
    uint32_t hz = clock_state.supported ? cpu_hz() : 0U;
    uint32_t config = clock_state.supported ? CLOCK_WORD(cx ? CX_CLOCK_CURRENT : CLOCK_PLL) : 0U;
    uint32_t pending = clock_state.supported ? CLOCK_WORD(cx ? CX_CLOCK_PENDING : CLOCK_PENDING) : 0U;
    uint32_t ahb_divisor = cx && !(config & 0x100U) ? ((config >> 12) & 7U) + 1U : 2U;
    bool verified_active = clock_state.active && hz == clock_state.target_hz &&
                           !(pending & (cx ? 2U : 1U));
    fprintf(file, "performance_clock supported=%u active=%u status=%d entry_hz=%lu target_hz=%lu register_cpu_hz=%lu ahb_hz=%lu asic=%lx pll=%08lx divider=%08lx source=%08lx pending=%08lx requests=%lu polls=%lu\n",
            clock_state.supported, verified_active, clock_state.status,
            (unsigned long)clock_state.entry_hz, (unsigned long)clock_state.target_hz,
            (unsigned long)hz, (unsigned long)(hz / ahb_divisor), (unsigned long)clock_state.asic,
            (unsigned long)config,
            (unsigned long)(clock_state.supported ? (cx ? config & 0x7000U : CLOCK_WORD(CLOCK_DIVIDER)) : 0U),
            (unsigned long)(clock_state.supported ? CLOCK_WORD(cx ? CX_CLOCK_LOAD : CLOCK_SOURCE) : 0U),
            (unsigned long)pending,
            (unsigned long)clock_state.requests, (unsigned long)clock_state.polls);
}
