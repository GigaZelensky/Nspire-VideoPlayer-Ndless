#include "flashfx_metadata.h"
#include <string.h>
bool ffx_decode_tag(const uint8_t raw[4], uint16_t *tag)
{
    if (!raw || !tag)
        return false;
    if (raw[0] == 255 && raw[1] == 255 && raw[2] == 255 && raw[3] == 255) {
        *tag = 0xffff;
        return true;
    }
    uint8_t b[3] = {raw[0], raw[1], raw[2]};
    unsigned syndrome = raw[3], position = 1;
    for (unsigned byte = 0; byte < 3; ++byte)
        for (unsigned bit = 1; bit < 256; bit <<= 1) {
            while ((position & (position - 1U)) == 0)
                ++position;
            if (b[byte] & bit)
                syndrome ^= position;
            ++position;
        }
    if (syndrome && (syndrome & (syndrome - 1U))) {
        unsigned parity = 2;
        for (unsigned p = 4; p < syndrome; p <<= 1)
            ++parity;
        unsigned bit = syndrome - parity - 1U;
        if (bit < 24U)
            b[bit >> 3] ^= (uint8_t)(1U << (bit & 7U));
    }
    if (b[2] != (uint8_t)(b[0] ^ ~b[1]))
        return false;
    *tag = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return true;
}
bool ffx_decode_unit_header(const uint8_t *page, size_t bytes, uint32_t *sequence)
{
    return ffx_decode_unit_header_layout(page, bytes, sequence, 2052U);
}
bool ffx_decode_unit_header_layout(const uint8_t *page, size_t bytes, uint32_t *sequence,
                                   uint32_t tag_column)
{
    static const uint8_t magic[16] = {0xcc, 0xdd, 0x44, 0x4c, 0x5f, 0x46, 0x53, 0x34,
                                      0x2e, 0x30, 0x30, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint16_t tag;
    if ((tag_column != 2052U && tag_column != 2060U) || !page || bytes != 2112U || !sequence ||
        !ffx_decode_tag(page + tag_column, &tag) || tag != 0x48e2U || memcmp(page, magic, 16))
        return false;
    unsigned sum = 0;
    for (unsigned i = 0; i < 54U; ++i)
        sum += page[i];
    if ((sum & 0xffffU) != ((unsigned)page[54] | ((unsigned)page[55] << 8)))
        return false;
    *sequence = (uint32_t)page[28] | ((uint32_t)page[29] << 8) | ((uint32_t)page[30] << 16) |
                ((uint32_t)page[31] << 24);
    return true;
}
