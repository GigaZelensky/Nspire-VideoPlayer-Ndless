#include "cx_nand_reader.h"
#include "../platform/nspire_hardware.h"
#include <string.h>

/* Match the native CX PL351 transaction boundaries: release CE on each
 * data access, use byte transfers for STATUS/READID, and poll chip status
 * before switching back to the page data stream. */
#define CTRL 0x8fff1000U
#define DATA 0x81280000U
#define CMD_STATUS 0x81000380U
#define CMD_ID 0x81200480U
#define CMD_PAGE 0x81918000U
#define CMD_READ_MODE 0x81000000U
enum {
    STATUS_ISSUE,
    STATUS_READ,
    ID_ISSUE,
    ID_READ,
    PAGE_ISSUE,
    PAGE_READY,
    PAGE_DATA,
    PAGE_ECC,
    PUBLISH,
    DRAIN_ISSUE,
    DRAIN_READ,
    PAGE_STATUS_READ,
    PAGE_READ_MODE
};

static uint32_t rd(CxNandReader *r, uint32_t address)
{
    return r->bus.read32(r->bus.context, address);
}
static void wr(CxNandReader *r, uint32_t address, uint32_t value)
{
    r->bus.write32(r->bus.context, address, value);
}
static bool supported_id(const uint8_t id[4])
{
    /* Legacy extended-ID 128 MiB x8 SLC parts. Check geometry rather than
     * requiring the emulator's Samsung manufacturer/device tuple. A1/F1/D1
     * identify the 128 MiB x8 class; byte 3 describes page/OOB/erase sizes.
     * The mounted FlashFX layout is independently validated by the caller. */
    return id[0] != 0U && id[0] != 0xffU &&
        (id[1] == 0xa1U || id[1] == 0xf1U || id[1] == 0xd1U) &&
        !(id[2] & 0x0cU) && (id[3] & 0x77U) == 0x15U;
}
/* Four calls per byte: inline the masked parity calculations in the ECC loop. */
static inline __attribute__((always_inline)) unsigned parity(uint32_t word)
{
    word ^= word >> 16;
    word ^= word >> 8;
    word ^= word >> 4;
    return (0x6996U >> (word & 15U)) & 1U;
}
static void ecc(const uint8_t *data, uint8_t code[3], unsigned bytes, unsigned bits)
{
    uint32_t index = 0, total = 0, ecc = 0xffffffU;
    for (unsigned i = 0; i < bytes; ++i) {
        unsigned v = data[i], p = parity(v);
        total ^= p;
        if (p)
            index ^= i << 3;
        index ^= parity(v & 0xaaU) | (parity(v & 0xccU) << 1) | (parity(v & 0xf0U) << 2);
    }
    for (unsigned i = 0; i < bits; ++i) {
        unsigned odd = (index >> i) & 1U;
        ecc ^= ((total ^ odd) | (odd << 1)) << (2U * i);
    }
    code[0] = (uint8_t)(ecc >> 6);
    code[1] = (uint8_t)(ecc >> 14);
    code[2] = (uint8_t)((ecc >> 22) | (ecc << 2));
}
void cx_nand_ecc512(const uint8_t data[512], uint8_t code[3])
{
    ecc(data, code, 512, 12);
}
void cx_nand_ecc256(const uint8_t data[256], uint8_t code[3])
{
    ecc(data, code, 256, 11);
}
static int correct(uint8_t *data, const uint8_t code[3], unsigned bytes, unsigned bits)
{
    uint8_t calculated[3];
    ecc(data, calculated, bytes, bits);
    uint32_t packed = (uint32_t)(code[0] ^ calculated[0]) |
                      ((uint32_t)(code[1] ^ calculated[1]) << 8) |
                      ((uint32_t)(code[2] ^ calculated[2]) << 16);
    if (!packed)
        return 0;
    if (!(packed & (packed - 1U)))
        return 2;
    uint32_t syndrome = ((packed << 6) | (packed >> 18)) & 0xffffffU;
    uint32_t mask = bits == 12U ? 0x555555U : 0x155555U;
    if ((syndrome & ~((1U << (bits * 2U)) - 1U)) || ((syndrome ^ (syndrome >> 1)) & mask) != mask)
        return -1;
    unsigned index = 0;
    for (unsigned i = 0; i < bits; ++i)
        index |= ((syndrome >> (2U * i + 1U)) & 1U) << i;
    data[index >> 3] ^= (uint8_t)(1U << (index & 7U));
    ecc(data, calculated, bytes, bits);
    return memcmp(calculated, code, 3) == 0 ? 1 : -1;
}
int cx_nand_correct512(uint8_t data[512], const uint8_t code[3])
{
    return correct(data, code, 512, 12);
}
int cx_nand_correct256(uint8_t data[256], const uint8_t code[3])
{
    return correct(data, code, 256, 11);
}
static bool passive_gate(CxNandReader *r)
{
    CxNandSnapshot *s = &r->snapshot;
    s->asic = rd(r, 0x900a0000U);
    if (!nspire_asic_is_cx(s->asic)) {
        r->error = CX_NAND_UNSUPPORTED_CONTROLLER;
        return false;
    }
    for (unsigned i = 0; i < 4U; ++i)
        s->peripheral_id[i] = (uint8_t)rd(r, CTRL + 0xfe0U + 4U * i);
    /* PL351, ARM designer0x41, supported emulated/researched revision3.
     * Record other revisions passively; do not silently assume identical IP.
     */
    if (s->peripheral_id[0] != 0x51U || s->peripheral_id[1] != 0x13U ||
        s->peripheral_id[2] != 0x34U || s->peripheral_id[3] != 0U) {
        r->error = CX_NAND_UNSUPPORTED_CONTROLLER;
        return false;
    }
    s->memc_status = rd(r, CTRL);
    s->memif_cfg = rd(r, CTRL + 4U);
    s->ecc_status = rd(r, CTRL + 0x300U);
    s->ecc_config = rd(r, CTRL + 0x304U);
    /* Exact researched controller capability image. Its maximum bus width
     * and chip-select count are capabilities, not fitted-device geometry;
     * the following READID separately verifies the actual 8-bit SLC device.
     */
    if (s->memif_cfg != 0x56U || (s->memc_status & 1U)) {
        r->error = CX_NAND_UNSUPPORTED_CONTROLLER;
        return false;
    }
    if ((s->ecc_status & 0x40U) || (s->ecc_config & 12U)) {
        r->error = CX_NAND_UNSUPPORTED_ECC_MODE;
        return false;
    }
    return true;
}
bool cx_nand_init(CxNandReader *r, CxNandBus bus)
{
    return cx_nand_init_layout(r, bus, CX_NAND_ECC_BOOT512);
}
bool cx_nand_init_layout(CxNandReader *r, CxNandBus bus, CxNandLayout layout)
{
    if (!r || !bus.read32 || !bus.read8 || !bus.write32 ||
        (layout != CX_NAND_ECC_BOOT512 && layout != CX_NAND_ECC_FLASHFX256))
        return false;
    memset(r, 0, sizeof(*r));
    r->bus = bus;
    r->layout = layout;
    r->quiescent = true;
    r->initialized = passive_gate(r);
    if (!r->initialized)
        r->status = CX_NAND_ERROR;
    return r->initialized;
}
static bool begin(CxNandReader *r, uint32_t now, uint32_t timeout)
{
    if (!r || !r->initialized)
        return false;
    if (r->status == CX_NAND_PENDING || !r->quiescent) {
        /* Reject a second submit without changing the in-flight result. */
        return false;
    }
    if (!timeout || timeout >= 0x80000000U) {
        r->error = CX_NAND_BAD_ARGUMENT;
        return false;
    }
    if (!passive_gate(r))
        return false;
    r->error = CX_NAND_OK;
    r->status = CX_NAND_PENDING;
    r->phase = STATUS_ISSUE;
    r->started = now;
    r->timeout = timeout;
    r->cancel = false;
    r->position = r->sector = 0;
    r->corrected_data = r->corrected_codes = 0;
    return true;
}
bool cx_nand_identify(CxNandReader *r, uint32_t now, uint32_t timeout)
{
    if (!begin(r, now, timeout))
        return false;
    r->identifying = true;
    r->identified = false;
    r->destination = NULL;
    return true;
}
bool cx_nand_begin_range(CxNandReader *r, uint32_t page, uint32_t column, void *destination,
                         size_t bytes, uint32_t now, uint32_t timeout)
{
    if (!r || !r->identified || r->status == CX_NAND_PENDING || !r->quiescent)
        return false;
    uintptr_t dest = (uintptr_t)destination, object = (uintptr_t)r;
    if (!destination || !bytes || column >= 2112U || bytes > 2112U - column || page >= 65536U ||
        dest > UINTPTR_MAX - bytes || (dest < object + sizeof(*r) && object < dest + bytes)) {
        r->error = CX_NAND_BAD_ARGUMENT;
        return false;
    }
    if (!begin(r, now, timeout))
        return false;
    r->identifying = false;
    r->page = page;
    r->column = column;
    r->bytes = (uint32_t)bytes;
    r->raw_tag = r->layout == CX_NAND_ECC_FLASHFX256 && column == 2060U && bytes == 4U;
    r->read_column = r->raw_tag ? column : 0;
    r->transfer_bytes = r->raw_tag ? 4U : 2112U;
    r->destination = destination;
    return true;
}
void cx_nand_cancel(CxNandReader *r)
{
    if (r && (r->status == CX_NAND_PENDING || !r->quiescent))
        r->cancel = true;
}
static CxNandStatus terminal(CxNandReader *r, CxNandStatus status)
{
    r->destination = NULL;
    r->status = status;
    return status;
}
CxNandStatus cx_nand_step(CxNandReader *r, uint32_t now, uint32_t budget)
{
    if (!r)
        return CX_NAND_ERROR;
    if (r->status != CX_NAND_PENDING && r->quiescent)
        return r->status;
    if ((uint32_t)(now - r->started) >= r->timeout) {
        if (!r->cancel) {
            r->cancel = true;
            r->error = CX_NAND_TIMEOUT;
        }
        if (r->error == CX_NAND_TIMEOUT && (uint32_t)(now - r->started - r->timeout) >= r->timeout)
            r->status = CX_NAND_ERROR;
    }
    if (r->cancel && r->phase != DRAIN_ISSUE && r->phase != DRAIN_READ) {
        if (r->quiescent)
            return terminal(r, r->error ? CX_NAND_ERROR : CX_NAND_CANCELED);
        r->phase = DRAIN_ISSUE;
    }
    if (r->phase == STATUS_ISSUE || r->phase == DRAIN_ISSUE) {
        bool draining = r->phase == DRAIN_ISSUE;
        wr(r, CMD_STATUS, 0);
        r->quiescent = false;
        r->phase = draining ? DRAIN_READ : STATUS_READ;
    } else if (r->phase == STATUS_READ || r->phase == DRAIN_READ) {
        bool draining = r->phase == DRAIN_READ;
        r->last_chip_status = r->bus.read8(r->bus.context, DATA);
        if (!(r->last_chip_status & 0x40U)) {
            r->phase = draining ? DRAIN_ISSUE : STATUS_ISSUE;
            return r->status;
        }
        r->quiescent = true;
        if (draining)
            return terminal(r, r->error ? CX_NAND_ERROR : CX_NAND_CANCELED);
        if (r->last_chip_status & 1U) {
            r->error = CX_NAND_CHIP_FAILURE;
            return terminal(r, CX_NAND_ERROR);
        }
        r->phase = r->identifying ? ID_ISSUE : PAGE_ISSUE;
    } else if (r->phase == ID_ISSUE) {
        wr(r, CMD_ID, 0);
        r->quiescent = false;
        r->phase = ID_READ;
    } else if (r->phase == ID_READ) {
        for (unsigned i = 0; i < 4U; ++i)
            r->id[i] = r->bus.read8(r->bus.context, DATA);
        r->quiescent = true;
        if (!supported_id(r->id)) {
            r->error = CX_NAND_UNSUPPORTED_GEOMETRY;
            return terminal(r, CX_NAND_ERROR);
        }
        r->identified = true;
        return terminal(r, CX_NAND_DONE);
    } else if (r->phase == PAGE_ISSUE) {
        /* Issue 00 + two column/two row addresses + 30. */
        wr(r, CMD_PAGE, (r->page << 16) | r->read_column);
        r->quiescent = false;
        r->phase = PAGE_READY;
    } else if (r->phase == PAGE_READY) {
        wr(r, CMD_STATUS, 0);
        r->phase = PAGE_STATUS_READ;
    } else if (r->phase == PAGE_STATUS_READ) {
        r->last_chip_status = r->bus.read8(r->bus.context, DATA);
        if (!(r->last_chip_status & 0x40U)) {
            r->phase = PAGE_READY;
        } else if (r->last_chip_status & 1U) {
            r->quiescent = true;
            r->error = CX_NAND_CHIP_FAILURE;
            return terminal(r, CX_NAND_ERROR);
        } else {
            r->phase = PAGE_READ_MODE;
        }
    } else if (r->phase == PAGE_READ_MODE) {
        /* STATUS temporarily changes the output stream. Native CX reads
         * send a command-only 00 before fetching data at the retained column. */
        wr(r, CMD_READ_MODE, 0);
        r->phase = PAGE_DATA;
    } else if (r->phase == PAGE_DATA) {
        if (budget > 128U)
            budget = 128U;
        budget &= ~3U;
        while (budget && r->position < r->transfer_bytes) {
            uint32_t word = rd(r, DATA);
            for (unsigned i = 0; i < 4U; ++i)
                r->data[r->position + i] = (uint8_t)(word >> (8U * i));
            r->position += 4U;
            budget -= 4U;
        }
        if (r->position == r->transfer_bytes) {
            r->quiescent = true;
            if (r->raw_tag) {
                r->position = 0;
                r->phase = PUBLISH;
            } else {
                r->phase = PAGE_ECC;
            }
        }
    } else if (r->phase == PAGE_ECC) {
        bool ffx = r->layout == CX_NAND_ECC_FLASHFX256;
        unsigned sector_bytes = ffx ? 256U : 512U;
        unsigned code_offset = ffx ? 2048U + (r->sector / 2U) * 16U + (r->sector & 1U ? 8U : 2U)
                                   : 2056U + r->sector * 16U;
        int result = correct(r->data + r->sector * sector_bytes, r->data + code_offset,
                             sector_bytes, ffx ? 11U : 12U);
        if (result < 0) {
            r->error = CX_NAND_ECC;
            return terminal(r, CX_NAND_ERROR);
        }
        r->corrected_data += result == 1;
        r->corrected_codes += result == 2;
        if (++r->sector == (ffx ? 8U : 4U)) {
            r->position = 0;
            r->phase = PUBLISH;
        }
    } else if (r->phase == PUBLISH) {
        if (budget > 128U)
            budget = 128U;
        if (budget > r->bytes - r->position)
            budget = r->bytes - r->position;
        memcpy(r->destination + r->position, r->data + (r->raw_tag ? 0U : r->column) + r->position,
               budget);
        r->position += budget;
        if (r->position == r->bytes)
            return terminal(r, CX_NAND_DONE);
    }
    return r->status;
}
