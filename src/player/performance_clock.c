#include "performance_clock.h"
#include "native_interrupts.h"
#include <stdint.h>

#define CLOCK_WORD(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define CLOCK_PLL 0x90140030U
#define CLOCK_DIVIDER 0x90140020U
#define CLOCK_PENDING 0x90140024U
#define CLOCK_SOURCE 0x90140810U
#define CLOCK_PLL_FIELDS 0xFF1F0311U
#define CLOCK_DIVIDER_FIELDS 0x00F00000U
#define CLOCK_TARGET_PLL ((41U << 24) | (2U << 16) | 0x301U)
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
    uint32_t native_calls;
    uint32_t saved_emi74, saved_emi1c, native_hash;
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

/* Native SRAM copy of the OS's complete PLL/EMI transition. The checked
 * block includes the worker, self-refresh helpers and their literal pool.
 * This validates actual instructions, not an OS version number. */
#define CX2_CLOCK_BLOCK 0xA40012C8U
#define CX2_CLOCK_BLOCK_BYTES 0x258U
#define CX2_CLOCK_BLOCK_FNV 0x8D914E2FU
#define CX2_CLOCK_ENTRY 0xA4001444U
#define EMI_CONTROL 0x90120004U
#define EMI_TIMING74 0x90120074U
#define EMI_TIMING1C 0x9012001CU

static uint32_t native_clock_hash(void)
{
    const volatile uint8_t *code = (const volatile uint8_t *)(uintptr_t)CX2_CLOCK_BLOCK;
    uint32_t hash = 2166136261U;
    for (unsigned i = 0; i < CX2_CLOCK_BLOCK_BYTES; ++i)
        hash = (hash ^ code[i]) * 16777619U;
    return hash;
}

static bool apply_cx2(uint32_t pll_fields, uint32_t divider_fields)
{
    unsigned saved = native_critical_enter();
    bool ok = false;
    if (!native_mapping()) { clock_state.status = -1; goto done; }
    /* This native worker applies divider zero. Require
     * its normal entry configuration so restore never transiently removes
     * a divider from a higher PLL frequency. */
    if (divider_fields || CLOCK_WORD(CLOCK_DIVIDER) != 0x10000000U) {
        clock_state.status = -11; goto done;
    }
    clock_state.native_hash = native_clock_hash();
    if (clock_state.native_hash != CX2_CLOCK_BLOCK_FNV) {
        clock_state.status = -12; goto done;
    }
    if (!(CLOCK_WORD(CLOCK_SOURCE) & 0x10U) ||
        (CLOCK_WORD(EMI_CONTROL) & 0x40CU) ||
        (CLOCK_WORD(0xDC00000CU) & (1U << 15)) || CLOCK_WORD(CLOCK_PENDING)) {
        clock_state.status = -13; goto done;
    }
    bool restoring = pll_fields == clock_state.saved_pll &&
                     divider_fields == clock_state.saved_divider;
    uint32_t pll = (CLOCK_WORD(CLOCK_PLL) & ~CLOCK_PLL_FIELDS) | pll_fields;
    /* These are the OS's highest frequency timing profile. Restore the
     * actual entry timings instead of guessing them from the entry PLL. */
    uint32_t timing74 = restoring ? clock_state.saved_emi74 : 0x55U;
    uint32_t timing1c = restoring ? clock_state.saved_emi1c : 0x528U;
    ++clock_state.requests;
    ++clock_state.native_calls;
    /* CPU IRQ/FIQ stay masked: IRQ15 wakes WFI without entering the OS.
     * The SRAM worker enters DRAM self-refresh, commits the PLL through
     * 0x90140020, updates EMI timings, exits self-refresh and acknowledges
     * the PMU. It touches neither the DRAM stack nor app code in that span. */
    ((void (*)(uint32_t, uint32_t, uint32_t))(uintptr_t)CX2_CLOCK_ENTRY)(pll, timing74, timing1c);
    ok = (CLOCK_WORD(CLOCK_PLL) & CLOCK_PLL_FIELDS) == pll_fields &&
         CLOCK_WORD(CLOCK_DIVIDER) == 0x10000000U &&
         CLOCK_WORD(EMI_TIMING74) == timing74 && CLOCK_WORD(EMI_TIMING1C) == timing1c &&
         !(CLOCK_WORD(EMI_CONTROL) & 0x40CU);
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
        if (!cx) {
            clock_state.saved_emi74 = CLOCK_WORD(EMI_TIMING74);
            clock_state.saved_emi1c = CLOCK_WORD(EMI_TIMING1C);
        }
        clock_state.owned = true;
    }
    uint32_t target = cx ? CX_TARGET_CLOCK | (clock_state.saved_pll & 0xC0000000U) : CLOCK_TARGET_PLL;
    clock_state.active = apply(target, 0U) && cpu_hz() == clock_state.target_hz;
    if (!clock_state.active) {
        int failure = clock_state.status ? clock_state.status : -5;
        if (performance_clock_restore())
            clock_state.status = failure;
    }
    return clock_state.active;
}

bool performance_clock_restore(void)
{
    if (!clock_state.owned) return true;
    for (unsigned attempt = 0; attempt < 2U; ++attempt) {
        if (!apply(clock_state.saved_pll, clock_state.saved_divider))
            continue;
        clock_state.owned = false;
        clock_state.active = false;
        return true;
    }
    /* Keep ownership on failure so a later lifecycle cleanup can retry. */
    return false;
}

void performance_clock_debug(FILE *file)
{
    if (!file) return;
    bool cx = clock_state.asic == 0x101U;
    uint32_t hz = clock_state.supported ? cpu_hz() : 0U;
    uint32_t config = clock_state.supported ? CLOCK_WORD(cx ? CX_CLOCK_CURRENT : CLOCK_PLL) : 0U;
    uint32_t pending = clock_state.supported ? CLOCK_WORD(cx ? CX_CLOCK_PENDING : CLOCK_PENDING) : 0U;
    uint32_t ahb_divisor = cx && !(config & 0x100U) ? ((config >> 12) & 7U) + 1U : 2U;
    bool configured_active = clock_state.active && hz == clock_state.target_hz &&
                             !(pending & (cx ? 2U : 1U));
    fprintf(file, "performance_clock supported=%u active=%u status=%d entry_hz=%lu target_hz=%lu register_cpu_hz=%lu ahb_hz=%lu asic=%lx pll=%08lx divider=%08lx source=%08lx pending=%08lx requests=%lu polls=%lu\n",
            clock_state.supported, configured_active, clock_state.status,
            (unsigned long)clock_state.entry_hz, (unsigned long)clock_state.target_hz,
            (unsigned long)hz, (unsigned long)(hz / ahb_divisor), (unsigned long)clock_state.asic,
            (unsigned long)config,
            (unsigned long)(clock_state.supported ? (cx ? config & 0x7000U : CLOCK_WORD(CLOCK_DIVIDER)) : 0U),
            (unsigned long)(clock_state.supported ? CLOCK_WORD(cx ? CX_CLOCK_LOAD : CLOCK_SOURCE) : 0U),
            (unsigned long)pending,
            (unsigned long)clock_state.requests, (unsigned long)clock_state.polls);
    if (!cx && clock_state.supported)
        fprintf(file, "performance_clock_native calls=%lu code_hash=%08lx emi74=%08lx emi1c=%08lx\n",
                (unsigned long)clock_state.native_calls, (unsigned long)clock_state.native_hash,
                (unsigned long)CLOCK_WORD(EMI_TIMING74), (unsigned long)CLOCK_WORD(EMI_TIMING1C));
}
