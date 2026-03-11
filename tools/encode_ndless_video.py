#!/usr/bin/env python3
"""Encode a normal video file into a streamed Ndless-native movie container."""

from __future__ import annotations

import argparse
import html
import json
import re
import struct
import subprocess
import sys
import tempfile
import time
import unicodedata
import zlib
from dataclasses import asdict, dataclass
from pathlib import Path

import imageio_ffmpeg


MAGIC = b"NVP1"
VERSION = 9
SCREEN_W = 320
SCREEN_H = 240
HEADER_STRUCT = struct.Struct("<4sHHHHHHHHHHHHIIIII")
CHUNK_INDEX_STRUCT = struct.Struct("<IIIIII")
START_CODE_RE = re.compile(rb"\x00\x00(?:\x00)?\x01")
SUBTITLE_LINE_BREAK_RE = re.compile(r"(?i)<br\s*/?>|\\N|\\n")
SUBTITLE_TAG_RE = re.compile(r"(?s)<[^>]+>")
SUBTITLE_ASS_OVERRIDE_RE = re.compile(r"\{\\[^}]*\}")

NAL_IDR = 5
NAL_SEI = 6
NAL_SPS = 7
NAL_PPS = 8
NAL_AUD = 9
VCL_NAL_TYPES = {1, NAL_IDR}
CHUNK_BOUNDARY_NAL_TYPES = {NAL_SPS, NAL_PPS, NAL_IDR}
STREAM_PROFILES = ("fast", "balanced", "quality", "intra")


@dataclass(slots=True)
class SubtitleCue:
    start_ms: int
    end_ms: int
    text: str


@dataclass(slots=True)
class SubtitleTrack:
    name: str
    cues: list[SubtitleCue]


@dataclass(slots=True)
class EmbeddedSubtitleTrackInfo:
    ordinal: int
    name: str
    codec_name: str
    text_supported: bool


@dataclass(slots=True)
class EncodeStats:
    input_path: str
    output_path: str
    width: int
    height: int
    video_x: int
    video_y: int
    fps: float
    frame_count: int
    chunk_count: int
    idr_chunks: int
    raw_h264_bytes: int
    bytes_written: int
    average_bytes_per_frame: float


@dataclass(slots=True)
class VideoProbe:
    storage_width: int
    storage_height: int
    display_width: int
    display_height: int
    fps: float
    duration: float


@dataclass(slots=True)
class NalUnit:
    nal_type: int
    data: bytes


@dataclass(slots=True)
class AccessUnit:
    nal_units: list[NalUnit]

    def contains_type(self, nal_type: int) -> bool:
        return any(unit.nal_type == nal_type for unit in self.nal_units)

    def bytes(self) -> bytes:
        return b"".join(unit.data for unit in self.nal_units)


def log(message: str, *, quiet: bool = False) -> None:
    if not quiet:
        print(message, flush=True)


def format_duration_hms(seconds: float) -> str:
    total_seconds = max(0, int(round(seconds)))
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    secs = total_seconds % 60
    return f"{hours:02d}:{minutes:02d}:{secs:02d}"


def normalize_output_path(output_arg: str) -> Path:
    path = Path(output_arg).resolve()
    lower_name = path.name.lower()

    if lower_name.endswith(".nvp.tns"):
        return path
    if lower_name.endswith(".nvp"):
        return path.with_name(path.name + ".tns")
    if lower_name.endswith(".tns"):
        return path.with_name(path.name[:-4] + ".nvp.tns")
    return path.with_name(path.name + ".nvp.tns")


def repair_mojibake(text: str) -> str:
    if not any(marker in text for marker in ("Ãƒ", "Ã¢", "â‚¬", "â„¢", "Å“", "Å¾")):
        return text
    try:
        repaired = text.encode("latin-1").decode("utf-8")
    except UnicodeError:
        return text
    return repaired if "\ufffd" not in repaired else text


