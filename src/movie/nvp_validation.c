#include "nvp_validation.h"

#include <stddef.h>
#include <string.h>

bool nvp_header_is_valid(const MovieHeader *header, uint32_t file_size)
{
    uint32_t index_bytes;
    uint32_t subtitle_bytes;
    uint32_t cue_meta_size;

    /* Ndless seeks use signed 32-bit offsets; playback timestamps are uint32_t ms. */
    if (!header || file_size < sizeof(*header) || file_size > INT32_MAX ||
        memcmp(header->magic, "NVP1", 4) != 0 ||
        movie_codec_from_header(header) == MOVIE_CODEC_UNKNOWN || header->canvas_width == 0 ||
        header->canvas_height == 0 || header->video_width == 0 || header->video_height == 0 ||
        header->video_width > UINT16_MAX / 2U ||
        (uint32_t)header->video_x + header->video_width > header->canvas_width ||
        (uint32_t)header->video_y + header->video_height > header->canvas_height ||
        (uint64_t)header->video_width * header->video_height * sizeof(uint16_t) > UINT32_MAX ||
        header->fps_num == 0 || header->fps_den == 0 || header->frame_count == 0 ||
        (uint64_t)header->frame_count * 1000U * header->fps_den / header->fps_num > UINT32_MAX ||
        header->chunk_count == 0 || header->chunk_count > header->frame_count ||
        header->chunk_count > INT32_MAX || header->index_offset < sizeof(*header) ||
        header->index_offset > file_size ||
        header->chunk_count > (file_size - header->index_offset) / sizeof(ChunkIndexEntry)) {
        return false;
    }
    index_bytes = header->chunk_count * (uint32_t)sizeof(ChunkIndexEntry);
    if (header->subtitle_count == 0 && header->subtitle_offset == 0) {
        return true;
    }
    if (header->subtitle_offset < header->index_offset + index_bytes ||
        header->subtitle_offset > file_size) {
        return false;
    }
    if (header->subtitle_count == 0) {
        return true;
    }
    subtitle_bytes = file_size - header->subtitle_offset;
    cue_meta_size = header->version >= MOVIE_VERSION_POSITIONED_SUBS ? 22U : 10U;
    /* At least one track (count + six-byte descriptor), followed by all cue metadata. */
    return subtitle_bytes >= 8U && header->subtitle_count <= (subtitle_bytes - 8U) / cue_meta_size;
}

bool nvp_index_is_valid(const MovieHeader *header, const ChunkIndexEntry *index)
{
    uint32_t chunk_index;
    uint32_t frame_cursor = 0;

    if (!header || !index || header->chunk_count == 0 || header->frame_count == 0) {
        return false;
    }
    for (chunk_index = 0; chunk_index < header->chunk_count; ++chunk_index) {
        const ChunkIndexEntry *entry = index + chunk_index;
        uint32_t table_space;

        if (entry->first_frame != frame_cursor || entry->frame_count == 0 ||
            entry->frame_count > header->frame_count - frame_cursor ||
            entry->offset < sizeof(*header) || entry->offset > header->index_offset ||
            entry->packed_size > header->index_offset - entry->offset ||
            entry->packed_size != entry->unpacked_size ||
            entry->frame_table_offset > entry->packed_size) {
            return false;
        }
        table_space = entry->packed_size - entry->frame_table_offset;
        /* Payload-size word, frame offsets, and at least one payload byte. */
        if (table_space <= 4U || entry->frame_count > (table_space - 5U) / sizeof(uint32_t)) {
            return false;
        }
        frame_cursor += entry->frame_count;
    }
    return frame_cursor == header->frame_count;
}
