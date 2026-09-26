#ifndef NDVIDEO_NVP_VALIDATION_H
#define NDVIDEO_NVP_VALIDATION_H

#include <stdbool.h>
#include "nvp_format.h"

/* Validate on-disk ranges before allocating or decoding their contents. */
bool nvp_header_is_valid(const MovieHeader *header, uint32_t file_size);
bool nvp_index_is_valid(const MovieHeader *header, const ChunkIndexEntry *index);

#endif
