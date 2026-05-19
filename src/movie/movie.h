#ifndef NDVIDEO_MOVIE_H
#define NDVIDEO_MOVIE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <SDL/SDL.h>

#include "codecs/codec.h"
#include "codecs/h264bsd/h264bsd_decoder.h"
#include "movie/nvp_format.h"

#define PREFETCH_CHUNK_COUNT 5
#define UI_BUFFER_CHUNK_CACHE_COUNT (PREFETCH_CHUNK_COUNT + 2)

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
} PrefetchState;

typedef struct {
    uint8_t *chunk_storage;
    size_t chunk_storage_size;
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
    bool chunk_dirty;
    uint16_t foreground_decode_avg_ms;
    uint16_t foreground_decode_peak_ms;
} H264DecoderContext;

typedef struct {
    void *decoder;
    bool chunk_dirty;
    bool discontinuity;
} Mpeg4DecoderContext;

typedef struct Movie {
    FILE *file;
    long current_file_pos;
    MovieHeader header;
    MovieCodec codec;
    const MovieCodecOps *codec_ops;
    ChunkIndexEntry *chunk_index;
    SubtitleCue *subtitles;
    SubtitleTrack *subtitle_tracks;
    uint16_t subtitle_track_count;
    uint16_t selected_subtitle_track;
    uint16_t *framebuffer;
    uint8_t *chunk_storage;
    size_t chunk_storage_size;
    bool chunk_storage_in_sram;
    uint8_t *chunk_bytes;
    uint32_t *frame_offsets;
    size_t chunk_size;
    int loaded_chunk;
    PrefetchedChunk prefetched[PREFETCH_CHUNK_COUNT];
    int ui_buffer_chunks[UI_BUFFER_CHUNK_CACHE_COUNT];
    size_t ui_buffer_chunk_count;
    int decoded_local_frame;
    uint32_t current_frame;
    SDL_Surface *frame_surface;
    H264DecoderContext h264;
    Mpeg4DecoderContext mpeg4;
    uint32_t last_read_bytes;
    uint32_t last_read_time_ms;
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
} Movie;

#endif
