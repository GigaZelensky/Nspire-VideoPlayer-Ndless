/* Single-threaded, resumable AV1 decoding for Ndless.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "av1_decoder.h"
#include "av1/dav1d/src/decode.h"
#include "av1/dav1d/src/ndless_scratch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct av1_decoder {
    Dav1dContext *context;
    void *working_memory;
    Dav1dPicture picture, previous;
    uint64_t frame_serial;
    bool repeats_previous;
    av1_frame_t frame;
    unsigned done, total, block_size, last_blocks;
    bool held, pending, failed, have_sequence, reduced_header;
    int64_t pts;
    char error[192];
};

typedef struct {
    bool sequence, reduced, keyframe, existing, visible;
} PacketInfo;

static bool packet_info(const uint8_t *data, size_t bytes, bool reduced, PacketInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->reduced = reduced;
    unsigned frames = 0, tiles = 0, obus = 0;
    bool frame_obu = false;
    for (size_t pos = 0; pos < bytes;) {
        uint8_t header = data[pos++];
        if ((header & 0x81U) || ++obus > 64U) return false;
        unsigned type = (header >> 3U) & 15U;
        if (header & 4U) {
            if (pos == bytes || data[pos++] != 0) return false;
        }
        size_t size = bytes - pos;
        if (header & 2U) {
            uint64_t length = 0;
            unsigned i;
            for (i = 0; i < 8U; ++i) {
                if (pos == bytes) return false;
                uint8_t b = data[pos++];
                length |= (uint64_t)(b & 127U) << (7U * i);
                if (!(b & 128U)) break;
            }
            if (i == 8U || length > bytes - pos) return false;
            size = (size_t)length;
        }
        if (type == 1U) {
            if (!size || frames) return false;
            info->sequence = true;
            info->reduced = (data[pos] & 8U) != 0;
        } else if (type == 3U || type == 6U) {
            if (!size || ++frames != 1U) return false;
            frame_obu = type == 6U;
            info->existing = !info->reduced && (data[pos] & 128U);
            info->keyframe = info->reduced || (!info->existing && !(data[pos] & 96U));
            info->visible = info->reduced || info->existing || (data[pos] & 16U);
        } else if (type == 4U) {
            if (!frames || frame_obu || !size) return false;
            ++tiles;
        } else if (type != 2U && type != 5U && type != 15U) {
            return false;
        }
        pos += size;
    }
    return frames == 1U && info->visible && (frame_obu || info->existing || tiles);
}

bool av1_packet_is_independent(const void *data, size_t bytes)
{
    PacketInfo info;
    return data && bytes && bytes <= 262144U && packet_info(data, bytes, false, &info) &&
        info.sequence && info.keyframe;
}

static void decoder_log(void *cookie, const char *format, va_list args)
{
    av1_decoder_t *d = cookie;
    vsnprintf(d->error, sizeof(d->error), format, args);
    d->error[strcspn(d->error, "\r\n")] = 0;
}

static av1_status_t fail(av1_decoder_t *d, const char *reason)
{
    if (reason) snprintf(d->error, sizeof(d->error), "%s", reason);
    if (!d->error[0]) snprintf(d->error, sizeof(d->error), "AV1 decoding failed");
    d->failed = true;
    return AV1_ERROR;
}

av1_decoder_t *av1_create(void)
{
    av1_decoder_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = settings.max_frame_delay = 1;
    settings.frame_size_limit = 320U * 240U;
    settings.apply_grain = 0;
    settings.strict_std_compliance = 1;
    settings.logger.cookie = d;
    settings.logger.callback = decoder_log;
    if (dav1d_open(&d->context, &settings) < 0) { free(d); return NULL; }
    d->context->ndless_cooperative = 1;
    d->block_size = 64;
    return d;
}

size_t av1_working_memory_size(void) { return DAV1D_NDLESS_SCRATCH_BYTES; }

av1_decoder_t *av1_create_with_memory(void *memory, size_t bytes)
{
    if (!memory || ((uintptr_t)memory & 31U) || bytes < av1_working_memory_size()) return NULL;
    av1_decoder_t *d = av1_create();
    if (d) d->working_memory = memory;
    return d;
}

const void *av1_external_memory(const av1_decoder_t *d) { return d ? d->working_memory : NULL; }

void av1_release_frame(av1_decoder_t *d)
{
    if (!d || !d->held) return;
    /* Move the output reference, retaining its identity without copying YUV. */
    dav1d_picture_unref(&d->previous);
    d->previous = d->picture;
    memset(&d->picture, 0, sizeof(d->picture));
    d->repeats_previous = false;
    memset(&d->frame, 0, sizeof(d->frame));
    d->held = d->pending = false;
}

