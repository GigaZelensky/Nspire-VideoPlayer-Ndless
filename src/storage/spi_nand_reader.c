#include "spi_nand_reader.h"
#include <string.h>

enum {
    READY_ISSUE,
    READY_WAIT,
    PAGE_ISSUE,
    PAGE_WAIT,
    STATUS_ISSUE,
    STATUS_WAIT,
    CACHE_ISSUE,
    CACHE_DRAIN,
    RESET_WAIT
};
enum {
    CONTROL = 0x10U,
    STATUS = 0x18U,
    INT_ENABLE = 0x20U,
    INT_STATUS = 0x24U,
    RESPONSE = 0x28U,
    FEATURES = 0x54U,
    FIFO = 0x100U
};
static uint32_t rd(SpiNandReader *r, uint32_t reg)
{
    return r->bus.read32(r->bus.context, reg);
}
static void wr(SpiNandReader *r, uint32_t reg, uint32_t value)
{
    r->bus.write32(r->bus.context, reg, value);
}
static void issue(SpiNandReader *r, uint32_t address, uint32_t format, uint32_t bytes,
                  uint32_t command)
{
    /* Exact register ordering of 1001A5E8 / 1001A558. Writing the command
     * register can launch the transfer; never restore old command registers. */
    wr(r, INT_STATUS, rd(r, INT_STATUS) | 1U);
    wr(r, 0, address);
    wr(r, 4, format);
    wr(r, 8, bytes);
    wr(r, 12, command);
    wr(r, INT_ENABLE, rd(r, INT_ENABLE) | 2U);
    r->quiescent = false;
}
static bool completed(SpiNandReader *r)
{
    if (!(rd(r, INT_STATUS) & 1U))
        return false;
    wr(r, INT_STATUS, rd(r, INT_STATUS) | 1U);
    wr(r, INT_ENABLE, r->saved_enable);
    r->quiescent = true;
    return true;
}
static void terminal(SpiNandReader *r, SpiNandStatus status)
{
    r->status = status;
    r->destination = NULL;
}
static void abort_transfer(SpiNandReader *r, uint32_t now)
{
    /* Reset the SPI controller/FIFO, never NAND program/erase or device reset.
     * CTRL bit 8 is the same self-clearing reset used by 1001A358. */
    wr(r, INT_ENABLE, rd(r, INT_ENABLE) & ~2U);
    wr(r, CONTROL, rd(r, CONTROL) | 0x100U);
    r->phase = RESET_WAIT;
    r->reset_started = now;
    r->quiescent = false;
}
bool spi_nand_init(SpiNandReader *r, SpiNandBus bus)
{
    if (!r || !bus.read32 || !bus.read8 || !bus.write32)
        return false;
    memset(r, 0, sizeof(*r));
    r->bus = bus;
    r->quiescent = true;
    return true;
}
bool spi_nand_begin(SpiNandReader *r, uint32_t page, void *destination, size_t bytes, uint32_t now,
                    uint32_t timeout)
{
    if (bytes != 2048U) {
        if (r && r->status != SPI_NAND_PENDING)
            r->error = SPI_NAND_BAD_ARGUMENT;
        return false;
    }
    return spi_nand_begin_range(r, page, 0, destination, bytes, now, timeout);
}
bool spi_nand_begin_range(SpiNandReader *r, uint32_t page, uint32_t column, void *destination,
                          size_t bytes, uint32_t now, uint32_t timeout)
{
    if (!r || !r->bus.read32 || !r->bus.read8 || !r->bus.write32)
        return false;
    if (r->status == SPI_NAND_PENDING || !r->quiescent)
        return false;
    r->error = SPI_NAND_OK;
    uintptr_t dest = (uintptr_t)destination, object = (uintptr_t)r;
    if (!destination || !bytes || column >= 2112U || bytes > 2112U - column ||
        dest > UINTPTR_MAX - bytes || (dest < object + sizeof(*r) && object < dest + bytes) ||
        page >= 65536U || !timeout || timeout >= 0x80000000U) {
        r->error = SPI_NAND_BAD_ARGUMENT;
        return false;
    }
    uint32_t control = rd(r, CONTROL), enable = rd(r, INT_ENABLE);
    r->initial_status = rd(r, STATUS);
    r->initial_features = rd(r, FEATURES);
    r->initial_interrupt = rd(r, INT_STATUS);
    r->fifo_depth = (r->initial_features >> 8) & 255U;
    if ((control & 0x100100U) || (enable & 2U) || (r->initial_status & 2U)) {
        r->error = SPI_NAND_NOT_IDLE;
        return false;
    }
    if (!r->fifo_depth) {
        r->error = SPI_NAND_BAD_ARGUMENT;
        return false;
    }
    r->destination = destination;
    r->page = page;
    r->copied = 0;
    r->started = now;
    r->timeout = timeout;
    r->column = column;
    r->requested = (uint32_t)bytes;
    r->saved_enable = enable;
    r->saved_control = control;
    r->cancel = false;
    r->corrected = false;
    r->last_status = 0;
    r->rx_credit = 0;
    r->phase = READY_ISSUE;
    r->status = SPI_NAND_PENDING;
    return true;
}
void spi_nand_cancel(SpiNandReader *r)
{
    if (r && r->status == SPI_NAND_PENDING)
        r->cancel = true;
}
SpiNandStatus spi_nand_step(SpiNandReader *r, uint32_t now, uint32_t budget)
{
    if (!r)
        return SPI_NAND_ERROR;
    /* A reset timeout reports ERROR but does not release controller ownership.
     * Keep polling cleanup on later steps so a delayed reset can finish and
     * restore the saved registers before another reader/writer uses the bus. */
    if (r->phase == RESET_WAIT && !r->quiescent) {
        if (!(rd(r, CONTROL) & 0x100U)) {
            wr(r, CONTROL, r->saved_control);
            wr(r, INT_STATUS, rd(r, INT_STATUS) | 1U);
            wr(r, INT_ENABLE, r->saved_enable);
            r->quiescent = true;
            terminal(r, SPI_NAND_ERROR);
        } else if ((uint32_t)(now - r->reset_started) >= r->timeout) {
            r->error = SPI_NAND_RESET_FAILED;
            terminal(r, SPI_NAND_ERROR);
        }
        return r->status;
    }
    if (r->status != SPI_NAND_PENDING)
        return r->status;
    if ((uint32_t)(now - r->started) >= r->timeout) {
        r->error = SPI_NAND_TIMEOUT;
        if (r->quiescent)
            terminal(r, SPI_NAND_ERROR);
        else
            abort_transfer(r, now);
        return r->status;
    }
    switch (r->phase) {
    case READY_ISSUE:
        if (r->cancel) {
            terminal(r, SPI_NAND_CANCELED);
            break;
        }
        issue(r, 0xC0U, 0x01000001U, 0, 0x0F00010CU);
        r->phase = READY_WAIT;
        break;
    case READY_WAIT:
        if (completed(r)) {
            r->last_status = rd(r, RESPONSE) & 255U;
            r->phase = (r->last_status & 1U) ? READY_ISSUE : PAGE_ISSUE;
        }
        break;
    case PAGE_ISSUE:
        if (r->cancel) {
            terminal(r, SPI_NAND_CANCELED);
            break;
        }
        issue(r, r->page, 0x01000003U, 0, 0x13000102U);
        r->phase = PAGE_WAIT;
        break;
    case PAGE_WAIT:
        if (completed(r))
            r->phase = STATUS_ISSUE;
        break;
    case STATUS_ISSUE:
        issue(r, 0xC0U, 0x01000001U, 0, 0x0F00010CU);
        r->phase = STATUS_WAIT;
        break;
    case STATUS_WAIT:
        if (completed(r)) {
            r->last_status = rd(r, RESPONSE) & 255U;
            if (r->last_status & 1U) {
                r->phase = STATUS_ISSUE;
                break;
            }
            if (r->cancel) {
                terminal(r, SPI_NAND_CANCELED);
                break;
            }
            unsigned ecc = (r->last_status >> 4) & 7U;
            if (ecc > 1U) {
                r->error = SPI_NAND_ECC;
                terminal(r, SPI_NAND_ERROR);
                break;
            }
            r->corrected = ecc == 1U;
            r->phase = CACHE_ISSUE;
        }
        break;
    case CACHE_ISSUE:
        if (r->cancel) {
            terminal(r, SPI_NAND_CANCELED);
            break;
        }
        issue(r, (((r->page / 64U) & 1U) << 12) | r->column, 0x01080002U, r->requested,
              0x0B000100U);
        r->phase = CACHE_DRAIN;
        break;
    case CACHE_DRAIN: {
        /* 0x54 is constant capacity, not occupancy. The native receive loop
         * waits for STATUS.RX_READY before draining one capacity-sized burst.
         * Keep the undrained part across small-budget steps; do not wait for
         * another full-burst indication halfway through the final burst. */
        if (!r->rx_credit && r->copied < r->requested && (rd(r, STATUS) & 2U)) {
            r->rx_credit = r->fifo_depth;
            if (r->rx_credit > r->requested - r->copied)
                r->rx_credit = r->requested - r->copied;
        }
        uint32_t n = r->requested - r->copied;
        if (n > r->rx_credit)
            n = r->rx_credit;
        if (n > budget)
            n = budget;
        if (n > 128U)
            n = 128U;
        uint8_t *destination = r->destination + r->copied;
        r->rx_credit -= n;
        r->copied += n;
        while (n >= 4U) {
            uint32_t value = rd(r, FIFO);
            destination[0] = (uint8_t)value;
            destination[1] = (uint8_t)(value >> 8);
            destination[2] = (uint8_t)(value >> 16);
            destination[3] = (uint8_t)(value >> 24);
            destination += 4;
            n -= 4U;
        }
        while (n--)
            *destination++ = r->bus.read8(r->bus.context, FIFO);
        if (r->copied == r->requested && completed(r))
            terminal(r, r->cancel ? SPI_NAND_CANCELED : SPI_NAND_DONE);
        break;
    }
    default:
        r->error = SPI_NAND_BAD_ARGUMENT;
        abort_transfer(r, now);
        break;
    }
    return r->status;
}