def sanitize_subtitle_text(text: str) -> str:
    text = html.unescape(text)
    text = repair_mojibake(text)
    text = unicodedata.normalize("NFKD", text)
    text = SUBTITLE_LINE_BREAK_RE.sub("\n", text)
    text = SUBTITLE_ASS_OVERRIDE_RE.sub("", text)
    text = SUBTITLE_TAG_RE.sub("", text)
    text = text.encode("ascii", "ignore").decode("ascii")
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r"[ \t]*\n[ \t]*", "\n", text)
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def parse_srt(path: Path) -> list[SubtitleCue]:
    raw = path.read_text(encoding="utf-8-sig", errors="replace").replace("\r\n", "\n")
    entries = re.split(r"\n\s*\n", raw.strip())
    cues: list[SubtitleCue] = []
    for entry in entries:
        lines = [line.strip() for line in entry.split("\n") if line.strip()]
        if len(lines) < 2:
            continue
        if "-->" in lines[0]:
            time_line = lines[0]
            text_lines = lines[1:]
        else:
            time_line = lines[1]
            text_lines = lines[2:]
        match = re.match(
            r"(\d+):(\d+):(\d+)[,.](\d+)\s*-->\s*(\d+):(\d+):(\d+)[,.](\d+)",
            time_line,
        )
        if not match:
            continue
        start_ms = (
            int(match.group(1)) * 3600
            + int(match.group(2)) * 60
            + int(match.group(3))
        ) * 1000 + int(match.group(4)[:3].ljust(3, "0"))
        end_ms = (
            int(match.group(5)) * 3600
            + int(match.group(6)) * 60
            + int(match.group(7))
        ) * 1000 + int(match.group(8)[:3].ljust(3, "0"))
        text = sanitize_subtitle_text("\n".join(text_lines))
        if text:
            cues.append(SubtitleCue(start_ms=start_ms, end_ms=end_ms, text=text))
    return cues


def ffprobe_path() -> Path | None:
    ffmpeg_path = Path(imageio_ffmpeg.get_ffmpeg_exe())
    candidate_names = ["ffprobe.exe", "ffprobe"]
    for name in candidate_names:
        candidate = ffmpeg_path.with_name(name)
        if candidate.exists():
            return candidate
    return None


def subtitle_track_display_name(ordinal: int, language: str | None, title: str | None) -> str:
    parts: list[str] = []
    if language and language.lower() not in {"und", "unknown"}:
        parts.append(language.upper())
    if title:
        parts.append(title.strip())
    if not parts:
        return f"Subtitle {ordinal + 1}"
    return " - ".join(part for part in parts if part)


def is_text_subtitle_codec(codec_name: str | None) -> bool:
    codec = (codec_name or "").strip().lower()
    if not codec:
        return False
    return codec not in {
        "hdmv_pgs_subtitle",
        "pgssub",
        "dvd_subtitle",
        "dvb_subtitle",
        "xsub",
    }


def probe_embedded_subtitle_tracks(input_path: Path) -> list[EmbeddedSubtitleTrackInfo]:
    ffprobe = ffprobe_path()
    if ffprobe is not None:
        completed = subprocess.run(
            [
                str(ffprobe),
                "-v",
                "error",
                "-select_streams",
                "s",
                "-show_entries",
                "stream=index,codec_name:stream_tags=language,title",
                "-of",
                "json",
                str(input_path),
            ],
            capture_output=True,
            text=True,
        )
        if completed.returncode == 0:
            try:
                payload = json.loads(completed.stdout or "{}")
            except json.JSONDecodeError:
                payload = {}
            streams = payload.get("streams", [])
            tracks: list[EmbeddedSubtitleTrackInfo] = []
            for ordinal, stream in enumerate(streams):
                tags = stream.get("tags", {}) if isinstance(stream, dict) else {}
                codec_name = stream.get("codec_name") if isinstance(stream, dict) else None
                tracks.append(EmbeddedSubtitleTrackInfo(
                    ordinal=ordinal,
                    name=subtitle_track_display_name(
                        ordinal,
                        tags.get("language") if isinstance(tags, dict) else None,
                        tags.get("title") if isinstance(tags, dict) else None,
                    ),
                    codec_name=(codec_name or "unknown"),
                    text_supported=is_text_subtitle_codec(codec_name),
                ))
            if tracks:
                return tracks

    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    completed = subprocess.run(
        [ffmpeg, "-hide_banner", "-i", str(input_path)],
        capture_output=True,
        text=True,
    )
    tracks = []
    subtitle_ordinal = 0
    for line in completed.stderr.splitlines():
        if " Subtitle:" not in line:
            continue
        language_match = re.search(r"Stream #\d+:\d+(?:\(([^)]+)\))?: Subtitle:", line)
        language = language_match.group(1) if language_match else None
        title_match = re.search(r"title\s*:\s*(.+)$", line, re.IGNORECASE)
        title = title_match.group(1).strip() if title_match else None
        codec_match = re.search(r"Subtitle:\s*([a-zA-Z0-9_]+)", line)
        codec_name = codec_match.group(1).lower() if codec_match else "unknown"
        tracks.append(EmbeddedSubtitleTrackInfo(
            ordinal=subtitle_ordinal,
            name=subtitle_track_display_name(subtitle_ordinal, language, title),
            codec_name=codec_name,
            text_supported=is_text_subtitle_codec(codec_name),
        ))
        subtitle_ordinal += 1
    return tracks


