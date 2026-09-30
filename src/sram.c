#include "sram.h"
#include "platform/arm926_ram_span.h"

#include <libndls.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SRAM_PHYSICAL_ADDRESS 0xA4000000U
#define SRAM_VIRTUAL_ADDRESS 0xEE000000U
#define SRAM_CX_HARDWARE_SIZE (128U * 1024U)
#define SRAM_CX_POOL_SIZE (16U * 1024U)
#define SRAM_CX_POOL_OFFSET (SRAM_CX_HARDWARE_SIZE - SRAM_CX_POOL_SIZE)
#define SRAM_CX2_HARDWARE_SIZE (256U * 1024U)
#define SRAM_CX2_POOL_SIZE (128U * 1024U)
#define SRAM_CX2_POOL_OFFSET (SRAM_CX2_HARDWARE_SIZE - SRAM_CX2_POOL_SIZE)
#define SRAM_SECTION_SIZE 0x100000U
#define SRAM_TTB_SIZE 16384U
#define SRAM_TTB_ALIGNMENT 16384U
#define SRAM_CACHE_LINE_SIZE 32U
#define SRAM_DOMAIN_SHIFT 5U
#define SRAM_SECTION_ACCESS_FULL (0x3U << 10)
#define SRAM_SECTION_CACHE_NONE (0x0U << 2)
#define SRAM_SECTION_CACHE_WRITEBACK (0x3U << 2)
#define SRAM_SECTION_TYPE 0x2U
#define SRAM_SECTION_BIT4 (1U << 4)

static uint32_t g_sram_original_ttbr0;
static uint32_t *g_sram_ttb = NULL;
static void *g_sram_clone = NULL;
static void *g_sram_clone_allocation = NULL;
static uint8_t g_sram_original_domain = 0;
static uint8_t *g_sram_pool = NULL;
static size_t g_sram_pool_used = 0;
static size_t g_sram_pool_capacity = 0;
static size_t g_sram_pool_offset = 0;
static bool g_sram_enabled = false;
static bool g_sram_direct = false;
static bool g_sram_native_mapping = false;

static uint32_t g_sram_table_offset;
static uint32_t g_sram_saved_zero, g_sram_saved_alias, g_sram_saved_pool;
static char g_sram_status_message[96] = "not initialized";

static void sram_set_status_message(const char *message)
{
    if (!message) {
        message = "unknown";
    }
    strncpy(g_sram_status_message, message, sizeof(g_sram_status_message) - 1U);
    g_sram_status_message[sizeof(g_sram_status_message) - 1U] = '\0';
}

static unsigned sram_critical_enter(void)
{
    unsigned saved, masked;
    /* The Ndless SWI return clears FIQ masking. Keep both interrupt classes
     * blocked across the snapshot/remap using local CPU instructions only. */
    __asm__ volatile("mrs %0, cpsr\n\torr %1, %0, #0xc0\n\tmsr cpsr_c, %1"
                     : "=&r"(saved), "=&r"(masked)
                     :
                     : "memory", "cc");
    return saved & 0xC0U;
}

static void sram_critical_leave(unsigned saved)
{
    unsigned current, bits;
    __asm__ volatile(
        "mrs %0, cpsr\n\tbic %0, %0, #0xc0\n\tand %1, %2, #0xc0\n\torr %0, %0, %1\n\tmsr cpsr_c, %0"
        : "=&r"(current), "=&r"(bits)
        : "r"(saved)
        : "memory", "cc");
}

static uint32_t sram_get_ttbr0(void)
{
    uint32_t ttbr0;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr0));
    return ttbr0;
}

static void sram_set_ttbr0(uint32_t ttbr0)
{
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" ::"r"(ttbr0));
}

static void sram_drain_write_buffer(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}

static void sram_invalidate_tlb(void)
{
    __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" ::"r"(0) : "memory");
    sram_drain_write_buffer();
}

static void sram_flush_dcache_range(uintptr_t start, uintptr_t end)
{
    uintptr_t address = start & ~(uintptr_t)(SRAM_CACHE_LINE_SIZE - 1U);
    while (address < end) {
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" ::"r"(address) : "memory");
        address += SRAM_CACHE_LINE_SIZE;
    }
}

static uint32_t sram_l1_index(uintptr_t address)
{
    return (uint32_t)(address >> 20);
}

