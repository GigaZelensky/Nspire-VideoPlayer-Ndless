/* Single-core Ndless adapter for the bundled libde265 decoder.
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#include "hevc_decoder.h"
#include "hevc/libde265/decctx.h"
#include "hevc/libde265/image.h"
#include <new>
#include <string.h>

struct hevc_decoder {
    decoder_context *ctx;
    hevc_frame_t frame;
    bool held, pending, flushing, failed;
    const char *error;
    uint64_t total_ctus;
    unsigned last_ctus;
};

static hevc_status_t fail(hevc_decoder_t *d, const char *message)
{
    if (d) { d->error = message; d->failed = true; }
    return HEVC_ERROR;
}

static hevc_decoder_t *create_decoder(void *memory)
{
    hevc_decoder_t *d = new (std::nothrow) hevc_decoder_t();
    if (!d) return NULL;
    if (memory) {
        if (de265_init() != DE265_OK) { delete d; return NULL; }
        d->ctx = new (std::nothrow) decoder_context(memory);
        if (!d->ctx) de265_free();
    } else d->ctx = reinterpret_cast<decoder_context *>(de265_new_decoder());
    if (!d->ctx) { delete d; return NULL; }
    d->ctx->param_conceal_stream_errors = false;
    d->ctx->param_suppress_faulty_pictures = true;
    d->error = "no error";
    return d;
}

extern "C" hevc_decoder_t *hevc_create(void) { return create_decoder(NULL); }
extern "C" size_t hevc_working_memory_size(void) { return sizeof(HevcWorkingMemory); }
extern "C" hevc_decoder_t *hevc_create_with_memory(void *memory, size_t bytes)
{
    if (!memory || (reinterpret_cast<uintptr_t>(memory)&31U) || bytes<sizeof(HevcWorkingMemory)) return NULL;
    return create_decoder(memory);
}
extern "C" const void *hevc_external_memory(const hevc_decoder_t *d)
{
    return d && !d->ctx->owns_working_memory ? d->ctx->working_memory : NULL;
}

extern "C" void hevc_destroy(hevc_decoder_t *d)
{
    if (!d) return;
    de265_free_decoder(d->ctx);
    delete d;
}

extern "C" void hevc_reset(hevc_decoder_t *d)
{
    if (!d) return;
    de265_reset(d->ctx);
    while (de265_get_warning(d->ctx)!=DE265_OK) {}
    d->held = d->pending = d->flushing = d->failed = false;
    d->last_ctus = 0;
    d->total_ctus = 0;
    d->ctx->picture_ctus_done = d->ctx->picture_ctus_total = d->ctx->picture_ctu_size = 0;
    d->error = "no error";
    memset(&d->frame, 0, sizeof(d->frame));
}

static size_t start_code(const uint8_t *data, size_t length, size_t from, size_t *payload)
{
    for (size_t i=from; i+2<length; ++i) {
        if (!data[i] && !data[i+1] && data[i+2] == 1) {
            *payload = i+3;
            return i;
        }
    }
    *payload=length;
    return length;
}

extern "C" hevc_status_t hevc_submit_annexb(hevc_decoder_t *d,
                                            const void *input, size_t bytes, int64_t pts)
{
    if (!d || d->failed) return HEVC_ERROR;
    if (d->held) return HEVC_FRAME_READY;
    if (d->pending) return HEVC_PROGRESS;
    if (d->flushing) return fail(d,"cannot submit after flush; reset first");
    if (!input || bytes<6 || bytes>262144) return fail(d,"invalid HEVC access unit size");
    const uint8_t *data = static_cast<const uint8_t *>(input);
    size_t begin, next;
    size_t first = start_code(data,bytes,0,&begin);
    if (first>1 || (first==1 && data[0])) return fail(d,"missing Annex-B start code");
    unsigned pictures=0, nals=0;
    /* Validate the entire access unit before changing decoder state. */
    for (size_t pos=begin; pos<bytes; pos=next) {
        size_t end=start_code(data,bytes,pos,&next);
        while (end>pos && !data[end-1]) --end;
        if (end-pos<2 || (data[pos]&0x80) || !(data[pos+1]&7))
            return fail(d,"invalid HEVC NAL header");
        if ((data[pos]&1) || (data[pos+1]&0xf8))
            return fail(d,"HEVC multilayer streams are unsupported");
        unsigned type=(data[pos]>>1)&63;
        if (type<32) {
            if (end-pos<3) return fail(d,"truncated HEVC slice");
            pictures += (data[pos+2]&0x80) != 0;
        }
        if (++nals>64) return fail(d,"too many NAL units in one frame");
    }
    if (pictures!=1) return fail(d,"one complete HEVC picture is required per access unit");
    for (size_t pos=begin; pos<bytes; pos=next) {
        size_t end=start_code(data,bytes,pos,&next);
        while (end>pos && !data[end-1]) --end;
        de265_error err=de265_push_NAL(d->ctx,data+pos,static_cast<int>(end-pos),pts,NULL);
        if (err!=DE265_OK) return fail(d,de265_get_error_text(err));
    }
    de265_push_end_of_frame(d->ctx);
    d->pending=true;
    return HEVC_PROGRESS;
}