void av1_reset(av1_decoder_t *d)
{
    if (!d) return;
    av1_release_frame(d);
    dav1d_flush(d->context);
    d->context->ndless_repeat_previous = NULL;
    dav1d_picture_unref(&d->previous);
    d->repeats_previous = false;
    d->pending = d->failed = d->have_sequence = d->reduced_header = false;
    d->done = d->total = d->last_blocks = 0;
    d->block_size = 64;
    d->error[0] = 0;
}

void av1_destroy(av1_decoder_t *d)
{
    if (!d) return;
    av1_reset(d);
    dav1d_close(&d->context);
    free(d);
}

av1_status_t av1_submit_obus(av1_decoder_t *d, const void *data, size_t bytes, int64_t pts)
{
    if (!d || d->failed) return AV1_ERROR;
    if (d->held) return AV1_FRAME_READY;
    if (d->pending) return AV1_PROGRESS;
    PacketInfo info;
    if (!data || !bytes || bytes > 262144U || !packet_info(data, bytes, d->reduced_header, &info))
        return fail(d, "AV1 requires one complete visible frame per packet");
    if (info.sequence) {
        Dav1dSequenceHeader sequence;
        if (dav1d_parse_sequence_header(&sequence, data, bytes) < 0)
            return fail(d, "invalid AV1 sequence header");
        if (sequence.profile || sequence.hbd || sequence.layout != DAV1D_PIXEL_LAYOUT_I420 ||
            sequence.max_width <= 0 || sequence.max_width > 320 ||
            sequence.max_height <= 0 || sequence.max_height > 240 || sequence.film_grain_present)
            return fail(d, "AV1 requires Main 8-bit 4:2:0 up to 320x240, without film grain");
        d->block_size = sequence.sb128 ? 128U : 64U;
        d->reduced_header = sequence.reduced_still_picture_header;
        d->have_sequence = true;
    }
    if (!d->have_sequence) return fail(d, "AV1 sequence header missing");
    if (info.keyframe) {
        d->context->ndless_repeat_previous = NULL;
        dav1d_picture_unref(&d->previous);
    }
    Dav1dData input = {0};
    uint8_t *copy = dav1d_data_create(&input, bytes);
    if (!copy) return fail(d, "AV1 input allocation failed");
    memcpy(copy, data, bytes);
    input.m.timestamp = pts;
    d->context->ndless_repeat_previous = d->previous.ref ? &d->previous : NULL;
    d->repeats_previous = false;
    int result = dav1d_send_data(d->context, &input);
    dav1d_data_unref(&input);
    if (result < 0) return fail(d, NULL);
    if (!d->context->fc[0].ndless_pending && !d->context->out.p.data[0])
        return fail(d, "incomplete AV1 frame");
    d->done = d->last_blocks = 0;
    d->total = d->context->fc[0].ndless_pending ? d->context->fc[0].ndless_total : 0;
    d->pts = pts;
    d->pending = true;
    return AV1_PROGRESS;
}