static size_t sram_hardware_pool_size(void)
{
    return is_cx2 ? SRAM_CX2_POOL_SIZE : SRAM_CX_POOL_SIZE;
}

static size_t sram_hardware_size(void)
{
    return is_cx2 ? SRAM_CX2_HARDWARE_SIZE : SRAM_CX_HARDWARE_SIZE;
}

static size_t sram_hardware_pool_offset(void)
{
    return is_cx2 ? SRAM_CX2_POOL_OFFSET : SRAM_CX_POOL_OFFSET;
}

static const char *sram_enabled_message(void)
{
    return is_cx2 ? "enabled cx2 high 128K" : "enabled cx high 16K";
}

static void *sram_alloc_aligned(size_t size, size_t alignment, void **raw_allocation)
{
    uintptr_t aligned_address;
    uint8_t *raw;

    if (raw_allocation) {
        *raw_allocation = NULL;
    }
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1U)) != 0U) {
        return NULL;
    }

    raw = (uint8_t *)malloc(size + alignment - 1U);
    if (!raw) {
        return NULL;
    }
    aligned_address = ((uintptr_t)raw + (alignment - 1U)) & ~(uintptr_t)(alignment - 1U);
    if (raw_allocation) {
        *raw_allocation = raw;
    }
    return (void *)aligned_address;
}

static void sram_map_section(uintptr_t virtual_address, uintptr_t physical_address,
                             uint32_t attributes)
{
    uint32_t descriptor;
    uint32_t index;
    uintptr_t entry_address;

    if (!g_sram_ttb) {
        return;
    }

    index = sram_l1_index(virtual_address);
    descriptor = (uint32_t)(physical_address & 0xFFF00000U) | attributes | SRAM_SECTION_TYPE |
                 SRAM_SECTION_BIT4;
    g_sram_ttb[index] = descriptor;
    entry_address = (uintptr_t)&g_sram_ttb[index];
    sram_flush_dcache_range(entry_address, entry_address + sizeof(g_sram_ttb[index]));
    sram_invalidate_tlb();
}

/* Original CX keeps its native identity mapping and TTBR. The old EE alias
 * path failed on hardware; it is deliberately not used for this smaller pool.
 * Recognize the native startup layout, not an OS version number. Different
 * layouts keep using RAM until their SRAM ownership has been established. */
