#include "nve_crypto.h"
#include <limits.h>
#include <string.h>

static const char header_domain[] = "NVE1-header";
static const char block_domain[] = "NVE1-block";
static const char encryption_domain[] = "NVE1-encryption";
static const char authentication_domain[] = "NVE1-authentication";

void nve_wipe(void *data, size_t bytes)
{
    typedef uint32_t WipeWord __attribute__((__may_alias__));
    uint8_t *p = data;
    while (bytes && ((uintptr_t)p & 3U)) { *(volatile uint8_t *)p++ = 0; --bytes; }
    while (bytes >= 4U) { *(volatile WipeWord *)(void *)p = 0; p += 4; bytes -= 4; }
    while (bytes--) *(volatile uint8_t *)p++ = 0;
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t x)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(x >> (i * 8));
}
static bool equal_tag(const uint8_t *a, const uint8_t *b)
{
    uint32_t difference = 0;
    for (unsigned i = 0; i < NVE_TAG_BYTES; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}
static void hmac(const br_hmac_key_context *key, const void *data, size_t bytes, uint8_t out[32])
{
    br_hmac_context h;
    br_hmac_init(&h, key, 32);
    br_hmac_update(&h, data, bytes);
    br_hmac_out(&h, out);
    nve_wipe(&h, sizeof(h));
}
bool nve_header_parse(NveHeader *header, const void *data, uint32_t physical_bytes)
{
    if (!header || !data) return false;
    const uint8_t *p = data;
    if (memcmp(p, "NVE1", 4) || le32(p + 4) != 1U || le32(p + 8) != NVE_RECORD_BYTES ||
        le32(p + 12) != NVE_KDF_ROUNDS || le32(p + 20) || !le32(p + 28) ||
        !le32(p + 60) || le32(p + 60) > 65535U || !le32(p + 64) || le32(p + 64) > 65535U) return false;
    for (unsigned i = 68; i < NVE_HEADER_AUTH_BYTES; ++i) if (p[i]) return false;
    uint32_t plain = le32(p + 16);
    if (plain < 48U || plain > INT32_MAX) return false;
    uint64_t size = NVE_HEADER_BYTES + (uint64_t)plain +
        (((uint64_t)plain + NVE_RECORD_BYTES - 1U) / NVE_RECORD_BYTES) * NVE_TAG_BYTES;
    if (size > INT32_MAX || size != physical_bytes) return false;
    memcpy(header->bytes, data, NVE_HEADER_BYTES);
    header->plain_bytes = plain;
    header->physical_bytes = physical_bytes;
    header->duration_ms = le32(p + 24);
    header->frame_count = le32(p + 28);
    header->fps_num = le32(p + 60);
    header->fps_den = le32(p + 64);
    return true;
}
bool nve_kdf_begin(NveKdf *kdf, const NveHeader *header, const void *password, size_t bytes)
{
    if (!kdf || !header || !password || !bytes || bytes > NVE_PASSWORD_MAX) return false;
    nve_wipe(kdf, sizeof(*kdf));
    kdf->header = *header;
    br_hmac_key_init(&kdf->password, &br_sha256_vtable, password, bytes);
    uint8_t salt[20];
    memcpy(salt, header->bytes + 32, 16);
    salt[16] = salt[17] = salt[18] = 0; salt[19] = 1;
    hmac(&kdf->password, salt, sizeof(salt), kdf->u);
    memcpy(kdf->master, kdf->u, 32);
    kdf->rounds = 1;
    kdf->active = true;
    nve_wipe(salt, sizeof(salt));
    return true;
}
bool nve_kdf_step(NveKdf *kdf, unsigned rounds)
{
    if (!kdf || !kdf->active) return false;
    while (rounds-- && kdf->rounds < NVE_KDF_ROUNDS) {
        hmac(&kdf->password, kdf->u, 32, kdf->u);
        for (unsigned i = 0; i < 32; ++i) kdf->master[i] ^= kdf->u[i];
        ++kdf->rounds;
    }
    return kdf->rounds == NVE_KDF_ROUNDS;
}
bool nve_kdf_finish(NveKdf *kdf, NveKeys *keys)
{
    if (!kdf || !keys || !kdf->active || kdf->rounds != NVE_KDF_ROUNDS) return false;
    br_hmac_key_context master;
    br_hmac_context tag;
    uint8_t encryption[32], authentication[32], check[32];
    nve_wipe(keys, sizeof(*keys));
    br_hmac_key_init(&master, &br_sha256_vtable, kdf->master, 32);
    hmac(&master, encryption_domain, sizeof(encryption_domain), encryption);
    hmac(&master, authentication_domain, sizeof(authentication_domain), authentication);
    br_hmac_key_init(&keys->mac, &br_sha256_vtable, authentication, 32);
    br_hmac_init(&tag, &keys->mac, 32);
    br_hmac_update(&tag, header_domain, sizeof(header_domain));
    br_hmac_update(&tag, kdf->header.bytes, NVE_HEADER_AUTH_BYTES);
    br_hmac_out(&tag, check);
    bool valid = equal_tag(check, kdf->header.bytes + NVE_HEADER_AUTH_BYTES);
    if (valid) {
        keys->header = kdf->header;
        br_aes_ct_ctr_init(&keys->aes, encryption, sizeof(encryption));
        keys->valid = true;
    } else nve_wipe(keys, sizeof(*keys));
    nve_wipe(encryption, sizeof(encryption)); nve_wipe(authentication, sizeof(authentication));
    nve_wipe(check, sizeof(check)); nve_wipe(&master, sizeof(master)); nve_wipe(&tag, sizeof(tag));
    nve_wipe(kdf, sizeof(*kdf));
    return valid;
}
void nve_reader_init(NveReader *reader, const NveKeys *keys)
{
    memset(reader, 0, sizeof(*reader));
    reader->keys = keys;
    reader->cached_unit = UINT32_MAX;
}
static void select_unit(NveReader *r)
{
    r->unit = (r->offset + r->copied) / NVE_RECORD_BYTES;
    r->unit_bytes = r->keys->header.plain_bytes - r->unit * NVE_RECORD_BYTES;
    if (r->unit_bytes > NVE_RECORD_BYTES) r->unit_bytes = NVE_RECORD_BYTES;
    r->progress = 0;
    r->phase = r->cached_unit == r->unit ? NVE_READ_COPY : NVE_READ_FETCH;
}
bool nve_reader_begin(NveReader *r, uint32_t offset, void *destination, uint32_t bytes)
{
    if (!r || !r->keys || !r->keys->valid || !destination || !bytes ||
        offset > r->keys->header.plain_bytes || bytes > r->keys->header.plain_bytes - offset ||
        (uintptr_t)destination > UINTPTR_MAX - bytes)
        return false;
    if (r->phase != NVE_READ_DONE && r->phase != NVE_READ_IDLE) nve_reader_cancel(r);
    r->offset = offset; r->bytes = bytes; r->copied = 0; r->destination = destination;
    select_unit(r);
    return true;
}
uint32_t nve_reader_physical_offset(const NveReader *r)
{
    return NVE_HEADER_BYTES + r->unit * (NVE_RECORD_BYTES + NVE_TAG_BYTES);
}
void nve_reader_supplied(NveReader *r, bool success)
{
    if (!r || r->phase != NVE_READ_FETCH) return;
    r->cached_unit = UINT32_MAX;
    if (!success) { nve_reader_cancel(r); r->phase = NVE_READ_ERROR; return; }
    uint8_t index[8];
    put32(index, r->unit); put32(index + 4, r->unit_bytes);
    br_hmac_init(&r->mac, &r->keys->mac, 32);
    br_hmac_update(&r->mac, block_domain, sizeof(block_domain));
    br_hmac_update(&r->mac, r->keys->header.bytes + NVE_HEADER_AUTH_BYTES, NVE_TAG_BYTES);
    br_hmac_update(&r->mac, index, sizeof(index));
    r->progress = 0; r->phase = NVE_READ_VERIFY;
}
void nve_reader_step(NveReader *r)
{
    if (!r) return;
    if (!r->keys || !r->keys->valid) {
        nve_reader_cancel(r); r->phase = NVE_READ_ERROR; return;
    }
    if (r->phase == NVE_READ_VERIFY) {
        uint32_t count = r->unit_bytes - r->progress;
        if (count > 512U) count = 512U;
        br_hmac_update(&r->mac, r->block + r->progress, count);
        r->progress += count;
        if (r->progress == r->unit_bytes) {
            uint8_t tag[32];
            br_hmac_out(&r->mac, tag);
            bool valid = equal_tag(tag, r->block + r->unit_bytes);
            nve_wipe(tag, sizeof(tag)); nve_wipe(&r->mac, sizeof(r->mac));
            if (!valid) { nve_reader_cancel(r); r->phase = NVE_READ_ERROR; return; }
            r->progress = 0; r->phase = NVE_READ_DECRYPT;
        }
    } else if (r->phase == NVE_READ_DECRYPT) {
        uint32_t count = r->unit_bytes - r->progress;
        if (count > 64U) count = 64U;
        br_aes_ct_ctr_run(&r->keys->aes, r->keys->header.bytes + 48,
            (r->unit * NVE_RECORD_BYTES + r->progress) / 16U, r->block + r->progress, count);
        r->progress += count;
        if (r->progress == r->unit_bytes) { r->cached_unit = r->unit; r->phase = NVE_READ_COPY; }
    } else if (r->phase == NVE_READ_COPY) {
        uint32_t within = (r->offset + r->copied) % NVE_RECORD_BYTES;
        uint32_t count = r->bytes - r->copied;
        if (count > r->unit_bytes - within) count = r->unit_bytes - within;
        if (count > 512U) count = 512U;
        memcpy(r->destination + r->copied, r->block + within, count);
        r->copied += count;
        if (r->copied == r->bytes) r->phase = NVE_READ_DONE;
        else if (within + count == r->unit_bytes) select_unit(r);
    }
}
void nve_reader_cancel(NveReader *r)
{
    if (!r) return;
    nve_wipe(r->block, sizeof(r->block)); nve_wipe(&r->mac, sizeof(r->mac));
    r->cached_unit = UINT32_MAX; r->phase = NVE_READ_IDLE; r->destination = NULL;
}