static av1_status_t step_frame(av1_decoder_t *d, unsigned budget)
{
    if (!d || d->failed) return AV1_ERROR;
    d->last_blocks = 0;
    if (d->held) return AV1_FRAME_READY;
    if (!d->pending) return AV1_NEED_INPUT;
    Dav1dFrameContext *f = &d->context->fc[0];
    if (f->ndless_pending) {
        int result = dav1d_ndless_frame_step(f, budget);
        d->last_blocks = f->ndless_done - d->done;
        d->done = f->ndless_done;
        if (result == DAV1D_NDLESS_YIELD) return AV1_PROGRESS;
        if (result < 0) return fail(d, NULL);
    }
    if (dav1d_get_picture(d->context, &d->picture) < 0) return fail(d, NULL);
    /* Consume trailing metadata while the copied input still belongs to this
     * packet. A second output would violate the container's frame table. */
    Dav1dPicture extra = {0};
    int trailing = dav1d_get_picture(d->context, &extra);
    if (trailing != DAV1D_ERR(EAGAIN) || extra.data[0]) {
        dav1d_picture_unref(&extra);
        dav1d_picture_unref(&d->picture);
        return fail(d, "invalid trailing AV1 data");
    }
    d->held = true;
    if (d->picture.p.bpc != 8 || d->picture.p.layout != DAV1D_PIXEL_LAYOUT_I420 ||
        d->picture.p.w <= 0 || d->picture.p.w > 320 || d->picture.p.h <= 0 || d->picture.p.h > 240 ||
        ((d->picture.p.w | d->picture.p.h) & 1))
        return fail(d, "unsupported AV1 output dimensions or format");
    d->frame.width = d->frame.coded_width = d->picture.p.w;
    d->frame.height = d->frame.coded_height = d->picture.p.h;
    d->frame.pts = d->pts;
    for (unsigned p = 0; p < 3; ++p) {
        d->frame.plane[p] = d->picture.data[p];
        d->frame.stride[p] = d->picture.stride[p != 0];
    }
    d->repeats_previous = d->previous.ref &&
        d->previous.p.w == d->picture.p.w && d->previous.p.h == d->picture.p.h &&
        d->previous.p.bpc == d->picture.p.bpc &&
        d->previous.p.layout == d->picture.p.layout &&
        ((d->total && d->context->tc[0].ndless_repeat) ||
         (d->picture.ref == d->previous.ref &&
          d->picture.data[0] == d->previous.data[0] &&
          d->picture.data[1] == d->previous.data[1] &&
          d->picture.data[2] == d->previous.data[2] &&
          d->picture.stride[0] == d->previous.stride[0] &&
          d->picture.stride[1] == d->previous.stride[1]));
    ++d->frame_serial;
    return AV1_FRAME_READY;
}

av1_status_t av1_step_until(av1_decoder_t *d, unsigned budget,
                            uint64_t deadline_ticks, av1_clock_fn clock)
{
    if (!d) return AV1_ERROR;
    Dav1dTaskContext *const t = &d->context->tc[0];
    av1_clock_fn previous_clock = t->ndless_clock;
    uint64_t previous_deadline = t->ndless_deadline_ticks;
    Dav1dNdlessScratch *previous = dav1d_ndless_scratch;
    t->ndless_clock = clock;
    t->ndless_deadline_ticks = deadline_ticks;
    dav1d_ndless_scratch = d->working_memory;
    av1_status_t status = step_frame(d, budget);
    dav1d_ndless_scratch = previous;
    t->ndless_clock = previous_clock;
    t->ndless_deadline_ticks = previous_deadline;
    return status;
}

av1_status_t av1_step(av1_decoder_t *d, unsigned budget)
{
    return av1_step_until(d, budget, 0, NULL);
}

const av1_frame_t *av1_get_frame(const av1_decoder_t *d) { return d && d->held && !d->failed ? &d->frame : NULL; }
const char *av1_error_string(const av1_decoder_t *d) { return d ? d->error : "AV1 decoder allocation failed"; }
unsigned av1_last_step_blocks(const av1_decoder_t *d) { return d ? d->last_blocks : 0; }
unsigned av1_picture_blocks_done(const av1_decoder_t *d) { return d ? d->done : 0; }
unsigned av1_picture_blocks_total(const av1_decoder_t *d) { return d ? d->total : 0; }
unsigned av1_block_size(const av1_decoder_t *d) { return d ? d->block_size : 64U; }

bool av1_frame_repeats_previous(const av1_decoder_t *d) {
    return d && d->held && !d->failed && d->repeats_previous;
}
uint64_t av1_frame_serial(const av1_decoder_t *d) { return d ? d->frame_serial : 0; }