def extract_embedded_subtitle_track(input_path: Path, output_path: Path, track_index: int) -> Path:
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    command = [
        ffmpeg,
        "-y",
        "-i",
        str(input_path),
        "-map",
        f"0:s:{track_index}",
        str(output_path),
    ]
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr.strip() or f"ffmpeg subtitle extraction failed for track {track_index}")
    return output_path


def load_subtitle_tracks(
    input_path: Path,
    output_path: Path,
    subtitle_arg: str | None,
    selected_tracks: list[int] | None,
) -> list[SubtitleTrack]:
    if not subtitle_arg:
        return []

    if subtitle_arg != "embedded":
        cues = parse_srt(Path(subtitle_arg))
        if not cues:
            return []
        return [SubtitleTrack(name=Path(subtitle_arg).stem or "Subtitles", cues=cues)]

    available_tracks = probe_embedded_subtitle_tracks(input_path)
    if not available_tracks:
        raise RuntimeError("No embedded subtitle tracks were found.")

    if selected_tracks:
        wanted_tracks = list(dict.fromkeys(selected_tracks))
    else:
        wanted_tracks = [track.ordinal for track in available_tracks if track.text_supported]
        if not wanted_tracks:
            raise RuntimeError("No supported text embedded subtitle tracks were found.")
    track_info_map = {track.ordinal: track for track in available_tracks}
    subtitle_tracks: list[SubtitleTrack] = []

    for track_index in wanted_tracks:
        track_info = track_info_map.get(track_index)
        if track_info is None:
            raise RuntimeError(f"Embedded subtitle track {track_index} is not available.")
        if not track_info.text_supported:
            raise RuntimeError(
                f"Embedded subtitle track {track_index} ({track_info.name}) uses unsupported bitmap codec "
                f"'{track_info.codec_name}'. Select a text subtitle track instead."
            )
        extracted = output_path.parent / f"{output_path.stem}.track{track_index}.srt"
        try:
            extract_embedded_subtitle_track(input_path, extracted, track_index)
            cues = parse_srt(extracted)
            if cues:
                subtitle_tracks.append(SubtitleTrack(name=track_info.name, cues=cues))
        finally:
            try:
                extracted.unlink()
            except FileNotFoundError:
                pass
    return subtitle_tracks


def force_even(value: int, maximum: int) -> int:
    value = max(2, min(value, maximum))
    if value % 2 == 0:
        return value
    if value + 1 <= maximum:
        return value + 1
    return value - 1


def fit_dimensions(source_width: int, source_height: int, max_width: int, max_height: int) -> tuple[int, int, int, int]:
    scale = min(max_width / source_width, max_height / source_height)
    width = force_even(int(round(source_width * scale)), max_width)
    height = force_even(int(round(source_height * scale)), max_height)
    x = (max_width - width) // 2
    y = (max_height - height) // 2
    return width, height, x, y


def parse_fraction(value: str | None) -> float:
    if not value or value in {"0", "0/0"}:
        return 0.0
    if "/" in value:
        numerator_text, denominator_text = value.split("/", 1)
        numerator = float(numerator_text or 0)
        denominator = float(denominator_text or 0)
        if denominator == 0:
            return 0.0
        return numerator / denominator
    return float(value)


