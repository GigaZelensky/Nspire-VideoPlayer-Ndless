"""Authenticated, seekable AES-256 encryption for NVP files (NVE1)."""
from __future__ import annotations

import argparse
import getpass
import hashlib
import hmac
import os
from pathlib import Path
import struct

HEADER_BYTES = 128
RECORD_BYTES = 16384
TAG_BYTES = 32
KDF_ROUNDS = 600000
HEADER_DOMAIN = b"NVE1-header\0"
BLOCK_DOMAIN = b"NVE1-block\0"
ENC_DOMAIN = b"NVE1-encryption\0"
MAC_DOMAIN = b"NVE1-authentication\0"


def read_password(path: Path | None = None) -> bytearray:
    if path is not None:
        password = bytearray(path.read_bytes().rstrip(b"\r\n"))
    else:
        first = getpass.getpass("Video password: ")
        if first != getpass.getpass("Confirm password: "):
            raise ValueError("Passwords do not match.")
        password = bytearray(first.encode("utf-8"))
    if not password or len(password) > 128 or any(c < 32 or c > 126 for c in password):
        password[:] = b"\0" * len(password)
        raise ValueError("Use a non-empty password of at most 128 printable ASCII characters.")
    return password


class EncryptedWriter:
    """Forward NVP packing, with one allowed final header rewrite at offset 0.

    The first record stays in RAM until close, so placeholder NVP headers are
    never encrypted twice with the same CTR counter. No plaintext temporary
    NVP file is created; at most two 16 KiB records are buffered here.
    """
    def __init__(self, path: Path, plain_bytes: int, duration_ms: int, password: bytearray,
                 frame_count: int, fps_num: int, fps_den: int):
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        self.Cipher, self.algorithms, self.modes = Cipher, algorithms, modes
        physical = HEADER_BYTES + plain_bytes + TAG_BYTES * ((plain_bytes + RECORD_BYTES - 1) // RECORD_BYTES)
        if plain_bytes < 48 or physical > 0x7FFFFFFF:
            password[:] = b"\0" * len(password)
            raise ValueError("Encrypted movie exceeds the player's file-size limit.")
        if not 1 <= frame_count <= 0xFFFFFFFF or not 1 <= fps_num <= 65535 or not 1 <= fps_den <= 65535:
            password[:] = b"\0" * len(password)
            raise ValueError("Invalid movie timing.")
        salt, self.nonce = os.urandom(16), os.urandom(12)
        try:
            master = hashlib.pbkdf2_hmac("sha256", password, salt, KDF_ROUNDS, 32)
        finally:
            password[:] = b"\0" * len(password)
        self.enc_key = hmac.digest(master, ENC_DOMAIN, "sha256")
        self.mac_key = hmac.digest(master, MAC_DOMAIN, "sha256")
        del master
        header = struct.pack("<4sIIIQII16s12sII28s", b"NVE1", 1, RECORD_BYTES, KDF_ROUNDS,
                             plain_bytes, min(duration_ms, 0xFFFFFFFF), frame_count, salt, self.nonce,
                             fps_num, fps_den, bytes(28))
        self.header_tag = hmac.digest(self.mac_key, HEADER_DOMAIN + header, "sha256")
        self.file = path.open("wb")
        try:
            self.file.write(header + self.header_tag)
        except BaseException:
            try:
                self.file.close()
            finally:
                self.enc_key = self.mac_key = b""
                path.unlink(missing_ok=True)
            raise
        self.plain_bytes = plain_bytes
        self.position = self.high_water = 0
        self.first = bytearray()
        self.pending = bytearray()
        self.pending_unit = 1
        self.rewriting = False

    def __enter__(self):
        return self

    def tell(self) -> int:
        return self.position

    def _record(self, index: int, plaintext: bytes | bytearray) -> None:
        counter = index * (RECORD_BYTES // 16)
        cipher = self.Cipher(self.algorithms.AES(self.enc_key),
                             self.modes.CTR(self.nonce + struct.pack(">I", counter))).encryptor()
        ciphertext = cipher.update(plaintext) + cipher.finalize()
        tag = hmac.digest(self.mac_key, BLOCK_DOMAIN + self.header_tag +
                          struct.pack("<II", index, len(ciphertext)) + ciphertext, "sha256")
        self.file.seek(HEADER_BYTES + index * (RECORD_BYTES + TAG_BYTES))
        self.file.write(ciphertext + tag)

    def write(self, data) -> int:
        view = memoryview(data)
        size = len(view)
        if self.rewriting:
            if self.position + size > 48:
                raise ValueError("Only the NVP header may be rewritten.")
            self.first[self.position:self.position + size] = view
            self.position += size
            return size
        if self.position + size > self.plain_bytes:
            raise ValueError("Packed NVP is larger than its declared length.")
        while view:
            buffer = self.first if self.position < RECORD_BYTES else self.pending
            amount = min(len(view), RECORD_BYTES - len(buffer))
            buffer.extend(view[:amount])
            view = view[amount:]
            self.position += amount
            if buffer is self.pending and len(buffer) == RECORD_BYTES:
                self._record(self.pending_unit, buffer)
                self.pending_unit += 1
                buffer[:] = b"\0" * len(buffer)
                buffer.clear()
        self.high_water = self.position
        return size

    def seek(self, offset: int, whence: int = 0) -> int:
        if whence or offset or self.high_water != self.plain_bytes or self.rewriting:
            raise ValueError("Encrypted packing only permits the final header rewrite.")
        self.rewriting = True
        self.position = 0
        return 0

    def __exit__(self, kind, value, traceback):
        try:
            if kind is None:
                if self.high_water != self.plain_bytes or self.first[:4] != b"NVP1":
                    raise ValueError("Incomplete NVP packing.")
                if self.pending:
                    self._record(self.pending_unit, self.pending)
                self._record(0, self.first)
                self.file.flush()
        finally:
            try:
                self.file.close()
            finally:
                self.first[:] = b"\0" * len(self.first)
                self.pending[:] = b"\0" * len(self.pending)
                self.enc_key = self.mac_key = b""


def encrypt_existing(source: Path, output: Path, password: bytearray) -> None:
    if source.resolve() == output.resolve() or (output.exists() and source.samefile(output)):
        raise ValueError("Choose a different output file; the original is kept.")
    with source.open("rb") as stream:
        header = stream.read(48)
        if len(header) != 48 or header[:4] != b"NVP1":
            raise ValueError("Input must be an unencrypted NVP movie.")
        values = struct.unpack("<4s12H5I", header)
        if not values[9] or not values[10]:
            raise ValueError("Invalid movie frame rate.")
        duration_ms = values[13] * values[10] * 1000 // values[9]
        stream.seek(0)
        output_started = False
        try:
            with EncryptedWriter(output, source.stat().st_size, duration_ms, password,
                                 values[13], values[9], values[10]) as writer:
                output_started = True
                while data := stream.read(65536):
                    writer.write(data)
        except BaseException:
            if output_started:
                output.unlink(missing_ok=True)
            raise


def main() -> None:
    parser = argparse.ArgumentParser(description="Encrypt an existing NVP without re-encoding its video.")
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--password-file", type=Path, help="Read the password from a file instead of prompting")
    args = parser.parse_args()
    encrypt_existing(args.source, args.output, read_password(args.password_file))
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
