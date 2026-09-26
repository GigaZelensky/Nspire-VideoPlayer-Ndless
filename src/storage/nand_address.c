#include "nand_address.h"
#include <string.h>
#define PAGE_BYTES 2048U
#define BLOCK_BYTES 131072U
#define CAPACITY_BYTES 134217728U
static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static NandAddressStatus finish(NandAddressJob *j, uint32_t block)
{
    uint64_t device = (uint64_t)block * BLOCK_BYTES + (j->offset & (BLOCK_BYTES - 1U));
    uint64_t physical = (uint64_t)j->map.physical_base_bytes + device;
    if (device + PAGE_BYTES > j->map.device_bytes || physical + PAGE_BYTES > CAPACITY_BYTES)
        return j->status = NAND_ADDRESS_ERROR;
    j->raw_page = (uint32_t)physical / PAGE_BYTES;
    return j->status = NAND_ADDRESS_READY;
}
bool nand_address_begin(NandAddressJob *j, const NandAddressMap *m, uint32_t offset)
{
    if (!j)
        return false;
    memset(j, 0, sizeof(*j));
    j->status = NAND_ADDRESS_ERROR;
    if (!m || m->media_bytes < PAGE_BYTES || m->device_bytes < PAGE_BYTES ||
        m->device_bytes > CAPACITY_BYTES ||
        m->physical_base_bytes > CAPACITY_BYTES - m->device_bytes ||
        offset > m->media_bytes - PAGE_BYTES ||
        m->before_remap_bytes > m->device_bytes - PAGE_BYTES ||
        offset > m->device_bytes - PAGE_BYTES - m->before_remap_bytes ||
        ((offset | m->before_remap_bytes | m->physical_base_bytes) & (PAGE_BYTES - 1U)) ||
        m->remap_limit_bytes > m->device_bytes || m->remap_count > 1024U ||
        (m->remap_count && !m->remaps))
        return false;
    j->map = *m;
    j->offset = offset + m->before_remap_bytes;
    j->source_block = j->offset / BLOCK_BYTES;
    j->status = NAND_ADDRESS_MORE;
    return true;
}
NandAddressStatus nand_address_step(NandAddressJob *j, uint32_t budget)
{
    if (!j)
        return NAND_ADDRESS_ERROR;
    if (j->status != NAND_ADDRESS_MORE)
        return j->status;
    if (!j->map.remap_enabled || j->offset >= j->map.remap_limit_bytes || !j->map.remap_count)
        return finish(j, j->source_block);
    if (budget > 64U)
        budget = 64U;
    while (budget-- && j->index < j->map.remap_count) {
        const uint8_t *entry = j->map.remaps + 4U * j->index++;
        if (le16(entry) == j->source_block)
            return finish(j, le16(entry + 2));
    }
    if (j->index == j->map.remap_count)
        return finish(j, j->source_block);
    return j->status;
}
