#include "nand_page_reader.h"
#include <string.h>
#define SPI_BASE 0xb8000000U
#define ADAPTER_ARGUMENT_ERROR 0x100U
#define ADAPTER_TIMEOUT_ERROR 0x101U

static uint32_t spi_read32(void *ctx, uint32_t offset)
{
    NandPageReader *r = ctx;
    return r->config.bus.read32(r->config.bus.context, SPI_BASE + offset);
}
static uint8_t spi_read8(void *ctx, uint32_t offset)
{
    NandPageReader *r = ctx;
    return r->config.bus.read8(r->config.bus.context, SPI_BASE + offset);
}
static void spi_write32(void *ctx, uint32_t offset, uint32_t value)
{
    NandPageReader *r = ctx;
    r->config.bus.write32(r->config.bus.context, SPI_BASE + offset, value);
}
static NandPageStatus cx_status(CxNandStatus status)
{
    switch (status) {
    case CX_NAND_IDLE:
        return NAND_PAGE_IDLE;
    case CX_NAND_PENDING:
        return NAND_PAGE_PENDING;
    case CX_NAND_DONE:
        return NAND_PAGE_DONE;
    case CX_NAND_CANCELED:
        return NAND_PAGE_CANCELED;
    default:
        return NAND_PAGE_ERROR;
    }
}
static NandPageStatus spi_status(SpiNandStatus status)
{
    switch (status) {
    case SPI_NAND_IDLE:
        return NAND_PAGE_IDLE;
    case SPI_NAND_PENDING:
        return NAND_PAGE_PENDING;
    case SPI_NAND_DONE:
        return NAND_PAGE_DONE;
    case SPI_NAND_CANCELED:
        return NAND_PAGE_CANCELED;
    default:
        return NAND_PAGE_ERROR;
    }
}
static void collect(NandPageReader *r)
{
    if (r->config.kind == NAND_PAGE_CX_PL351) {
        r->status = cx_status(r->engine.cx.status);
        r->error = r->engine.cx.error;
        r->quiescent = r->engine.cx.quiescent;
        r->corrected = r->engine.cx.corrected_data || r->engine.cx.corrected_codes;
    } else {
        r->status = spi_status(r->engine.spi.status);
        r->error = r->engine.spi.error;
        r->quiescent = r->engine.spi.quiescent;
        r->corrected = r->engine.spi.corrected;
    }
}
bool nand_page_init(NandPageReader *r, NandPageConfig config)
{
    if (!r || !config.bus.read32 || !config.bus.read8 || !config.bus.write32)
        return false;
    memset(r, 0, sizeof(*r));
    r->config = config;
    r->quiescent = true;
    if (config.kind == NAND_PAGE_CX_PL351 &&
        (config.spare == NAND_SPARE_CX_BOOT512 || config.spare == NAND_SPARE_CX_FLASHFX256_TAG12))
        r->initialized = cx_nand_init_layout(
            &r->engine.cx, config.bus,
            config.spare == NAND_SPARE_CX_BOOT512 ? CX_NAND_ECC_BOOT512 : CX_NAND_ECC_FLASHFX256);
    else if (config.kind == NAND_PAGE_CX2_SPI && config.spare == NAND_SPARE_CX2_ONDIE_TAG4) {
        SpiNandBus bus = {r, spi_read32, spi_read8, spi_write32};
        r->initialized = spi_nand_init(&r->engine.spi, bus);
    } else {
        r->status = NAND_PAGE_ERROR;
        r->error = ADAPTER_ARGUMENT_ERROR;
        return false;
    }
    collect(r);
    return r->initialized;
}
uint32_t nand_page_tag_column(const NandPageReader *r)
{
    if (!r || !r->initialized)
        return 0;
    if (r->config.spare == NAND_SPARE_CX2_ONDIE_TAG4)
        return 2052U;
    if (r->config.spare == NAND_SPARE_CX_FLASHFX256_TAG12)
        return 2060U;
    return 0;
}
bool nand_page_begin_range(NandPageReader *r, uint32_t page, uint32_t column, void *destination,
                           size_t bytes, uint32_t now, uint32_t timeout)
{
    if (!r || !r->initialized || r->status == NAND_PAGE_PENDING || !r->quiescent)
        return false;
    uintptr_t dest = (uintptr_t)destination, object = (uintptr_t)r;
    if (!destination || !bytes || column >= 2112U || bytes > 2112U - column || page >= 65536U ||
        !timeout || timeout >= 0x80000000U || dest > UINTPTR_MAX - bytes ||
        (dest < object + sizeof(*r) && object < dest + bytes)) {
        r->error = ADAPTER_ARGUMENT_ERROR;
        return false;
    }
    bool ok;
    r->probing = false;
    if (r->config.kind == NAND_PAGE_CX2_SPI)
        ok = spi_nand_begin_range(&r->engine.spi, page, column, destination, bytes, now, timeout);
    else if (r->engine.cx.identified)
        ok = cx_nand_begin_range(&r->engine.cx, page, column, destination, bytes, now, timeout);
    else {
        ok = cx_nand_identify(&r->engine.cx, now, timeout);
        r->probing = ok;
    }
    collect(r);
    if (!ok)
        return false;
    r->destination = destination;
    r->page = page;
    r->column = column;
    r->bytes = (uint32_t)bytes;
    r->started = now;
    r->timeout = timeout;
    return true;
}
void nand_page_cancel(NandPageReader *r)
{
    if (!r || !r->initialized)
        return;
    if (r->config.kind == NAND_PAGE_CX_PL351)
        cx_nand_cancel(&r->engine.cx);
    else
        spi_nand_cancel(&r->engine.spi);
}
NandPageStatus nand_page_step(NandPageReader *r, uint32_t now, uint32_t budget)
{
    if (!r)
        return NAND_PAGE_ERROR;
    if (!r->initialized)
        return r->status;
    if (r->config.kind == NAND_PAGE_CX2_SPI) {
        spi_nand_step(&r->engine.spi, now, budget);
        collect(r);
        return r->status;
    }
    cx_nand_step(&r->engine.cx, now, budget);
    collect(r);
    if (r->probing && r->status == NAND_PAGE_DONE) {
        r->probing = false;
        uint32_t elapsed = now - r->started;
        if (elapsed >= r->timeout) {
            r->status = NAND_PAGE_ERROR;
            r->error = ADAPTER_TIMEOUT_ERROR;
            r->engine.cx.status = CX_NAND_ERROR;
            r->engine.cx.error = CX_NAND_TIMEOUT;
        } else if (!cx_nand_begin_range(&r->engine.cx, r->page, r->column, r->destination, r->bytes,
                                        now, r->timeout - elapsed)) {
            r->engine.cx.status = CX_NAND_ERROR;
            collect(r);
        } else
            collect(r);
    } else if (r->probing && r->status != NAND_PAGE_PENDING)
        r->probing = false;
    return r->status;
}