def probe_video(input_path: Path) -> VideoProbe:
    ffprobe = ffprobe_path()
    if ffprobe is not None:
        completed = subprocess.run(
            [
                str(ffprobe),
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=width,height,sample_aspect_ratio,display_aspect_ratio,avg_frame_rate,r_frame_rate:"
                "format=duration",
                "-of",
                "json",
                str(input_path),
            ],
            capture_output=True,
            text=True,
        )
        if completed.returncode == 0:
            try:
                payload = json.loads(completed.stdout or "{}")
            except json.JSONDecodeError:
                payload = {}
            streams = payload.get("streams", [])
            if streams and isinstance(streams[0], dict):
                stream = streams[0]
                width = int(stream.get("width") or 0)
                height = int(stream.get("height") or 0)
                display_width = width
                display_height = height
                dar = stream.get("display_aspect_ratio")
                sar = stream.get("sample_aspect_ratio")
                if isinstance(dar, str) and ":" in dar and height > 0:
                    dar_n_text, dar_d_text = dar.split(":", 1)
                    dar_n = int(dar_n_text or 0)
                    dar_d = int(dar_d_text or 0)
                    if dar_n > 0 and dar_d > 0:
                        display_width = max(1, int(round(height * dar_n / dar_d)))
                elif isinstance(sar, str) and ":" in sar and width > 0:
                    sar_n_text, sar_d_text = sar.split(":", 1)
                    sar_n = int(sar_n_text or 0)
                    sar_d = int(sar_d_text or 0)
                    if sar_n > 0 and sar_d > 0:
                        display_width = max(1, int(round(width * sar_n / sar_d)))
                fps = parse_fraction(stream.get("avg_frame_rate") or stream.get("r_frame_rate"))
                duration_text = payload.get("format", {}).get("duration")
                duration = float(duration_text) if duration_text else 0.0
                if width > 0 and height > 0 and fps > 0:
                    return VideoProbe(
                        storage_width=width,
                        storage_height=height,
                        display_width=display_width,
                        display_height=display_height,
                        fps=fps,
                        duration=duration,
                    )

    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    completed = subprocess.run(
        [ffmpeg, "-hide_banner", "-i", str(input_path)],
        capture_output=True,
        text=True,
    )
    stderr = completed.stderr
    width = 0
    height = 0
    display_width = 0
    display_height = 0
    fps = 0.0
    duration = 0.0

    duration_match = re.search(r"Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)", stderr)
    if duration_match:
        duration = (
            int(duration_match.group(1)) * 3600
            + int(duration_match.group(2)) * 60
            + float(duration_match.group(3))
        )

    for line in stderr.splitlines():
        if " Video:" not in line:
            continue
        size_match = re.search(
            r"(?P<w>\d{2,5})x(?P<h>\d{2,5})(?:\s*\[SAR\s*(?P<sar_n>\d+):(?P<sar_d>\d+)\s*DAR\s*(?P<dar_n>\d+):(?P<dar_d>\d+)\])?",
            line,
        )
        fps_match = re.search(r"(?P<fps>\d+(?:\.\d+)?)\s*fps", line)
        if size_match:
            width = int(size_match.group("w"))
            height = int(size_match.group("h"))
            display_width = width
            display_height = height
            if size_match.group("dar_n") and size_match.group("dar_d"):
                dar_n = int(size_match.group("dar_n"))
                dar_d = int(size_match.group("dar_d"))
                if dar_n > 0 and dar_d > 0:
                    display_width = max(1, int(round(height * dar_n / dar_d)))
            elif size_match.group("sar_n") and size_match.group("sar_d"):
                sar_n = int(size_match.group("sar_n"))
                sar_d = int(size_match.group("sar_d"))
                if sar_n > 0 and sar_d > 0:
                    display_width = max(1, int(round(width * sar_n / sar_d)))
        if fps_match:
            fps = float(fps_match.group("fps"))
        if width and height and fps > 0:
            break

    return VideoProbe(
        storage_width=width,
        storage_height=height,
        display_width=display_width or width,
        display_height=display_height or height,
        fps=fps,
        duration=duration,
    )


def compress_chunk_payload(chunk_blob: bytes, zlib_level: int) -> bytes:
    compressed_blob = zlib.compress(chunk_blob, level=zlib_level)
    if len(compressed_blob) + 16 < len(chunk_blob):
        return compressed_blob
    return chunk_blob


def write_header(
    handle,
    *,
    max_width: int,
    max_height: int,
    video_x: int,
    video_y: int,
    video_width: int,
    video_height: int,
    fps: float,
    chunk_frames: int,
    frame_count: int,
    chunk_count: int,
    subtitle_count: int,
    index_offset: int,
    subtitle_offset: int,
) -> None:
    handle.seek(0)
    handle.write(
        HEADER_STRUCT.pack(
            MAGIC,
            VERSION,
            0,
            max_width,
            max_height,
            video_x,
            video_y,
            video_width,
            video_height,
            int(round(fps * 1000)),
            1000,
            0,
            chunk_frames,
            frame_count,
            chunk_count,
            subtitle_count,
            index_offset,
            subtitle_offset,
        )
    )


