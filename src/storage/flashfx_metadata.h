#ifndef NDVIDEO_FLASHFX_METADATA_H
#define NDVIDEO_FLASHFX_METADATA_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Datalight's four-byte protected tag; the validated profile supplies its
 * physical spare offset (+4 on CX II, +12 on the studied CX NTM). */
bool ffx_decode_tag(const uint8_t encoded[4], uint16_t *tag);
/* Full 2048+64 raw header page: protected tag, magic and additive checksum. */
bool ffx_decode_unit_header(const uint8_t *page, size_t bytes, uint32_t *sequence);
bool ffx_decode_unit_header_layout(const uint8_t *page, size_t bytes, uint32_t *sequence,
                                   uint32_t tag_column);
#endif