static uint32_t sram_read_word(uint32_t address, void *context)
{
    (void)context;
    return *(const volatile uint32_t *)(uintptr_t)address;
}
static bool sram_ram_span(uint32_t address, uint32_t bytes, uint32_t table)
{
    return arm926_ram_span(address, bytes, table, sram_read_word, NULL, NULL);
}
static __attribute__((naked, noinline)) uint32_t sram_banked_sp(unsigned mode
                                                                __attribute__((unused)))
{
    __asm__ volatile("mrs r1, cpsr\n\tbic r2, r1, #31\n\torr r2, r2, r0\n\t"
                     "msr cpsr_c, r2\n\tmov r0, sp\n\tmsr cpsr_c, r1\n\tbx lr");
}
static bool sram_cx_layout(uint32_t table)
{
    uint32_t control, domain;
    __asm__ volatile("mrc p15,0,%0,c1,c0,0" : "=r"(control));
    __asm__ volatile("mrc p15,0,%0,c3,c0,0" : "=r"(domain));
    if (!(control & 1U) || ((domain & 3U) != 1U && (domain & 3U) != 3U) || table < 0x10000000U ||
        table > 0x13ffc000U)
        return false;
    uint32_t self = sram_read_word(table + (table >> 20) * 4U, NULL);
    if ((self & 0x1e3U) != 2U || !(self & 0xc00U) || (self & 0xfff00000U) != (table & 0xfff00000U))
        return false;
    /* Preserve the established native write-through attributes; no synonyms
     * or new page-table entries are introduced. */
    if (sram_read_word(table + 0xa40U * 4U, NULL) != 0xa4000c1aU ||
        !sram_ram_span(0x10000000U, 0x2acU, table))
        return false;
    uint32_t reset = sram_read_word(0x10000020U, NULL);
    if (reset < 0x10000100U || reset > 0x11fff000U || (reset & 3U) ||
        !sram_ram_span(reset - 0x98U, 0xb4U, table))
        return false;
    static const uint32_t reset_code[] = {0xee110f10U, 0xe10f0000U, 0xe3c0001fU, 0xe3800013U,
                                          0xe38000c0U, 0xe12ff000U, 0xe51fd0b8U};
    static const uint32_t stack_offsets[] = {0x98U, 0x94U, 0x84U, 0x80U, 0x7cU, 0x78U, 0x74U};
    static const uint32_t stack_values[] = {0xa4009f00U, 0xa400a304U, 0xa400a80cU, 0xa400a910U,
                                            0xa400aa14U, 0xa400ab18U, 0xa400a308U};
    for (unsigned i = 0; i < 7U; ++i)
        if (sram_read_word(reset + i * 4U, NULL) != reset_code[i] ||
            sram_read_word(reset - stack_offsets[i], NULL) != stack_values[i])
            return false;
    /* Two native SRAM code-copy extents, including their actual PC-relative
     * literal loads and size subtraction. Branch destinations may relocate. */
    static const uint32_t startup_code[] = {
        0xe92d4070U, 0xe24dd030U, 0xe1a04000U, 0xeb000000U, 0xeb000000U, 0xe59f222cU, 0xe59f022cU,
        0xe0601002U, 0xeb000000U, 0xe59f0224U, 0xe59f1224U, 0xe0601001U, 0xeb000000U};
    for (unsigned i = 0; i < 13U; ++i) {
        uint32_t word = sram_read_word(0x10000050U + i * 4U, NULL);
        if (startup_code[i] == 0xeb000000U)
            word &= 0xff000000U;
        if (word != startup_code[i])
            return false;
    }
    static const uint32_t extents[] = {0xa4009b00U, 0xa4009700U, 0xa400ab1cU, 0xa400ae5cU};
    for (unsigned i = 0; i < 4U; ++i)
        if (sram_read_word(0x10000298U + i * 4U, NULL) != extents[i])
            return false;
    static const unsigned modes[] = {0x1fU, 0x11U, 0x12U, 0x13U, 0x17U, 0x1bU};
    for (unsigned i = 0; i < 6U; ++i) {
        uint32_t sp = sram_banked_sp(modes[i]);
        if ((sp >= 0xa401c000U && sp <= 0xa4020000U) || (sp >= 0x1c000U && sp <= 0x20000U))
            return false;
    }
    for (uint32_t at = 0xa401c000U; at < 0xa4020000U; at += 4U)
        if (sram_read_word(at, NULL))
            return false;
    return true;
}
static bool sram_init_cx(void)
{
    unsigned saved = sram_critical_enter();
    uint32_t table = sram_get_ttbr0() & 0xffffc000U;
    bool layout = sram_read_word(0x900a0000U, NULL) == 0x101U && sram_cx_layout(table);
    sram_critical_leave(saved);
    if (!layout) {
        sram_set_status_message("unrecognized cx SRAM layout");
        return false;
    }
    void *shadow = sram_alloc_aligned(SRAM_CX_POOL_SIZE, 32U, &g_sram_clone_allocation);
    sram_critical_enter(); /* SDK allocation can clear the FIQ mask. */
    if (!shadow || (sram_get_ttbr0() & 0xffffc000U) != table || !sram_cx_layout(table)) {
        sram_critical_leave(saved);
        free(g_sram_clone_allocation);
        g_sram_clone_allocation = NULL;
        sram_critical_leave(saved);
        sram_set_status_message("cx SRAM preparation failed");
        return false;
    }
    g_sram_original_ttbr0 = sram_get_ttbr0();
    g_sram_ttb = (uint32_t *)(uintptr_t)table;
    g_sram_clone = shadow;
    g_sram_pool = (uint8_t *)(uintptr_t)0xa401c000U;
    memcpy(shadow, g_sram_pool, SRAM_CX_POOL_SIZE);
    g_sram_pool_capacity = SRAM_CX_POOL_SIZE;
    g_sram_pool_offset = SRAM_CX_POOL_OFFSET;
    g_sram_pool_used = 0;
    g_sram_direct = true;
    g_sram_enabled = true;
    sram_critical_leave(saved);
    sram_set_status_message("enabled cx identity 16K");
    return true;
}