def format_fps_value(fps: float) -> str:
    return f"{fps:.6f}".rstrip("0").rstrip(".")


def h264_stream_profile_options(chunk_frames: int, stream_profile: str) -> tuple[str | None, str, list[str]]:
    keyint = 1 if stream_profile == "intra" else chunk_frames
    base_params = [
        f"keyint={keyint}",
        f"min-keyint={keyint}",
        "scenecut=0",
        "repeat-headers=1",
        "aud=1",
        "bframes=0",
        "ref=1",
        "cabac=0",
        "slices=1",
        "force-cfr=1",
        "weightp=0",
    ]

    if stream_profile == "fast":
        tune = "fastdecode"
        extra_params = [
            "no-deblock=1",
            "partitions=none",
            "aq-mode=0",
            "mbtree=0",
            "rc-lookahead=0",
            "sync-lookahead=0",
            "me=dia",
            "subme=0",
            "trellis=0",
        ]
    elif stream_profile == "balanced":
        tune = None
        extra_params = [
            "aq-mode=1",
            "mbtree=1",
            "rc-lookahead=12",
            "sync-lookahead=12",
            "me=hex",
            "subme=2",
            "trellis=0",
        ]
    elif stream_profile == "quality":
        tune = None
        extra_params = [
            "aq-mode=1",
            "mbtree=1",
            "rc-lookahead=20",
            "sync-lookahead=20",
            "me=hex",
            "subme=6",
            "trellis=1",
        ]
    elif stream_profile == "intra":
        tune = None
        extra_params = [
            "no-deblock=1",
            "aq-mode=1",
            "mbtree=0",
            "rc-lookahead=0",
            "sync-lookahead=0",
            "subme=4",
            "trellis=1",
        ]
    else:
        raise ValueError(f"Unsupported stream profile: {stream_profile}")

    return tune, ":".join(base_params + extra_params), ["filter_units=remove_types=6"]


def build_ffmpeg_command(
    *,
    input_path: Path,
    output_path: Path,
    width: int,
    height: int,
    fps: float,
    chunk_frames: int,
    crf: int,
    preset: str,
    level: str,
    stream_profile: str,
    start: float,
    duration: float | None,
) -> list[str]:
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    vf = f"fps={format_fps_value(fps)},scale={width}:{height}:flags=lanczos,setsar=1"
    tune, x264_params, bitstream_filters = h264_stream_profile_options(chunk_frames, stream_profile)
    command = [ffmpeg, "-y"]
    if start > 0:
        command += ["-ss", f"{start:.3f}"]
    command += ["-i", str(input_path)]
    if duration is not None:
        command += ["-t", f"{duration:.3f}"]
    command += [
        "-vf",
        vf,
        "-r",
        format_fps_value(fps),
        "-an",
        "-sn",
        "-dn",
        "-threads",
        "1",
        "-c:v",
        "libx264",
        "-preset",
        preset,
    ]
    if tune:
        command += ["-tune", tune]
    command += [
        "-profile:v",
        "baseline",
        "-level:v",
        level,
        "-pix_fmt",
        "yuv420p",
        "-g",
        str(chunk_frames),
        "-keyint_min",
        str(chunk_frames),
        "-sc_threshold",
        "0",
        "-bf",
        "0",
        "-refs",
        "1",
        "-force_key_frames",
        f"expr:gte(n,n_forced*{chunk_frames})",
        "-x264-params",
        x264_params,
    ]
    for bitstream_filter in bitstream_filters:
        command += ["-bsf:v", bitstream_filter]
    command += [
        "-crf",
        str(crf),
        "-f",
        "h264",
        str(output_path),
    ]
    return command


