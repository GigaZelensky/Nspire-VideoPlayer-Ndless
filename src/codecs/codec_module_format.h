#ifndef NDVIDEO_CODEC_MODULE_FORMAT_H
#define NDVIDEO_CODEC_MODULE_FORMAT_H
#include <stdint.h>

/* Byte layout only: all uint32 fields are little endian, never native structs.
 * Each module is an uncompressed standalone Zehn image, fully linked with a
 * custom CodecModuleEntry. Host Zehn sizes EXCLUDE every appended byte.
 * Footer lives at actual EOF in both .tns and bare .zehn bundles. */
#define CODEC_BUNDLE_MAGIC "NDVCMOD1"
#define CODEC_BUNDLE_VERSION 1U
#define CODEC_BUNDLE_FOOTER_BYTES 64U
#define CODEC_BUNDLE_ENTRY_BYTES 64U
#define CODEC_MODULE_MAX_BYTES (16U * 1024U * 1024U)

/* Footer: magic[8], version:u32, count:u32, index_offset:u32,
 * index_bytes:u32, file_bytes:u32, reserved_zero:u32, index_sha256[32].
 * SHA256 covers the exact index bytes. */
/* Entry: codec_id:u32, abi_version:u32, payload_offset:u32,
 * payload_bytes:u32, image_bytes:u32, workspace_bytes:u32,
 * flags_zero:u32, reserved_zero:u32, payload_sha256[32].
 * image_bytes is Zehn alloc_size minus its metadata prefix. SHA256 covers the
 * entire Zehn payload, including header/relocations/flags/extra/image bytes.
 * Payloads must be four-byte aligned, nonoverlapping and end before the index.
 * IDs match MovieCodec: H264=0, MPEG4=1, HEVC=2, AV1=3. */
#endif