static bool sram_init_impl(void)
{
    uintptr_t old_table_address;
    uint32_t old_ttbr0_flags;
    uint32_t sram_entry;
    size_t hardware_size;
    size_t pool_capacity;
    size_t pool_offset;
    size_t clone_size;
    unsigned interrupt_mask;
    bool cx2;

    if (g_sram_enabled) {
        sram_set_status_message(sram_enabled_message());
        return true;
    }
    if (!is_cx2)
        return sram_init_cx();

    hardware_size = sram_hardware_size();
    pool_capacity = sram_hardware_pool_size();
    pool_offset = sram_hardware_pool_offset();
    cx2 = is_cx2;
    clone_size = cx2 ? SRAM_SECTION_SIZE : pool_capacity;
    g_sram_original_ttbr0 = sram_get_ttbr0();
    old_ttbr0_flags = g_sram_original_ttbr0 & ~0xFFFFC000U;
    old_table_address = (uintptr_t)(g_sram_original_ttbr0 & 0xFFFFC000U);
    if (old_table_address != SRAM_PHYSICAL_ADDRESS + 0x4000U) {
        sram_set_status_message("unexpected OS page table");
        goto fail;
    }
    g_sram_table_offset = (uint32_t)(old_table_address - SRAM_PHYSICAL_ADDRESS);

    g_sram_clone = sram_alloc_aligned(clone_size, cx2 ? SRAM_SECTION_SIZE : SRAM_CACHE_LINE_SIZE,
                                      &g_sram_clone_allocation);
    if (!g_sram_clone) {
        sram_set_status_message("sram clone alloc failed");
        goto fail;
    }
    if (cx2) {
        /* The live SRAM copy below overwrites the first hardware_size bytes.
         * Clear only the unmapped hardware tail before publishing the alias. */
        memset((uint8_t *)g_sram_clone + hardware_size, 0, SRAM_SECTION_SIZE - hardware_size);
        sram_flush_dcache_range((uintptr_t)g_sram_clone + hardware_size,
                                (uintptr_t)g_sram_clone + SRAM_SECTION_SIZE);
    }

    memcpy(&sram_entry, (const uint32_t *)old_table_address + sram_l1_index(SRAM_PHYSICAL_ADDRESS),
           sizeof(sram_entry));
    if ((sram_entry & 0xFFF00000U) != SRAM_PHYSICAL_ADDRESS) {
        sram_set_status_message("sram not identity mapped");
        goto fail;
    }
    g_sram_original_domain = (uint8_t)((sram_entry >> SRAM_DOMAIN_SHIFT) & 0x0FU);

    /* Storage runs cooperatively inside this app with IRQs masked. Explicit
     * native standby restores physical SRAM through sram_with_native_mapping;
     * no OS idle-driver registration is needed during playback. */

    /* All allocations and validation precede publication. Copy the live OS
     * state only after IRQ/FIQ are masked, then install the finished table. */
    interrupt_mask = sram_critical_enter();
    if (cx2) {
        memcpy(g_sram_clone, (const void *)(uintptr_t)SRAM_PHYSICAL_ADDRESS, hardware_size);
        sram_flush_dcache_range((uintptr_t)g_sram_clone, (uintptr_t)g_sram_clone + hardware_size);
        /* The OS's set_pagetable_entry writes through fixed A4004000, not
         * through CP15 TTBR. That alias and the hardware walker must address
         * the SAME table. A separate L1 copy silently loses native updates. */
        g_sram_ttb = (uint32_t *)((uint8_t *)g_sram_clone + g_sram_table_offset);
        g_sram_saved_zero = g_sram_ttb[0];
        g_sram_saved_alias = g_sram_ttb[sram_l1_index(SRAM_PHYSICAL_ADDRESS)];
        g_sram_saved_pool = g_sram_ttb[sram_l1_index(SRAM_VIRTUAL_ADDRESS)];
        sram_map_section(SRAM_PHYSICAL_ADDRESS, (uintptr_t)g_sram_clone,
                         SRAM_SECTION_ACCESS_FULL | SRAM_SECTION_CACHE_NONE |
                             ((uint32_t)g_sram_original_domain << SRAM_DOMAIN_SHIFT));
        sram_map_section(0x00000000U, (uintptr_t)g_sram_clone,
                         SRAM_SECTION_ACCESS_FULL | SRAM_SECTION_CACHE_NONE |
                             ((uint32_t)g_sram_original_domain << SRAM_DOMAIN_SHIFT));
    } else {
        memcpy(g_sram_clone, (const void *)(uintptr_t)(SRAM_PHYSICAL_ADDRESS + pool_offset),
               pool_capacity);
        sram_flush_dcache_range((uintptr_t)g_sram_clone, (uintptr_t)g_sram_clone + pool_capacity);
    }
    sram_map_section(SRAM_VIRTUAL_ADDRESS, SRAM_PHYSICAL_ADDRESS,
                     SRAM_SECTION_ACCESS_FULL | SRAM_SECTION_CACHE_WRITEBACK);
    sram_drain_write_buffer();
    sram_set_ttbr0((uint32_t)(uintptr_t)g_sram_ttb | old_ttbr0_flags);
    sram_invalidate_tlb();
    sram_critical_leave(interrupt_mask);

    g_sram_pool = (uint8_t *)(uintptr_t)(SRAM_VIRTUAL_ADDRESS + pool_offset);
    g_sram_pool_used = 0;
    g_sram_pool_capacity = pool_capacity;
    g_sram_pool_offset = pool_offset;
    g_sram_enabled = true;
    sram_set_status_message(sram_enabled_message());
    return true;

fail:
    /* No failed preparation has published a new TTBR or mapping. */
    free(g_sram_clone_allocation);
    g_sram_clone = NULL;
    g_sram_clone_allocation = NULL;
    g_sram_ttb = NULL;
    g_sram_pool = NULL;
    g_sram_pool_used = 0;
    g_sram_pool_capacity = 0;
    g_sram_pool_offset = 0;
    g_sram_enabled = false;
    return false;
}