def encode_h264_bitstream(
    *,
    input_path: Path,
    width: int,
    height: int,
    fps: float,
    chunk_frames: int,
    crf: int,
    preset: str,
    level: str,
    stream_profile: str,
    start: float,
    duration: float | None,
) -> bytes:
    with tempfile.TemporaryDirectory(prefix="nvp-h264-") as temp_dir:
        bitstream_path = Path(temp_dir) / "video.264"
        command = build_ffmpeg_command(
            input_path=input_path,
            output_path=bitstream_path,
            width=width,
            height=height,
            fps=fps,
            chunk_frames=chunk_frames,
            crf=crf,
            preset=preset,
            level=level,
            stream_profile=stream_profile,
            start=start,
            duration=duration,
        )
        completed = subprocess.run(command, capture_output=True, text=True)
        if completed.returncode != 0:
            raise RuntimeError(completed.stderr.strip() or "ffmpeg H.264 encoding failed")
        return bitstream_path.read_bytes()


def parse_annex_b_nalus(bitstream: bytes) -> list[NalUnit]:
    matches = list(START_CODE_RE.finditer(bitstream))
    if not matches:
        raise RuntimeError("FFmpeg did not produce an Annex B H.264 bitstream.")

    nal_units: list[NalUnit] = []
    for index, match in enumerate(matches):
        start = match.start()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(bitstream)
        if match.end() >= end:
            continue
        nal_units.append(NalUnit(
            nal_type=bitstream[match.end()] & 0x1F,
            data=bitstream[start:end],
        ))

    if not nal_units:
        raise RuntimeError("No H.264 NAL units were found in the encoded bitstream.")
    return nal_units


def group_nals_into_access_units(nal_units: list[NalUnit]) -> list[AccessUnit]:
    access_units: list[AccessUnit] = []
    current: list[NalUnit] = []
    current_has_vcl = False

    for unit in nal_units:
        if unit.nal_type == NAL_AUD and current:
            if current_has_vcl:
                access_units.append(AccessUnit(nal_units=current))
            current = []
            current_has_vcl = False
        elif current and current_has_vcl and unit.nal_type in {NAL_AUD, NAL_SPS, NAL_PPS, NAL_SEI}:
            access_units.append(AccessUnit(nal_units=current))
            current = []
            current_has_vcl = False

        current.append(unit)
        if unit.nal_type in VCL_NAL_TYPES:
            current_has_vcl = True

    if current and current_has_vcl:
        access_units.append(AccessUnit(nal_units=current))

    if not access_units:
        raise RuntimeError("No decodable H.264 access units were found in the bitstream.")
    return access_units


def access_unit_starts_chunk(unit: AccessUnit) -> bool:
    return any(nal.nal_type in CHUNK_BOUNDARY_NAL_TYPES for nal in unit.nal_units)


def chunk_has_independent_start(unit: AccessUnit) -> bool:
    return unit.contains_type(NAL_IDR) and unit.contains_type(NAL_SPS) and unit.contains_type(NAL_PPS)


def group_access_units_into_chunks(access_units: list[AccessUnit], chunk_frames: int, stream_profile: str) -> list[list[AccessUnit]]:
    chunks: list[list[AccessUnit]] = []
    current: list[AccessUnit] = []

    for unit in access_units:
        if current and (
            (stream_profile == "intra" and len(current) >= chunk_frames)
            or (stream_profile != "intra" and access_unit_starts_chunk(unit))
        ):
            chunks.append(current)
            current = []
        current.append(unit)

    if current:
        chunks.append(current)

    if not chunks:
        raise RuntimeError("The H.264 bitstream did not produce any `.nvp` chunks.")

    for index, chunk in enumerate(chunks):
        if not chunk:
            raise RuntimeError("Encountered an empty chunk while grouping H.264 access units.")
        if not chunk_has_independent_start(chunk[0]):
            raise RuntimeError(
                f"Chunk {index} does not start with SPS/PPS/IDR. "
                "Check the FFmpeg keyframe and repeat-headers settings."
            )
        if index + 1 < len(chunks) and len(chunk) != chunk_frames:
            raise RuntimeError(
                f"Chunk {index} has {len(chunk)} frames, expected {chunk_frames}. "
                "FFmpeg did not honor the requested fixed IDR cadence."
            )
        if len(chunk) > chunk_frames:
            raise RuntimeError(
                f"Chunk {index} has {len(chunk)} frames, which exceeds chunk_frames={chunk_frames}."
            )

    return chunks


def build_access_unit_payload(unit: AccessUnit, *, keep_parameter_sets: bool) -> bytes:
    payload = bytearray()
    for nal in unit.nal_units:
        if nal.nal_type == NAL_AUD:
            continue
        if not keep_parameter_sets and nal.nal_type in {NAL_SPS, NAL_PPS, NAL_SEI}:
            continue
        payload.extend(nal.data)
    return bytes(payload)


