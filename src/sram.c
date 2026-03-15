#include "sram.h"

#include <libndls.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SRAM_PHYSICAL_ADDRESS 0xA4000000U
#define SRAM_VIRTUAL_ADDRESS 0xEE000000U
#define SRAM_POOL_SIZE (256U * 1024U)
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
static void *g_sram_ttb_allocation = NULL;
static void *g_sram_clone = NULL;
static void *g_sram_clone_allocation = NULL;
static uint8_t g_sram_original_domain = 0;
static uint8_t *g_sram_pool = NULL;
static size_t g_sram_pool_used = 0;
static bool g_sram_enabled = false;
static char g_sram_status_message[96] = "not initialized";

static void sram_set_status_message(const char *message)
{
    if (!message) {
        message = "unknown";
    }
    strncpy(g_sram_status_message, message, sizeof(g_sram_status_message) - 1U);
    g_sram_status_message[sizeof(g_sram_status_message) - 1U] = '\0';
}

static uint32_t sram_get_ttbr0(void)
{
    uint32_t ttbr0;
    __asm__ volatile ("mrc p15, 0, %0, c2, c0, 0" : "=r" (ttbr0));
    return ttbr0;
}

static void sram_set_ttbr0(uint32_t ttbr0)
{
    __asm__ volatile ("mcr p15, 0, %0, c2, c0, 0" :: "r" (ttbr0));
}

static void sram_drain_write_buffer(void)
{
    __asm__ volatile ("mcr p15, 0, %0, c7, c10, 4" :: "r" (0) : "memory");
}

static void sram_invalidate_tlb(void)
{
    __asm__ volatile ("mcr p15, 0, %0, c8, c7, 0" :: "r" (0) : "memory");
    sram_drain_write_buffer();
}

static void sram_flush_dcache_range(uintptr_t start, uintptr_t end)
{
    uintptr_t address = start & ~(uintptr_t) (SRAM_CACHE_LINE_SIZE - 1U);
    while (address < end) {
        __asm__ volatile ("mcr p15, 0, %0, c7, c10, 1" :: "r" (address) : "memory");
        address += SRAM_CACHE_LINE_SIZE;
    }
}

static uint32_t sram_l1_index(uintptr_t address)
{
    return (uint32_t) (address >> 20);
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

    raw = (uint8_t *) malloc(size + alignment - 1U);
    if (!raw) {
        return NULL;
    }
    aligned_address = ((uintptr_t) raw + (alignment - 1U)) & ~(uintptr_t) (alignment - 1U);
    if (raw_allocation) {
        *raw_allocation = raw;
    }
    return (void *) aligned_address;
}

static void sram_map_section(uintptr_t virtual_address, uintptr_t physical_address, uint32_t attributes)
{
    uint32_t descriptor;
    uint32_t index;
    uintptr_t entry_address;

    if (!g_sram_ttb) {
        return;
    }

    index = sram_l1_index(virtual_address);
    descriptor = (uint32_t) (physical_address & 0xFFF00000U) |
        attributes |
        SRAM_SECTION_TYPE |
        SRAM_SECTION_BIT4;
    g_sram_ttb[index] = descriptor;
    entry_address = (uintptr_t) &g_sram_ttb[index];
    sram_flush_dcache_range(entry_address, entry_address + sizeof(g_sram_ttb[index]));
    sram_invalidate_tlb();
}

