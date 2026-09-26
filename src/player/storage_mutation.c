#include "storage_mutation.h"
#include <stddef.h>
#include <stdint.h>
static uint32_t epoch = 1U;
static uint32_t generations[STORAGE_MUTATION_BLOCKS];
static StorageMutationStats stats;
uint32_t storage_mutation_epoch(void)
{
    return epoch;
}
uint32_t storage_mutation_block_generation(uint32_t block)
{
    return block < STORAGE_MUTATION_BLOCKS ? generations[block] : UINT32_MAX;
}
void storage_mutation_invalidate(void)
{
    /* Never let an old proof become valid again through epoch wrap. */
    if (epoch)
        ++epoch;
}
void storage_mutation_callback_failed(void)
{
    ++stats.failures;
    storage_mutation_invalidate();
}
StorageMutationStats storage_mutation_stats(void)
{
    return stats;
}
size_t storage_mutation_memory_bytes(void)
{
    return sizeof(epoch) + sizeof(generations) + sizeof(stats);
}
static bool unknown(void)
{
    ++stats.unknown;
    storage_mutation_invalidate();
    return false;
}
static void changed(uint32_t row)
{
    uint32_t *generation = &generations[row >> 6];
    if (*generation == UINT32_MAX) {
        storage_mutation_invalidate();
        *generation = 0;
    } else
        ++*generation;
}
bool storage_mutation_observe_spi(uint32_t command, const uint32_t d[5])
{
    ++stats.observed;
    if (!d)
        return unknown();
    uint32_t op = d[4] >> 24, mode = 0x100U, format = 0, flags = 0, transport = 0;
    switch (op) {
    case 0x0fU: /* GET FEATURE, including NAND ready/status. */
        format = 0x00010001U;
        flags = 0x00010100U;
        transport = 1003U;
        if (d[0] > 255U || d[2])
            return unknown();
        break;
    case 0x13U: /* Page-to-cache: no persistent array mutation. */
        format = 0x00010003U;
        flags = 1U;
        transport = 1001U;
        if (d[0] >= 65536U || d[2])
            return unknown();
        break;
    case 0x0bU:
    case 0x6bU: /* Single/quad cache read. */
        format = 0x00010802U;
        transport = 1002U;
        mode = op == 0x6bU ? 0x102U : 0x100U;
        if (d[0] > 8191U || d[2] > 2176U)
            return unknown();
        break;
    case 0x06U:
    case 0x04U: /* Write-enable/disable alone does not commit data. */
        format = 0x00010000U;
        flags = 1U;
        transport = 1001U;
        if (d[0] || d[2])
            return unknown();
        break;
    case 0x02U:
    case 0x32U:
    case 0x84U:
    case 0x34U: /* Cache load/random load. */
        format = 0x00010002U;
        flags = 1U;
        transport = 1004U;
        mode = (op == 0x32U || op == 0x34U) ? 0x102U : 0x100U;
        if (d[0] > 8191U || d[2] > 2176U)
            return unknown();
        break;
    case 0x10U:
    case 0xd8U: /* Program execute / block erase, physical row. */
        format = 0x00010003U;
        flags = 1U;
        transport = 1001U;
        if (d[0] >= 65536U || d[2])
            return unknown();
        break;
    default:
        return unknown();
    }
    if (command != transport || d[1] != format || d[3] != flags || d[4] != ((op << 24) | mode))
        return unknown();
    if (op == 0x10U) {
        ++stats.programs;
        changed(d[0]);
    } else if (op == 0xd8U) {
        ++stats.erases;
        changed(d[0]);
    }
    return true;
}