bool sram_init(void)
{
    unsigned saved = sram_critical_enter();
    sram_critical_leave(saved);
    bool result = sram_init_impl();
    sram_critical_leave(saved);
    return result;
}

static void sram_shutdown_impl(void)
{
    unsigned interrupt_mask;
    bool cx2;

    if (!g_sram_enabled) {
        return;
    }

    cx2 = !g_sram_direct;
    interrupt_mask = sram_critical_enter();
    if (cx2) {
        /* A400 still maps the live, uncached OS clone. Its normal heap alias
         * can retain clean stale cache lines after IRQ/worker updates. */
        memcpy((void *)(uintptr_t)SRAM_VIRTUAL_ADDRESS,
               (const void *)(uintptr_t)SRAM_PHYSICAL_ADDRESS, SRAM_CX2_HARDWARE_SIZE);
        /* The copied-back L1 includes live native mapping updates. Remove
         * only our three aliases before the original TTBR becomes active. */
        volatile uint32_t *restored =
            (volatile uint32_t *)(uintptr_t)(SRAM_VIRTUAL_ADDRESS + g_sram_table_offset);
        restored[0] = g_sram_saved_zero;
        restored[sram_l1_index(SRAM_PHYSICAL_ADDRESS)] = g_sram_saved_alias;
        restored[sram_l1_index(SRAM_VIRTUAL_ADDRESS)] = g_sram_saved_pool;
        sram_flush_dcache_range(SRAM_VIRTUAL_ADDRESS,
                                SRAM_VIRTUAL_ADDRESS + SRAM_CX2_HARDWARE_SIZE);
    } else {
        memcpy((void *)g_sram_pool, g_sram_clone, g_sram_pool_capacity);
        sram_flush_dcache_range((uintptr_t)g_sram_pool,
                                (uintptr_t)g_sram_pool + g_sram_pool_capacity);
    }
    sram_drain_write_buffer();
    if (!g_sram_direct) {
        sram_set_ttbr0(g_sram_original_ttbr0);
        sram_invalidate_tlb();
    }
    sram_critical_leave(interrupt_mask);

    free(g_sram_clone_allocation);
    g_sram_clone = NULL;
    g_sram_clone_allocation = NULL;
    g_sram_ttb = NULL;
    g_sram_pool = NULL;
    g_sram_pool_used = 0;
    g_sram_pool_capacity = 0;
    g_sram_pool_offset = 0;
    g_sram_enabled = false;
    g_sram_direct = false;
    sram_set_status_message("disabled");
}

void sram_shutdown(void)
{
    unsigned saved = sram_critical_enter();
    sram_critical_leave(saved);
    sram_shutdown_impl();
    sram_critical_leave(saved);
}