bool sram_init(void)
{
    uintptr_t old_table_address;
    uint32_t old_ttbr0_flags;
    uint32_t sram_entry;
    int interrupt_mask;

    if (g_sram_enabled) {
        sram_set_status_message("enabled");
        return true;
    }

    g_sram_original_ttbr0 = sram_get_ttbr0();
    old_ttbr0_flags = g_sram_original_ttbr0 & ~0xFFFFC000U;
    old_table_address = (uintptr_t) (g_sram_original_ttbr0 & 0xFFFFC000U);

    g_sram_ttb = (uint32_t *) sram_alloc_aligned(SRAM_TTB_SIZE, SRAM_TTB_ALIGNMENT, &g_sram_ttb_allocation);
    if (!g_sram_ttb) {
        sram_set_status_message("ttb alloc failed");
        goto fail;
    }
    memcpy(g_sram_ttb, (const void *) old_table_address, SRAM_TTB_SIZE);
    sram_flush_dcache_range((uintptr_t) g_sram_ttb, (uintptr_t) g_sram_ttb + SRAM_TTB_SIZE);
    sram_set_ttbr0((uint32_t) (uintptr_t) g_sram_ttb | old_ttbr0_flags);
    sram_invalidate_tlb();

    g_sram_clone = sram_alloc_aligned(SRAM_POOL_SIZE, SRAM_SECTION_SIZE, &g_sram_clone_allocation);
    if (!g_sram_clone) {
        sram_set_status_message("sram clone alloc failed");
        goto fail;
    }
    memcpy(g_sram_clone, (const void *) (uintptr_t) SRAM_PHYSICAL_ADDRESS, SRAM_POOL_SIZE);
    sram_flush_dcache_range((uintptr_t) g_sram_clone, (uintptr_t) g_sram_clone + SRAM_POOL_SIZE);
    sram_drain_write_buffer();

    sram_entry = g_sram_ttb[sram_l1_index(SRAM_PHYSICAL_ADDRESS)];
    if ((sram_entry & 0xFFF00000U) != SRAM_PHYSICAL_ADDRESS) {
        sram_set_status_message("sram not identity mapped");
        goto fail;
    }
    g_sram_original_domain = (uint8_t) ((sram_entry >> SRAM_DOMAIN_SHIFT) & 0x0FU);

    interrupt_mask = TCT_Local_Control_Interrupts(-1);
    sram_map_section(
        SRAM_PHYSICAL_ADDRESS,
        (uintptr_t) g_sram_clone,
        SRAM_SECTION_ACCESS_FULL |
        SRAM_SECTION_CACHE_NONE |
        ((uint32_t) g_sram_original_domain << SRAM_DOMAIN_SHIFT)
    );
    sram_map_section(
        0x00000000U,
        (uintptr_t) g_sram_clone,
        SRAM_SECTION_ACCESS_FULL |
        SRAM_SECTION_CACHE_NONE |
        ((uint32_t) g_sram_original_domain << SRAM_DOMAIN_SHIFT)
    );
    sram_map_section(
        SRAM_VIRTUAL_ADDRESS,
        SRAM_PHYSICAL_ADDRESS,
        SRAM_SECTION_ACCESS_FULL |
        SRAM_SECTION_CACHE_WRITEBACK
    );
    TCT_Local_Control_Interrupts(interrupt_mask);

    g_sram_pool = (uint8_t *) (uintptr_t) SRAM_VIRTUAL_ADDRESS;
    g_sram_pool_used = 0;
    g_sram_enabled = true;
    sram_set_status_message("enabled");
    return true;

fail:
    if (g_sram_ttb) {
        sram_set_ttbr0(g_sram_original_ttbr0);
        sram_invalidate_tlb();
    }
    free(g_sram_clone_allocation);
    free(g_sram_ttb_allocation);
    g_sram_clone = NULL;
    g_sram_clone_allocation = NULL;
    g_sram_ttb = NULL;
    g_sram_ttb_allocation = NULL;
    g_sram_pool = NULL;
    g_sram_pool_used = 0;
    g_sram_enabled = false;
    return false;
}

void sram_shutdown(void)
{
    int interrupt_mask;

    if (!g_sram_enabled) {
        return;
    }

    interrupt_mask = TCT_Local_Control_Interrupts(-1);
    memcpy((void *) (uintptr_t) SRAM_VIRTUAL_ADDRESS, g_sram_clone, SRAM_POOL_SIZE);
    sram_flush_dcache_range(SRAM_VIRTUAL_ADDRESS, SRAM_VIRTUAL_ADDRESS + SRAM_POOL_SIZE);
    sram_drain_write_buffer();
    sram_set_ttbr0(g_sram_original_ttbr0);
    sram_invalidate_tlb();
    TCT_Local_Control_Interrupts(interrupt_mask);

    free(g_sram_clone_allocation);
    free(g_sram_ttb_allocation);
    g_sram_clone = NULL;
    g_sram_clone_allocation = NULL;
    g_sram_ttb = NULL;
    g_sram_ttb_allocation = NULL;
    g_sram_pool = NULL;
    g_sram_pool_used = 0;
    g_sram_enabled = false;
    sram_set_status_message("disabled");
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

    aligned_address = (uintptr_t) g_sram_pool + g_sram_pool_used;
    mask = (uintptr_t) alignment - 1U;
    if (aligned_address & mask) {
        aligned_address = (aligned_address + mask) & ~mask;
    }
    aligned_used = aligned_address - (uintptr_t) g_sram_pool;
    if (aligned_used + size > SRAM_POOL_SIZE) {
        return NULL;
    }

    g_sram_pool_used = (size_t) (aligned_used + size);
    return (void *) aligned_address;
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
    return SRAM_POOL_SIZE;
}

const char *sram_status_message(void)
{
    return g_sram_status_message;
}