def build_chunk_blob(access_units: list[AccessUnit], stream_profile: str) -> tuple[bytes, list[int]]:
    payload = bytearray()
    frame_offsets: list[int] = []
    for index, unit in enumerate(access_units):
        frame_offsets.append(len(payload))
        payload.extend(build_access_unit_payload(
            unit,
            keep_parameter_sets=(index == 0 or stream_profile != "intra"),
        ))
    return bytes(payload), frame_offsets


def encode(args: argparse.Namespace) -> EncodeStats:
    input_path = Path(args.input).resolve()
    output_path = normalize_output_path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    stats_path = output_path.with_suffix(".json")

    subtitle_tracks = load_subtitle_tracks(input_path, output_path, args.subtitle, args.subtitle_track)
    subtitle_count = sum(len(track.cues) for track in subtitle_tracks)
    if subtitle_tracks and args.subtitle == "embedded":
        log(f"Embedding {len(subtitle_tracks)} subtitle track(s).", quiet=args.quiet)

    video_probe = probe_video(input_path)
    source_width = video_probe.display_width or video_probe.storage_width
    source_height = video_probe.display_height or video_probe.storage_height
    if source_width <= 0 or source_height <= 0:
        raise RuntimeError("Failed to determine input video dimensions.")

    fps = video_probe.fps if isinstance(args.fps, str) and args.fps.lower() == "source" else float(args.fps)
    if fps <= 0:
        raise RuntimeError("Target fps must be greater than zero.")

    target_width, target_height, video_x, video_y = fit_dimensions(
        source_width,
        source_height,
        args.max_width,
        args.max_height,
    )

    log(
        f"Encoding {input_path.name} -> {target_width}x{target_height} @ {fps:.3f}fps "
        f"(source {video_probe.storage_width}x{video_probe.storage_height}, display {source_width}x{source_height}, profile {args.stream_profile})",
        quiet=args.quiet,
    )

    start_time = time.time()
    bitstream = encode_h264_bitstream(
        input_path=input_path,
        width=target_width,
        height=target_height,
        fps=fps,
        chunk_frames=args.chunk_frames,
        crf=args.crf,
        preset=args.preset,
        level=args.level,
        stream_profile=args.stream_profile,
        start=args.start,
        duration=args.duration,
    )
    log(
        f"FFmpeg produced {len(bitstream) / 1024:.1f} KiB of Annex B H.264 in {time.time() - start_time:.1f}s.",
        quiet=args.quiet,
    )

    nal_units = parse_annex_b_nalus(bitstream)
    access_units = group_nals_into_access_units(nal_units)
    chunks = group_access_units_into_chunks(access_units, args.chunk_frames, args.stream_profile)

    if not access_units:
        raise RuntimeError("No frames were found in the encoded H.264 bitstream.")

    output_handle = output_path.open("wb")
    output_handle.write(b"\0" * HEADER_STRUCT.size)

    chunk_index: list[tuple[int, int, int, int, int, int]] = []
    frame_cursor = 0
    for chunk_number, access_unit_chunk in enumerate(chunks, start=1):
        chunk_payload, frame_offsets = build_chunk_blob(access_unit_chunk, args.stream_profile)
        stored_chunk_blob = bytearray()
        stored_chunk_blob += struct.pack("<I", len(chunk_payload))
        for frame_offset in frame_offsets:
            stored_chunk_blob += struct.pack("<I", frame_offset)
        stored_chunk_blob += chunk_payload
        while len(stored_chunk_blob) % 4:
            stored_chunk_blob.append(0)

        stored_blob = compress_chunk_payload(bytes(stored_chunk_blob), args.zlib_level)
        offset = output_handle.tell()
        output_handle.write(stored_blob)
        chunk_index.append((
            offset,
            len(stored_blob),
            len(stored_chunk_blob),
            frame_cursor,
            len(access_unit_chunk),
            0,
        ))

        elapsed = time.time() - start_time
        fps_done = frame_cursor / elapsed if elapsed > 0 else 0.0
        frame_end = frame_cursor + len(access_unit_chunk) - 1
        progress = (chunk_number / len(chunks)) * 100.0
        eta_chunks = len(chunks) - chunk_number
        eta = (eta_chunks * args.chunk_frames / fps_done) if fps_done > 0 and eta_chunks > 0 else 0.0
        log(
            f"Chunk {chunk_number:03d}: {progress:5.1f}% | ETA {format_duration_hms(eta)} | "
            f"frames {frame_cursor}-{frame_end} | size {len(stored_blob) / 1024:.1f} KiB | "
            f"total size {output_handle.tell() / (1024 * 1024):.2f} MiB",
            quiet=args.quiet,
        )
        frame_cursor += len(access_unit_chunk)

    index_offset = output_handle.tell()
    for entry in chunk_index:
        output_handle.write(CHUNK_INDEX_STRUCT.pack(*entry))

    subtitle_offset = output_handle.tell()
    if subtitle_tracks:
        output_handle.write(struct.pack("<H", len(subtitle_tracks)))
        for track in subtitle_tracks:
            encoded_name = sanitize_subtitle_text(track.name).encode("ascii", "replace")
            output_handle.write(struct.pack("<HI", len(encoded_name), len(track.cues)))
            output_handle.write(encoded_name)
        for track in subtitle_tracks:
            for cue in track.cues:
                encoded = sanitize_subtitle_text(cue.text).encode("ascii", "replace")
                output_handle.write(struct.pack("<IIH", cue.start_ms, cue.end_ms, len(encoded)))
                output_handle.write(encoded)

    write_header(
        output_handle,
        max_width=args.max_width,
        max_height=args.max_height,
        video_x=video_x,
        video_y=video_y,
        video_width=target_width,
        video_height=target_height,
        fps=fps,
        chunk_frames=args.chunk_frames,
        frame_count=len(access_units),
        chunk_count=len(chunks),
        subtitle_count=subtitle_count,
        index_offset=index_offset,
        subtitle_offset=subtitle_offset,
    )
    output_handle.close()

    bytes_written = output_path.stat().st_size
    stats = EncodeStats(
        input_path=str(input_path),
        output_path=str(output_path),
        width=target_width,
        height=target_height,
        video_x=video_x,
        video_y=video_y,
        fps=fps,
        frame_count=len(access_units),
        chunk_count=len(chunks),
        idr_chunks=len(chunks),
        raw_h264_bytes=len(bitstream),
        bytes_written=bytes_written,
        average_bytes_per_frame=bytes_written / len(access_units),
    )
    stats_path.write_text(json.dumps(asdict(stats), indent=2), encoding="utf-8")
    log(
        f"Wrote {output_path.name}: {bytes_written / (1024 * 1024):.2f} MiB | "
        f"{len(access_units)} frames | {len(chunks)} chunks | raw H.264 {len(bitstream) / 1024:.1f} KiB",
        quiet=args.quiet,
    )
    return stats


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="Input video file")
    parser.add_argument("--output", required=True, help="Output .nvp.tns file")
    parser.add_argument("--subtitle", help="Optional .srt path or 'embedded'")
    parser.add_argument("--subtitle-track", dest="subtitle_track", action="append", type=int, help="Embedded subtitle track index to include; repeat to keep multiple tracks")
    parser.add_argument("--fps", default="12", help="Target framerate or 'source'")
    parser.add_argument("--max-width", type=int, default=SCREEN_W, help="Fit width")
    parser.add_argument("--max-height", type=int, default=SCREEN_H, help="Fit height")
    parser.add_argument("--chunk-frames", type=int, default=24, help="Frames per streamed chunk and forced IDR interval")
    parser.add_argument("--crf", type=int, default=24, help="libx264 CRF quality target")
    parser.add_argument("--preset", default="slow", help="libx264 preset")
    parser.add_argument("--level", default="1.3", help="Target H.264 level")
    parser.add_argument("--stream-profile", choices=STREAM_PROFILES, default="fast", help="Decoder-complexity profile: fast is smoothest, balanced/quality trade more CPU for better image quality")
    parser.add_argument("--zlib-level", type=int, default=9, help="Chunk compression level (0-9)")
    parser.add_argument("--start", type=float, default=0.0, help="Optional clip start offset in seconds")
    parser.add_argument("--duration", type=float, help="Optional clip duration in seconds")
    parser.add_argument("--quiet", action="store_true", help="Silence progress logging")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    encode(parse_args(argv or sys.argv[1:]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