static void sram_swap_cx_pool(void)
{
    uint32_t *shadow = g_sram_clone;
    volatile uint32_t *pool = (volatile uint32_t *)g_sram_pool;
    for (size_t i = 0; i < SRAM_CX_POOL_SIZE / 4U; ++i) {
        uint32_t value = pool[i];
        pool[i] = shadow[i];
        shadow[i] = value;
    }
    sram_flush_dcache_range((uintptr_t)shadow, (uintptr_t)shadow + SRAM_CX_POOL_SIZE);
    sram_flush_dcache_range((uintptr_t)g_sram_pool, (uintptr_t)g_sram_pool + SRAM_CX_POOL_SIZE);
    sram_drain_write_buffer();
}

static void sram_invalidate_dcache_range(uintptr_t start, uintptr_t end)
{
    for (uintptr_t address = start & ~(uintptr_t)31U; address < end; address += 32U)
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(address) : "memory");
}

static void sram_invalidate_icache(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" ::"r"(0) : "memory");
}

bool sram_with_native_mapping(void (*operation)(void *), void *context)
{
    if (!operation)
        return false;
    if (!g_sram_enabled) {
        operation(context);
        return true;
    }
    if (g_sram_direct) {
        unsigned saved = sram_critical_enter();
        if ((sram_get_ttbr0() & 0xffffc000U) != (g_sram_original_ttbr0 & 0xffffc000U)) {
            sram_critical_leave(saved);
            return false;
        }
        sram_swap_cx_pool();
        sram_invalidate_dcache_range((uintptr_t)g_sram_pool,
                                     (uintptr_t)g_sram_pool + SRAM_CX_POOL_SIZE);
        sram_critical_leave(saved);
        operation(context);
        sram_critical_enter();
        sram_invalidate_dcache_range((uintptr_t)g_sram_pool,
                                     (uintptr_t)g_sram_pool + SRAM_CX_POOL_SIZE);
        sram_swap_cx_pool();
        sram_critical_leave(saved);
        return true;
    }
    /* No allocations, pool reset or decoder teardown. The unused clone tail
     * is already owned, aligned DRAM and can retain the complete fast pool.
     * Reject before any mutation if the caller no longer owns this table. */
    if (!g_sram_clone || g_sram_pool_capacity != SRAM_CX2_POOL_SIZE ||
        g_sram_pool_offset != SRAM_CX2_POOL_OFFSET ||
        (sram_get_ttbr0() & 0xFFFFC000U) != (uint32_t)(uintptr_t)g_sram_ttb)
        return false;
    uint8_t *backup = (uint8_t *)g_sram_clone + SRAM_CX2_HARDWARE_SIZE;
    uint32_t active_ttbr = sram_get_ttbr0();
    unsigned saved = sram_critical_enter();
    memcpy(backup, g_sram_pool, g_sram_pool_capacity);
    sram_flush_dcache_range((uintptr_t)backup, (uintptr_t)backup + g_sram_pool_capacity);

    /* A400 is the authoritative uncached OS clone in DRAM; EE is physical SRAM.
     * They refer to different physical storage while the player table is live.
     * Copy live OS state back and remove only our three L1 aliases. */
    memcpy((void *)(uintptr_t)SRAM_VIRTUAL_ADDRESS, (const void *)(uintptr_t)SRAM_PHYSICAL_ADDRESS,
           SRAM_CX2_HARDWARE_SIZE);
    volatile uint32_t *table =
        (volatile uint32_t *)(uintptr_t)(SRAM_VIRTUAL_ADDRESS + g_sram_table_offset);
    table[0] = g_sram_saved_zero;
    table[sram_l1_index(SRAM_PHYSICAL_ADDRESS)] = g_sram_saved_alias;
    table[sram_l1_index(SRAM_VIRTUAL_ADDRESS)] = g_sram_saved_pool;
    sram_flush_dcache_range(SRAM_VIRTUAL_ADDRESS, SRAM_VIRTUAL_ADDRESS + SRAM_CX2_HARDWARE_SIZE);
    /* The clean above commits the restored OS bytes before this invalidate.
     * Remove EE cache aliases before native code modifies physical SRAM.
     * Otherwise a later pool access/eviction could replay pre-sleep bytes. */
    sram_invalidate_dcache_range(SRAM_VIRTUAL_ADDRESS,
                                 SRAM_VIRTUAL_ADDRESS + SRAM_CX2_HARDWARE_SIZE);
    sram_drain_write_buffer();
    sram_set_ttbr0(g_sram_original_ttbr0);
    sram_invalidate_tlb();
    sram_invalidate_icache();
    g_sram_native_mapping = true;
    sram_critical_leave(saved);

    operation(context);

    saved = sram_critical_enter();
    /* Discard stale clean heap aliases of the old OS clone before copying
     * the now-live native state, including any native page-table updates. */
    sram_invalidate_dcache_range((uintptr_t)g_sram_clone,
                                 (uintptr_t)g_sram_clone + SRAM_CX2_HARDWARE_SIZE);
    memcpy(g_sram_clone, (const void *)(uintptr_t)SRAM_PHYSICAL_ADDRESS, SRAM_CX2_HARDWARE_SIZE);
    g_sram_saved_zero = g_sram_ttb[0];
    g_sram_saved_alias = g_sram_ttb[sram_l1_index(SRAM_PHYSICAL_ADDRESS)];
    g_sram_saved_pool = g_sram_ttb[sram_l1_index(SRAM_VIRTUAL_ADDRESS)];
    uint32_t attributes = SRAM_SECTION_ACCESS_FULL | SRAM_SECTION_CACHE_NONE |
                          ((uint32_t)g_sram_original_domain << SRAM_DOMAIN_SHIFT);
    sram_map_section(SRAM_PHYSICAL_ADDRESS, (uintptr_t)g_sram_clone, attributes);
    sram_map_section(0U, (uintptr_t)g_sram_clone, attributes);
    sram_map_section(SRAM_VIRTUAL_ADDRESS, SRAM_PHYSICAL_ADDRESS,
                     SRAM_SECTION_ACCESS_FULL | SRAM_SECTION_CACHE_WRITEBACK);
    sram_flush_dcache_range((uintptr_t)g_sram_clone,
                            (uintptr_t)g_sram_clone + SRAM_CX2_HARDWARE_SIZE);
    sram_drain_write_buffer();
    sram_set_ttbr0(active_ttbr);
    sram_invalidate_tlb();
    sram_invalidate_icache();
    memcpy(g_sram_pool, backup, g_sram_pool_capacity);
    g_sram_native_mapping = false;
    /* Pool clients retain the same EE addresses and allocation watermark. */
    sram_critical_leave(saved);
    return true;
}

