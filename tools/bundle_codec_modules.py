#!/usr/bin/env python3
"""Append on-demand decoder images without enlarging the host Zehn image."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct

CODECS = {"h264": 0, "mpeg4": 1, "hevc": 2, "av1": 3}
MAGIC = b"NDVCMOD1"
MAX_MODULE_BYTES = 16 * 1024 * 1024
HEADER = struct.Struct("<8I")
ENTRY = struct.Struct("<8I32s")
FOOTER = struct.Struct("<8s6I32s")


def module_image_size(payload: bytes) -> int:
    if len(payload) < HEADER.size or len(payload) > MAX_MODULE_BYTES:
        raise ValueError("Invalid module payload size")
    signature, version, file_bytes, relocations, flags, extra, alloc, entry = HEADER.unpack_from(payload)
    prefix = HEADER.size + 4 * (relocations + flags) + extra
    if signature != 0x6E68655A or version != 1 or file_bytes != len(payload):
        raise ValueError("Invalid module Zehn header")
    if prefix > file_bytes or alloc < file_bytes or alloc - prefix > MAX_MODULE_BYTES:
        raise ValueError("Invalid module image bounds")
    if file_bytes - prefix < 4 or entry & 3 or entry > file_bytes - prefix - 4:
        raise ValueError("Invalid module entry point")
    for offset in range(HEADER.size, HEADER.size + relocations * 4, 4):
        kind = payload[offset]
        if kind not in (0, 1, 2, 4):
            raise ValueError("Module must use uncompressed, supported Zehn relocations")
    return alloc - prefix


def bundle(host: bytes, modules: dict[str, bytes]) -> bytes:
    if not modules or set(modules) - CODECS.keys():
        raise ValueError("Select one or more known codecs")
    if len(host) >= FOOTER.size and host[-FOOTER.size:-FOOTER.size + 8] == MAGIC:
        raise ValueError("Host already has a module trailer")
    # The native loader scans only the first 20 KiB for its host Zehn header.
    signature = struct.pack("<I", 0x6E68655A)
    position = host.find(signature, 0, 20 * 1024)
    while position >= 0:
        if position + HEADER.size <= len(host):
            header = HEADER.unpack_from(host, position)
            if header[1] == 1 and header[2] == len(host) - position:
                break
        position = host.find(signature, position + 1, 20 * 1024)
    if position < 0:
        raise ValueError("Host must contain exactly its original Zehn payload")
    result = bytearray(host)
    entries = bytearray()
    for name in CODECS:
        if name not in modules:
            continue
        payload = modules[name]
        image_bytes = module_image_size(payload)
        result.extend(b"\0" * (-len(result) % 4))
        offset = len(result)
        result.extend(payload)
        entries.extend(ENTRY.pack(CODECS[name], 1, offset, len(payload), image_bytes,
                                  0, 0, 0, hashlib.sha256(payload).digest()))
    result.extend(b"\0" * (-len(result) % 4))
    index_offset = len(result)
    total = index_offset + len(entries) + FOOTER.size
    if total > 0x7FFFFFFF:
        raise ValueError("Bundle exceeds the calculator's seek range")
    result.extend(entries)
    result.extend(FOOTER.pack(MAGIC, 1, len(modules), index_offset, len(entries),
                              total, 0, hashlib.sha256(entries).digest()))
    assert result[:len(host)] == host
    return bytes(result)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--host", type=Path)
    operation.add_argument("--manifest", type=Path, help="Write the host's expected module hashes")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--module", action="append", default=[], metavar="CODEC=FILE")
    args = parser.parse_args()
    try:
        modules = {}
        for item in args.module:
            name, separator, path = item.partition("=")
            if not separator or name not in CODECS or name in modules:
                raise ValueError(f"Invalid or duplicate module argument: {item}")
            modules[name] = Path(path).read_bytes()
            module_image_size(modules[name])
        if not modules:
            raise ValueError("At least one module is required")
        if args.manifest:
            rows = []
            for name in CODECS:
                value = hashlib.sha256(modules[name]).digest() if name in modules else bytes(32)
                rows.append("    {" + ",".join(f"0x{byte:02x}" for byte in value) + "}")
            mask = sum(1 << CODECS[name] for name in modules)
            text = ("/* Generated from decoder payloads; do not edit. */\n"
                    "#ifndef NDVIDEO_GENERATED_CODEC_MODULE_MANIFEST_H\n"
                    "#define NDVIDEO_GENERATED_CODEC_MODULE_MANIFEST_H\n#include <stdint.h>\n"
                    f"#define CODEC_MODULE_EXPECTED_MASK {mask}U\n"
                    "static const uint8_t CODEC_MODULE_EXPECTED_HASHES[4][32] = {\n" +
                    ",\n".join(rows) + "\n};\n#endif\n")
            args.manifest.parent.mkdir(parents=True, exist_ok=True)
            if not args.manifest.exists() or args.manifest.read_text() != text:
                temporary = args.manifest.with_suffix(".tmp")
                temporary.write_text(text)
                temporary.replace(args.manifest)
            return
        if not args.output:
            raise ValueError("--output is required with --host")
        output = bundle(args.host.read_bytes(), modules)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(output)
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    print(f"Bundled {len(modules)} codec modules: {len(output)} bytes")


if __name__ == "__main__":
    main()
