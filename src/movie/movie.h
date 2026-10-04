#ifndef NDVIDEO_MOVIE_H
#define NDVIDEO_MOVIE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <SDL/SDL.h>

#include "codecs/codec.h"
#include "codecs/hevc_decoder.h"
#include "codecs/av1_decoder.h"
#include "codecs/h264bsd/h264bsd_decoder.h"
#include "movie/nvp_format.h"
#include "player/playback_cadence.h"

/* Short, high-motion chunks can exhaust five slots in well under a second.
 * Allocations remain lazy and subject to the existing total-byte budget. */
#define PREFETCH_CHUNK_COUNT 16

typedef struct {
    uint32_t start_ms;
    uint32_t end_ms;
    char *text;
    uint8_t position_mode;
    uint8_t align;
    uint16_t pos_x;
    uint16_t pos_y;
    uint16_t margin_l;
    uint16_t margin_r;
    uint16_t margin_v;
} SubtitleCue;

typedef struct {
    char *name;
    uint32_t cue_start;
    uint32_t cue_count;
    uint8_t supports_positioning;
} SubtitleTrack;

typedef enum {
    PREFETCH_IDLE = 0,
    PREFETCH_READING,
    PREFETCH_READY,
    PREFETCH_FAILED,
} PrefetchState;

typedef struct {
    uint8_t *chunk_storage;
    uint8_t *chunk_allocation;
    size_t chunk_storage_size;
    size_t chunk_capacity;
    int chunk_index;
    PrefetchState state;
    size_t read_offset;
} PrefetchedChunk;

typedef struct {
    storage_t *decoder;
    uint32_t full_width;
    uint32_t full_height;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_width;
    uint32_t crop_height;
    bool headers_ready;
    bool decoder_initialized;
    bool decoder_failed;
    bool chunk_dirty;
} H264DecoderContext;

typedef struct {
    void *decoder;
    bool chunk_dirty;
    bool discontinuity;
} Mpeg4DecoderContext;

typedef struct {
    hevc_decoder_t *decoder;
    const hevc_frame_t *picture;
    const uint8_t *access_unit;
    size_t access_unit_size;
    bool decoder_failed;
    uint64_t submitted_frames, decoded_ctus;
} HevcDecoderContext;

typedef struct {
    av1_decoder_t *decoder;
    const av1_frame_t *picture;
    const uint8_t *access_unit;
    size_t access_unit_size;
    bool decoder_failed;
    uint64_t submitted_frames, decoded_blocks;
} Av1DecoderContext;

typedef struct Movie {
    FILE *file;
    struct MovieAsyncIo *async_io;
    struct VideoLookahead *video_lookahead;
    bool lookahead_enabled;
    bool diag_async_used;
    uint32_t diag_async_reads, diag_async_bytes, diag_async_waits;
    uint32_t diag_async_cancels, diag_async_failures, diag_async_max_ticks;
    int32_t diag_async_native_error;
    long current_file_pos;
    MovieHeader header;
    bool encrypted;
    /* Reduced once on load; keep the original serialized header for metadata. */
    uint16_t timing_fps_num;
    uint16_t timing_fps_den;
    MovieCodec codec;
    const MovieCodecOps *codec_ops;
    ChunkIndexEntry *chunk_index;
    SubtitleCue *subtitles;
    SubtitleTrack *subtitle_tracks;
    uint8_t *subtitle_storage;
    size_t subtitle_storage_size;
    uint16_t subtitle_track_count;
    uint16_t selected_subtitle_track;
    const SubtitleCue *subtitle_lookup_cue;
    uint32_t subtitle_lookup_from_ms;
    uint32_t subtitle_lookup_until_ms;
    uint16_t subtitle_lookup_track;
    bool subtitle_lookup_valid;
    uint16_t *framebuffer;
    uint8_t *framebuffer_allocation;
    uint8_t *chunk_storage;
    uint8_t *chunk_storage_allocation;
    size_t chunk_storage_size;
    size_t chunk_storage_capacity;
    bool chunk_storage_in_sram;
    uint8_t *chunk_bytes;
    uint32_t *frame_offsets;
    uint32_t *frame_offsets_allocation;
    size_t frame_offsets_capacity;
    size_t chunk_size;
    int loaded_chunk;
    PrefetchedChunk prefetched[PREFETCH_CHUNK_COUNT];
    unsigned prefetch_slots; /* Selected once from the validated movie index. */
    int decoded_local_frame;
    uint32_t current_frame;
    SDL_Surface *frame_surface;
    H264DecoderContext h264;
    Mpeg4DecoderContext mpeg4;
    HevcDecoderContext hevc;
    Av1DecoderContext av1;
    uint16_t foreground_decode_avg_ms, foreground_decode_peak_ms;
    uint64_t foreground_pending_ticks;
    uint32_t last_read_bytes;
    uint32_t last_read_time_ms;
    uint32_t prefetch_read_bytes_per_ms;
    uint32_t prefetch_wait_frame; /* Presentation index + 1; zero before first read turn. */
    uint32_t diag_last_snapshot_ms;
    uint32_t diag_prefetch_tick_count;
    uint32_t diag_active_prefetch_tick_count;
    uint32_t diag_io_priority_count;
    uint32_t diag_foreground_decode_count;
    uint32_t diag_foreground_direct_decode_count;
    uint32_t diag_display_fps_window_start_ms;
    uint16_t diag_display_fps_x10;
    uint16_t diag_display_fps_window_frames;
    uint32_t diag_lag_event_count;
    PlaybackCadence cadence;
    uint32_t diag_lag_frame_total;
    uint32_t diag_max_lag_frames;
    uint32_t diag_max_late_ms;
    uint32_t diag_max_spare_ms;
    uint32_t diag_chunk_load_sync_count;
    uint32_t diag_chunk_load_prefetched_count;
    uint32_t diag_prefetch_read_ops;
    uint32_t diag_prefetch_read_bytes;
    uint32_t diag_h264_replay_count;
    uint32_t diag_h264_replay_frames_total;
    uint32_t diag_h264_replay_max_distance;
    uint32_t diag_last_spare_ms;
    bool debug_idr_cache_valid;
    int debug_idr_cache_chunk;
    uint32_t debug_idr_cache_start_local;
    uint32_t debug_idr_cache_end_local;
    uint32_t diag_render_count;
    uint32_t diag_render_skipped_count;
    uint32_t diag_render_total_ms;
    uint32_t diag_render_max_ms;
} Movie;

#endif
