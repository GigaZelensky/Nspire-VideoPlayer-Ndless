#include "raw_file_reader.h"
#include <string.h>
static RawFileStatus fail(RawFileReader *r, unsigned error)
{
    r->error = error;
    r->status = RAW_FILE_ERROR;
    return r->status;
}
bool raw_file_init(RawFileReader *r, const uint8_t *state, size_t bytes, uint32_t owner,
                   uint32_t blocks, uint32_t index, uint32_t inode, NandPageConfig bus,
                   NandAddressMap layout, const RawFileOverlay *overlay, uint32_t count)
{
    if (!r || !state || bytes != FFX_MAP_STATE_BYTES || count > 32U || (count && !overlay))
        return false;
    memset(r, 0, sizeof(*r));
    memcpy(r->state, state, bytes);
    r->layout = layout;
    r->overlay = overlay;
    r->overlay_count = count;
    FfxMapRegion empty[3] = {{0, 0}, {0, 0}, {0, 0}};
    if (!ffx_map_init(&r->map, r->state, bytes, owner, blocks, empty) ||
        !rel_reader_init(&r->file, 2048, blocks, index, inode, r->workspace,
                         sizeof(r->workspace)) ||
        !nand_page_init(&r->nand, bus) || !nand_page_tag_column(&r->nand))
        return false;
    for (unsigned i = 0; i < count; ++i) {
        if (!overlay[i].data || overlay[i].block >= blocks)
            return false;
        for (unsigned j = 0; j < i; ++j)
            if (overlay[i].block == overlay[j].block)
                return false;
    }
    r->initialized = true;
    return true;
}
bool raw_file_metadata_cache(RawFileReader *r, const RawFileOverlay *cache, uint32_t count)
{
    if (!r || !r->initialized || r->status == RAW_FILE_PENDING || count > 1024U ||
        (count && !cache))
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (!cache[i].data || cache[i].block >= r->file.block_count)
            return false;
    r->metadata_overlay = cache;
    r->metadata_overlay_count = count;
    return true;
}
bool raw_file_begin(RawFileReader *r, uint64_t offset, void *destination, uint32_t bytes)
{
    if (!r || !r->initialized || r->status == RAW_FILE_PENDING ||
        r->nand.status == NAND_PAGE_PENDING || !r->nand.quiescent)
        return false;
    if (!rel_reader_begin(&r->file, offset, destination, bytes))
        return false;
    r->physical = 0;
    r->loading = false;
    r->copying = false;
    r->canceled = false;
    r->error = 0;
    r->status = RAW_FILE_PENDING;
    return true;
}
void raw_file_cancel(RawFileReader *r)
{
    if (r && r->status == RAW_FILE_PENDING) {
        r->canceled = true;
        nand_page_cancel(&r->nand);
    }
}
static bool start_media(RawFileReader *r, uint32_t offset, uint32_t column, uint32_t bytes,
                        uint64_t token, unsigned source)
{
    r->media_offset = offset;
    r->column = column;
    r->bytes = bytes;
    r->token = token;
    r->source = source;
    if (!nand_address_begin(&r->address, &r->layout, offset)) {
        fail(r, 10);
        return false;
    }
    r->physical = 1;
    return true;
}
RawFileStatus raw_file_step(RawFileReader *r, uint32_t now, uint32_t timeout)
{
    if (!r)
        return RAW_FILE_ERROR;
    if (r->status != RAW_FILE_PENDING) {
        /* A timed-out CX chip may still become ready later. Physical ownership
         * remains retained on ERROR until the provider reports quiescence. */
        if (r->initialized && !r->nand.quiescent) {
            nand_page_cancel(&r->nand);
            nand_page_step(&r->nand, now, 128);
        }
        return r->status;
    }
    if (r->canceled) {
        if (r->nand.status == NAND_PAGE_PENDING || !r->nand.quiescent) {
            nand_page_cancel(&r->nand);
            nand_page_step(&r->nand, now, 128);
            return r->status;
        }
        rel_reader_cancel(&r->file);
        r->loading = false;
        r->copying = false;
        r->physical = 0;
        r->status = r->nand.quiescent ? RAW_FILE_CANCELED : RAW_FILE_ERROR;
        return r->status;
    }
    if (r->physical == 1) {
        NandAddressStatus status = nand_address_step(&r->address, 64);
        if (status == NAND_ADDRESS_ERROR)
            return fail(r, 11);
        if (status == NAND_ADDRESS_READY) {
            if (!nand_page_begin_range(&r->nand, r->address.raw_page, r->column, r->io, r->bytes,
                                       now, timeout))
                return fail(r, 12);
            ++r->page_reads;
            unsigned kind =
                r->source == 1U
                    ? (r->bytes == 2112U                             ? RAW_READ_FFX_HEADER
                       : r->column == nand_page_tag_column(&r->nand) ? RAW_READ_FFX_TAG
                                                                     : RAW_READ_FFX_BITMAP)
                    : (r->file.pending_metadata ? RAW_READ_REL_METADATA : RAW_READ_PAYLOAD);
            ++r->costs.physical[kind];
            r->physical = 2;
        }
        return r->status;
    }
    if (r->physical == 2) {
        NandPageStatus status = nand_page_step(&r->nand, now, 128);
        if (status == NAND_PAGE_ERROR)
            return fail(r, 13);
        if (status == NAND_PAGE_DONE) {
            r->physical = 0;
            if (r->source == 1) {
                if (!ffx_region_supply(&r->loader, r->token, r->io, r->bytes))
                    return fail(r, 14);
            } else if (!rel_reader_supply(&r->file, r->token, r->io, r->bytes))
                return fail(r, 15);
        }
        return r->status;
    }
    if (r->copying) {
        uint32_t n = FFX_MAP_REGION_BYTES - r->copy_position;
        if (n > 128)
            n = 128;
        memcpy(r->cache[r->copy_slot] + r->copy_position, r->loader.data + r->copy_position, n);
        r->copy_position += n;
        if (r->copy_position == FFX_MAP_REGION_BYTES) {
            r->map.region[r->copy_slot] =
                (FfxMapRegion){r->cache[r->copy_slot], FFX_MAP_REGION_BYTES};
            r->ages[r->copy_slot] = ++r->age;
            r->copying = false;
            r->loading = false;
            ++r->regions_loaded;
            if (r->loader.id < RAW_FILE_REGION_HISTOGRAM_SLOTS)
                ++r->costs.region[r->loader.id];
            else
                ++r->costs.region_other;
        }
        return r->status;
    }
    if (r->loading) {
        FfxRegionStatus status = ffx_region_step(&r->loader);
        if (status == FFX_REGION_ERROR)
            return fail(r, 16);
        if (status == FFX_REGION_NEED_PAGE) {
            FfxRegionRequest request;
            if (!ffx_region_request(&r->loader, &request))
                return fail(r, 17);
            start_media(r, request.media_offset, request.column, request.bytes, request.token, 1);
        } else if (status == FFX_REGION_DONE) {
            unsigned victim = 0;
            for (unsigned i = 0; i < 3; ++i) {
                if (!r->map.region[i].data) {
                    victim = i;
                    break;
                }
                if (r->ages[i] < r->ages[victim])
                    victim = i;
            }
            r->copy_slot = victim;
            r->copy_position = 0;
            r->copying = true;
            r->map.region[victim] = (FfxMapRegion){0, 0}; /* Cancel cannot expose partial data. */
        }
        return r->status;
    }
    RelStatus status = rel_reader_step(&r->file, 128);
    if (status == REL_ERROR)
        return fail(r, 20 + (unsigned)r->file.error);
    if (status == REL_DONE) {
        r->status = RAW_FILE_DONE;
        return r->status;
    }
    if (status == REL_NEED_BLOCK) {
        RelRequest request;
        if (!rel_reader_request(&r->file, &request))
            return fail(r, 30);
        for (unsigned i = 0; i < r->overlay_count; ++i)
            if (r->overlay[i].block == request.block) {
                if (!rel_reader_supply(&r->file, request.token, r->overlay[i].data, 2048))
                    return fail(r, 31);
                ++r->overlay_reads;
                return r->status;
            }
        if (r->file.pending_metadata) {
            const uint8_t *data = NULL;
            for (unsigned i = 0; i < r->metadata_overlay_count; ++i)
                if (r->metadata_overlay[i].block == request.block) {
                    if (data)
                        return fail(r, 33); /* Ambiguous live cache entry. */
                    data = r->metadata_overlay[i].data;
                }
            if (data) {
                if (!rel_reader_supply(&r->file, request.token, data, 2048))
                    return fail(r, 34);
                ++r->metadata_reads;
                return r->status;
            }
        }
        FfxMapAddress address;
        FfxMapStatus mapped = ffx_map_page(&r->map, request.block, &address);
        if (mapped == FFX_MAP_NEED_REGION) {
            if (!ffx_region_begin_layout(&r->loader, r->state, sizeof(r->state), r->map.owner,
                                         address.region_id, nand_page_tag_column(&r->nand)))
                return fail(r, 32);
            ++r->costs.regions_started;
            r->loading = true;
        } else if (mapped == FFX_MAP_OK) {
            for (unsigned i = 0; i < 3; ++i)
                if (r->map.region[i].data) {
                    const uint8_t *d = r->map.region[i].data;
                    if (((uint32_t)d[4] | ((uint32_t)d[5] << 8)) == address.region_id)
                        r->ages[i] = ++r->age;
                }
            start_media(r, address.media_byte_offset, 0, 2048, request.token, 0);
        } else
            return fail(r, 40 + (unsigned)mapped);
    }
    return r->status;
}
