"""AV1 packets used by the NVP encoder: one visible frame per IVF record."""
from dataclasses import dataclass
import struct


@dataclass(slots=True)
class Av1AccessUnit:
    data: bytes
    keyframe: bool
    timestamp: int
    codec: str = "av1"

    def bytes(self) -> bytes:
        return self.data


def obu_units(data: bytes) -> list[tuple[int, bytes, bytes]]:
    units = []
    pos = 0
    while pos < len(data):
        start = pos
        header = data[pos]
        pos += 1
        if header & 0x81:
            raise RuntimeError("Invalid AV1 OBU header.")
        kind = (header >> 3) & 15
        if header & 4:
            if pos == len(data) or data[pos] != 0:
                raise RuntimeError("AV1 requires a single temporal and spatial layer.")
            pos += 1
        size = len(data) - pos
        if header & 2:
            size = 0
            for i in range(8):
                if pos == len(data):
                    raise RuntimeError("Truncated AV1 OBU size.")
                value = data[pos]
                pos += 1
                size |= (value & 127) << (i * 7)
                if not value & 128:
                    break
            else:
                raise RuntimeError("Oversized AV1 OBU length.")
        if size > len(data) - pos:
            raise RuntimeError("Truncated AV1 OBU payload.")
        end = pos + size
        units.append((kind, data[start:end], data[pos:end]))
        if len(units) > 64:
            raise RuntimeError("Too many AV1 OBUs in one frame.")
        pos = end
    return units


def av1_bitstream_access_units(bitstream: bytes) -> list[Av1AccessUnit]:
    if len(bitstream) < 32 or bitstream[:4] != b"DKIF" or bitstream[8:12] != b"AV01":
        raise RuntimeError("AV1 encoding must produce an IVF stream.")
    version, header_size = struct.unpack_from("<HH", bitstream, 4)
    if version != 0 or header_size != 32:
        raise RuntimeError("Unsupported AV1 IVF header.")
    declared, = struct.unpack_from("<I", bitstream, 24)
    sequence = b""
    reduced = False
    units = []
    pos = 32
    while pos < len(bitstream):
        if pos + 12 > len(bitstream):
            raise RuntimeError("Truncated AV1 IVF record.")
        size, timestamp = struct.unpack_from("<IQ", bitstream, pos)
        pos += 12
        if not size or size > 262144 or size > len(bitstream) - pos:
            raise RuntimeError("Invalid AV1 IVF frame size.")
        packet = obu_units(bitstream[pos:pos + size])
        pos += size
        keyframe = False
        frames = 0
        has_sequence = False
        for kind, raw, payload in packet:
            if kind == 1:
                if not payload or payload[0] >> 5 or frames:
                    raise RuntimeError("Invalid or unsupported AV1 sequence header.")
                sequence = raw
                reduced = bool(payload[0] & 8)
                has_sequence = True
            elif kind in (3, 6):
                frames += 1
                if not payload or frames != 1 or not sequence:
                    raise RuntimeError("AV1 needs one complete frame with a known sequence header.")
                existing = not reduced and bool(payload[0] & 128)
                if not reduced and not existing and not payload[0] & 16:
                    raise RuntimeError("Hidden AV1 frames are unsupported; disable alt-ref and keyframe filtering.")
                keyframe = reduced or (not existing and not payload[0] & 96)
            elif kind not in (2, 4, 5, 15):
                raise RuntimeError(f"Unsupported AV1 OBU type {kind}.")
        if frames != 1:
            raise RuntimeError("An AV1 packet did not contain a visible frame.")
        if keyframe and not has_sequence:
            # Standalone chunks need the sequence header. Leave a temporal
            # delimiter first when one is present in the encoded packet.
            insert = 1 if packet and packet[0][0] == 2 else 0
            packet.insert(insert, (1, sequence, b""))
        units.append(Av1AccessUnit(b"".join(raw for _, raw, _ in packet), keyframe, timestamp))
    if not units or not units[0].keyframe or declared not in (0, len(units)):
        raise RuntimeError("AV1 frame count or initial keyframe is invalid.")
    return units


def build_av1_ivf(units: list[Av1AccessUnit], template: bytes) -> bytes:
    header = bytearray(template[:32])
    if len(header) != 32:
        raise RuntimeError("Missing AV1 IVF header.")
    struct.pack_into("<I", header, 24, len(units))
    return bytes(header) + b"".join(struct.pack("<IQ", len(unit.data), unit.timestamp) + unit.data
                                  for unit in units)


def av1_encoder_options(idr_frames: int, stream_profile: str, preset: str) -> list[str]:
    speeds = {"ultrafast": 8, "superfast": 7, "veryfast": 6, "faster": 5, "fast": 4,
              "medium": 3, "slow": 2, "slower": 1, "veryslow": 1, "placebo": 0}
    if preset not in speeds:
        raise RuntimeError(f"Unknown AV1 encoder preset: {preset}")
    keyint = 1 if stream_profile == "intra" else idr_frames
    options = ["-c:v", "libaom-av1", "-pix_fmt", "yuv420p", "-cpu-used", str(speeds[preset]),
               "-g", str(keyint), "-keyint_min", str(keyint), "-lag-in-frames", "16", "-auto-alt-ref", "0",
               "-tiles", "1x1", "-row-mt", "0", "-reduced-tx-type-set", "1",
               "-enable-reduced-reference-set", "1"]
    for feature in ("cdef", "restoration", "global-motion", "ref-frame-mvs", "obmc", "dual-filter",
                    "masked-comp", "interintra-comp", "filter-intra", "intrabc", "tx64"):
        options += ["-enable-" + feature, "0"]
    # Encoder lookahead improves rate allocation without delaying playback:
    # alt-ref and keyframe filtering stay disabled, so every packet is visible.
    options += ["-aom-params", "sb-size=64:enable-warped-motion=0:loopfilter-control=0:enable-keyframe-filtering=0"]
    if stream_profile == "fast":
        options += ["-enable-rect-partitions", "0", "-enable-1to4-partitions", "0",
                    "-enable-ab-partitions", "0", "-enable-angle-delta", "0"]
    return options