static hevc_status_t hold_frame(hevc_decoder_t *d, const de265_image *image)
{
    const seq_parameter_set& sps=image->get_sps();
    d->frame.width=de265_get_image_width(image,0);
    d->frame.height=de265_get_image_height(image,0);
    d->frame.coded_width=sps.pic_width_in_luma_samples;
    d->frame.coded_height=sps.pic_height_in_luma_samples;
    d->frame.crop_left=sps.conf_win_left_offset*2;
    d->frame.crop_top=sps.conf_win_top_offset*2;
    d->frame.pts=de265_get_image_PTS(image);
    if (!d->frame.width || !d->frame.height || d->frame.width>320 || d->frame.height>240 ||
        de265_get_chroma_format(image)!=de265_chroma_420 || de265_get_bits_per_pixel(image,0)!=8)
        return fail(d,"unsupported HEVC output format");
    for (int i=0; i<3; ++i)
        d->frame.plane[i]=de265_get_image_plane(image,i,&d->frame.stride[i]);
    d->held=true;
    return HEVC_FRAME_READY;
}

static hevc_status_t hevc_step_core(hevc_decoder_t *d, unsigned budget)
{
    if (!d || d->failed) return HEVC_ERROR;
    d->last_ctus=0;
    if (d->held) return HEVC_FRAME_READY;
    if (!d->pending && !d->flushing) return HEVC_NEED_INPUT;
    d->ctx->prepare_working_memory();
    d->ctx->ctu_budget=budget ? budget : 1;
    d->ctx->step_ctus=0;
    /* Bounded header-only work as well as CTU work: at most 16 NALs per call. */
    for (unsigned work=0; work<16; ++work) {
        int more=0;
        de265_error err=de265_decode(d->ctx,&more);
        d->total_ctus += d->ctx->step_ctus-d->last_ctus;
        d->last_ctus=d->ctx->step_ctus;
        if (err!=DE265_OK && err!=DE265_ERROR_COOPERATIVE_YIELD && err!=DE265_ERROR_WAITING_FOR_INPUT_DATA)
            return fail(d,de265_get_error_text(err));
        de265_error warning=de265_get_warning(d->ctx);
        if (warning!=DE265_OK) return fail(d,de265_get_error_text(warning));
        const de265_image *image=de265_peek_next_picture(d->ctx);
        if (image) return hold_frame(d,image);
        if (err==DE265_ERROR_COOPERATIVE_YIELD) return HEVC_PROGRESS;
        if (!more || err==DE265_ERROR_WAITING_FOR_INPUT_DATA) {
            d->pending=false;
            return d->flushing ? HEVC_END : HEVC_NEED_INPUT;
        }
        if (!d->ctx->ctu_budget) return HEVC_PROGRESS;
    }
    return HEVC_PROGRESS;
}

extern "C" hevc_status_t hevc_step_until(hevc_decoder_t *d, unsigned budget,
                                         uint64_t deadline_ticks, hevc_clock_fn clock)
{
    if (!d) return HEVC_ERROR;
    d->ctx->ctu_clock=clock;
    d->ctx->ctu_deadline_ticks=deadline_ticks;
    hevc_status_t status=hevc_step_core(d,budget);
    d->ctx->ctu_clock=NULL;
    d->ctx->ctu_deadline_ticks=0;
    return status;
}

extern "C" hevc_status_t hevc_step(hevc_decoder_t *d, unsigned budget)
{
    return hevc_step_until(d,budget,0,NULL);
}

extern "C" const hevc_frame_t *hevc_get_frame(const hevc_decoder_t *d)
{
    return d && d->held ? &d->frame : NULL;
}
extern "C" void hevc_release_frame(hevc_decoder_t *d)
{
    if (d && d->held) {
        de265_release_next_picture(d->ctx);
        d->held=false;
        /* One submitted AU, zero reorder: output is published only after all
         * of that AU's NALs and end-of-picture processing have completed. */
        d->pending=false;
    }
}
extern "C" hevc_status_t hevc_flush(hevc_decoder_t *d)
{
    if (!d || d->failed) return HEVC_ERROR;
    de265_error err=de265_flush_data(d->ctx);
    if (err!=DE265_OK) return fail(d,de265_get_error_text(err));
    d->flushing=true;
    return d->held ? HEVC_FRAME_READY : HEVC_PROGRESS;
}
extern "C" const char *hevc_error_string(const hevc_decoder_t *d) { return d ? d->error : "decoder allocation failed"; }
extern "C" unsigned hevc_last_step_ctus(const hevc_decoder_t *d) { return d ? d->last_ctus : 0; }
extern "C" uint64_t hevc_total_ctus(const hevc_decoder_t *d) { return d ? d->total_ctus : 0; }
extern "C" unsigned hevc_picture_ctus_done(const hevc_decoder_t *d) { return d ? d->ctx->picture_ctus_done : 0; }
extern "C" unsigned hevc_picture_ctus_total(const hevc_decoder_t *d) { return d ? d->ctx->picture_ctus_total : 0; }
extern "C" unsigned hevc_ctu_size(const hevc_decoder_t *d) { return d ? d->ctx->picture_ctu_size : 0; }
