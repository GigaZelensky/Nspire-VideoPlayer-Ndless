#ifndef NDVIDEO_NVE_CRYPTO_H
#define NDVIDEO_NVE_CRYPTO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "crypto/bearssl/bearssl_block.h"
#include "crypto/bearssl/bearssl_hmac.h"

/* NVE1 wraps a complete NVP stream. All integers are little-endian.
 * Header: magic[4], version:u32, record_bytes:u32, iterations:u32,
 * plaintext_bytes:u64, duration_ms:u32, frame_count:u32, salt[16], nonce[12],
 * fps_num:u32, fps_den:u32, reserved[28], header_tag[32]. Reserved bytes are zero.
 * Records: up to 16384 ciphertext bytes followed by a 32-byte HMAC.
 * AES-256-CTR uses nonce[12] || BE32(plaintext_offset / 16).
 * PBKDF2-HMAC-SHA256 derives a 32-byte master; domain-separated HMACs
 * derive independent encryption and authentication keys. Tags bind the
 * authenticated header, record number, length and ciphertext. No plaintext
 * is released until its complete record is authenticated. */
#define NVE_HEADER_BYTES 128U
#define NVE_HEADER_AUTH_BYTES 96U
#define NVE_RECORD_BYTES 16384U
#define NVE_TAG_BYTES 32U
#define NVE_KDF_ROUNDS 600000U
#define NVE_PASSWORD_MAX 128U

typedef struct NveHeader {
    uint8_t bytes[NVE_HEADER_BYTES];
    uint32_t plain_bytes, physical_bytes, duration_ms, frame_count, fps_num, fps_den;
} NveHeader;
typedef struct {
    NveHeader header;
    br_aes_ct_ctr_keys aes;
    br_hmac_key_context mac;
    bool valid;
} NveKeys;
typedef struct {
    NveHeader header;
    br_hmac_key_context password;
    uint8_t u[32], master[32];
    uint32_t rounds;
    bool active;
} NveKdf;

void nve_wipe(void *data, size_t bytes);
bool nve_header_parse(NveHeader *, const void *bytes, uint32_t physical_bytes);
bool nve_kdf_begin(NveKdf *, const NveHeader *, const void *password, size_t bytes);
bool nve_kdf_step(NveKdf *, unsigned rounds);
bool nve_kdf_finish(NveKdf *, NveKeys *);

typedef enum { NVE_READ_IDLE, NVE_READ_FETCH, NVE_READ_VERIFY, NVE_READ_DECRYPT,
               NVE_READ_COPY, NVE_READ_DONE, NVE_READ_ERROR } NveReadPhase;
typedef enum { NVE_ERROR_NONE, NVE_ERROR_KEYS, NVE_ERROR_IO,
               NVE_ERROR_AUTH, NVE_ERROR_STATE } NveReadError;
typedef struct {
    const NveKeys *keys;
    uint8_t block[NVE_RECORD_BYTES + NVE_TAG_BYTES];
    br_hmac_context mac;
    uint8_t *destination;
    uint32_t offset, bytes, copied, unit, unit_bytes, progress, cached_unit;
    NveReadPhase phase;
    NveReadError error;
    NveReadPhase error_phase;
    uint8_t error_cipher_hash[32], error_tag[32];
} NveReader;
void nve_reader_init(NveReader *, const NveKeys *);
bool nve_reader_begin(NveReader *, uint32_t offset, void *destination, uint32_t bytes);
uint32_t nve_reader_physical_offset(const NveReader *);
void nve_reader_supplied(NveReader *, bool success);
void nve_reader_step(NveReader *);
/* Cancel publication and discard any partly read/decrypted record. */
void nve_reader_cancel(NveReader *);
#endif