void *sram_alloc(size_t size, size_t alignment)
{
    uintptr_t aligned_address;
    uintptr_t aligned_used;
    uintptr_t mask;

    if (!g_sram_enabled || !g_sram_pool || size == 0) {
        return NULL;
    }
    if (alignment == 0) {
        alignment = 1;
    }
    if ((alignment & (alignment - 1U)) != 0U) {
        return NULL;
    }

    aligned_address = (uintptr_t)g_sram_pool + g_sram_pool_used;
    mask = (uintptr_t)alignment - 1U;
    if (aligned_address & mask) {
        aligned_address = (aligned_address + mask) & ~mask;
    }
    aligned_used = aligned_address - (uintptr_t)g_sram_pool;
    if (aligned_used > g_sram_pool_capacity || size > g_sram_pool_capacity - aligned_used) {
        return NULL;
    }

    g_sram_pool_used = (size_t)(aligned_used + size);
    return (void *)aligned_address;
}

bool sram_is_enabled(void)
{
    return g_sram_enabled;
}

size_t sram_bytes_used(void)
{
    return g_sram_pool_used;
}

size_t sram_bytes_capacity(void)
{
    return g_sram_pool_capacity;
}

const char *sram_status_message(void)
{
    return g_sram_status_message;
}

uint32_t sram_active_ttbr(void)
{
    return sram_get_ttbr0();
}

uint32_t sram_expected_ttbr(void)
{
    return g_sram_enabled ? (uint32_t)(uintptr_t)g_sram_ttb : 0U;
}

bool sram_uses_native_clone(void)
{
    /* Memory validators must describe the current mapping during a native
     * callback, not merely whether the player allocated a clone. */
    return g_sram_enabled && !g_sram_direct && !g_sram_native_mapping;
}
