#!/usr/bin/env python3
"""Encode a normal video file into a streamed Ndless-native movie container."""

from __future__ import annotations

import argparse
from collections import deque
import hashlib
import html
import json
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
from dataclasses import asdict, dataclass
from pathlib import Path

import imageio_ffmpeg
try:
    import psutil
except ImportError:
    psutil = None


MAGIC = b"NVP1"
VERSION = 10
SCREEN_W = 320
SCREEN_H = 240
HEADER_STRUCT = struct.Struct("<4sHHHHHHHHHHHHIIIII")
CHUNK_INDEX_STRUCT = struct.Struct("<IIIIII")
START_CODE_RE = re.compile(rb"\x00\x00(?:\x00)?\x01")
SUBTITLE_LINE_BREAK_RE = re.compile(r"(?i)<br\s*/?>|\\N|\\n")
SUBTITLE_TAG_RE = re.compile(r"(?s)<[^>]+>")
SUBTITLE_ASS_OVERRIDE_RE = re.compile(r"\{\\[^}]*\}")
ASS_OVERRIDE_BLOCK_RE = re.compile(r"\{([^}]*)\}")
ASS_POS_RE = re.compile(r"\\pos\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*\)")
ASS_MOVE_RE = re.compile(
    r"\\move\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)"
)
ASS_AN_RE = re.compile(r"\\an(\d)")
ASS_A_RE = re.compile(r"\\a(\d+)")
BBOX_TIME_RE = re.compile(r"pts_time:(?P<time>-?\d+(?:\.\d+)?)")
BBOX_VALUES_RE = re.compile(
    r"x1:(?P<x1>\d+)\s+x2:(?P<x2>\d+)\s+y1:(?P<y1>\d+)\s+y2:(?P<y2>\d+)\s+w:(?P<w>\d+)\s+h:(?P<h>\d+)"
)
DEFAULT_BURN_SUBTITLE_SIZE = 1.0
DEFAULT_BURN_SUBTITLE_HEIGHT_RATIO = 0.10
BITMAP_SUBTITLE_BBOX_PADDING = 4
FILTER_COMPLEX_SCRIPT_PLACEHOLDER = "__NVP_FILTER_COMPLEX_SCRIPT__"
BITMAP_SUBTITLE_ANALYSIS_CACHE_VERSION = 1
PGS_SEGMENT_PALETTE = 0x14
PGS_SEGMENT_OBJECT = 0x15
PGS_SEGMENT_PRESENTATION = 0x16
PGS_SEGMENT_WINDOW = 0x17
PGS_SEGMENT_DISPLAY = 0x80

NAL_IDR = 5
NAL_SEI = 6
NAL_SPS = 7
NAL_PPS = 8
NAL_AUD = 9
VCL_NAL_TYPES = {1, NAL_IDR}
CHUNK_BOUNDARY_NAL_TYPES = {NAL_SPS, NAL_PPS, NAL_IDR}
STREAM_PROFILES = ("fast", "balanced", "quality", "intra")
DEFAULT_MAX_CHUNK_KIB = 1024
DEFAULT_MAX_IDR_FRAMES = 24
SUBTITLE_COORD_SCALE = 10000
SUBTITLE_CUE_POSITION_NONE = 0
SUBTITLE_CUE_POSITION_MARGIN = 1
SUBTITLE_CUE_POSITION_ABSOLUTE = 2
ASS_DEFAULT_PLAYRES_X = 384
ASS_DEFAULT_PLAYRES_Y = 288


@dataclass(slots=True)
class SubtitleCue:
    start_ms: int
    end_ms: int
    text: str
    position_mode: int = SUBTITLE_CUE_POSITION_NONE
    align: int = 2
    pos_x: int = 0
    pos_y: int = 0
    margin_l: int = 0
    margin_r: int = 0
    margin_v: int = 0


@dataclass(slots=True)
class SubtitleTrack:
    name: str
    cues: list[SubtitleCue]


@dataclass(slots=True)
class BitmapSubtitleSegment:
    start_s: float
    end_s: float
    x: int
    y: int
    w: int
    h: int


@dataclass(slots=True)
class AssStyle:
    align: int = 2
    margin_l: int = 0
    margin_r: int = 0
    margin_v: int = 0


@dataclass(slots=True)
class EmbeddedSubtitleTrackInfo:
    ordinal: int
    name: str
    codec_name: str
    text_supported: bool


@dataclass(slots=True)
class BurnSubtitleSource:
    kind: str
    label: str
    path: Path | None = None
    stream_index: int | None = None


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


@dataclass(slots=True)
class ChunkSegment:
    first_frame: int
    access_units: list[AccessUnit]
    blob_size: int


def log(message: str, *, quiet: bool = False) -> None:
    if not quiet:
        print(message, flush=True)


def parse_ffmpeg_time(value: str) -> float:
    parts = value.strip().split(":")
    if len(parts) != 3:
        return 0.0
    try:
        hours = int(parts[0])
        minutes = int(parts[1])
        seconds = float(parts[2])
    except ValueError:
        return 0.0
    return (hours * 3600.0) + (minutes * 60.0) + seconds


def parse_ffmpeg_speed(value: str | None) -> float:
    if not value:
        return 0.0
    text = value.strip().lower()
    if text.endswith("x"):
        text = text[:-1]
    try:
        speed = float(text)
    except ValueError:
        return 0.0
    return speed if speed > 0.0 else 0.0


def parse_ffmpeg_int(value: str | None) -> int:
    if not value:
        return 0
    try:
        return int(value.strip())
    except ValueError:
        return 0


def format_duration_hms(seconds: float) -> str:
    total_seconds = max(0, int(round(seconds)))
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    secs = total_seconds % 60
    return f"{hours:02d}:{minutes:02d}:{secs:02d}"


def format_binary_size(byte_count: int) -> str:
    if byte_count < 1024:
        return f"{byte_count} B"
    if byte_count < 1024 * 1024:
        return f"{byte_count / 1024:.1f} KiB"
    if byte_count < 10 * 1024 * 1024:
        return f"{byte_count / (1024 * 1024):.3f} MiB"
    if byte_count < 1024 * 1024 * 1024:
        return f"{byte_count / (1024 * 1024):.2f} MiB"
    if byte_count < 10 * 1024 * 1024 * 1024:
        return f"{byte_count / (1024 * 1024 * 1024):.3f} GiB"
    if byte_count < 1024 * 1024 * 1024 * 1024:
        return f"{byte_count / (1024 * 1024 * 1024):.2f} GiB"
    return f"{byte_count / (1024 * 1024 * 1024 * 1024):.2f} TiB"


class ProgressEstimator:
    def __init__(
        self,
        *,
        recent_window_seconds: float = 120.0,
        speed_alpha: float = 0.15,
        size_alpha: float = 0.10,
    ) -> None:
        self.recent_window_seconds = recent_window_seconds
        self.speed_alpha = speed_alpha
        self.size_alpha = size_alpha
        self.samples: deque[tuple[float, float, int]] = deque()
        self.smoothed_speed = 0.0
        self.smoothed_size_rate = 0.0

    @staticmethod
    def _smooth(previous: float, current: float, alpha: float) -> float:
        if current <= 0.0:
            return previous
        if previous <= 0.0:
            return current
        return (alpha * current) + ((1.0 - alpha) * previous)

    def update(
        self,
        *,
        real_time: float,
        media_time: float,
        total_media_time: float | None,
        byte_count: int,
        reported_speed: float,
    ) -> tuple[float, int | None]:
        self.samples.append((real_time, media_time, byte_count))
        while len(self.samples) > 1 and real_time - self.samples[0][0] > self.recent_window_seconds:
            self.samples.popleft()

        cumulative_speed = (media_time / real_time) if real_time > 0.0 and media_time > 0.0 else 0.0
        recent_speed = 0.0
        recent_size_rate = 0.0
        if len(self.samples) >= 2:
            base_real, base_media, base_bytes = self.samples[0]
            delta_real = real_time - base_real
            delta_media = media_time - base_media
            delta_bytes = byte_count - base_bytes
            if delta_real >= 15.0 and delta_media > 0.0:
                recent_speed = delta_media / delta_real
            if delta_media >= 30.0 and delta_bytes > 0:
                recent_size_rate = delta_bytes / delta_media

        if reported_speed > 0.0 and media_time < 30.0:
            effective_speed = reported_speed
        else:
            effective_speed = cumulative_speed or recent_speed or reported_speed
            if cumulative_speed > 0.0 and recent_speed > 0.0:
                effective_speed = (cumulative_speed * 0.8) + (recent_speed * 0.2)
            if reported_speed > 0.0:
                if effective_speed > 0.0 and media_time < 120.0:
                    effective_speed = (effective_speed * 0.8) + (reported_speed * 0.2)
                elif effective_speed <= 0.0:
                    effective_speed = reported_speed
        self.smoothed_speed = self._smooth(self.smoothed_speed, effective_speed, self.speed_alpha)

        eta = 0.0
        if total_media_time and total_media_time > 0.0 and self.smoothed_speed > 0.0:
            eta = max(0.0, total_media_time - media_time) / self.smoothed_speed

        expected_size = None
        cumulative_size_rate = (byte_count / media_time) if media_time > 0.0 and byte_count > 0 else 0.0
        effective_size_rate = cumulative_size_rate or recent_size_rate
        if cumulative_size_rate > 0.0 and recent_size_rate > 0.0:
            effective_size_rate = (cumulative_size_rate * 0.85) + (recent_size_rate * 0.15)
        self.smoothed_size_rate = self._smooth(self.smoothed_size_rate, effective_size_rate, self.size_alpha)
        if total_media_time and total_media_time > 0.0 and media_time > 0.0:
            projected_size_rate = 0.0
            if media_time < 30.0 and cumulative_size_rate > 0.0:
                # Show a useful projection immediately, then hand off to the smoothed model.
                projected_size_rate = cumulative_size_rate
                if self.smoothed_size_rate > 0.0:
                    projected_size_rate = (cumulative_size_rate * 0.75) + (self.smoothed_size_rate * 0.25)
            elif self.smoothed_size_rate > 0.0:
                projected_size_rate = self.smoothed_size_rate
            elif cumulative_size_rate > 0.0:
                projected_size_rate = cumulative_size_rate

            if projected_size_rate > 0.0:
                expected_size = max(byte_count, int(round(projected_size_rate * total_media_time)))

        return eta, expected_size


def log_ffmpeg_progress(
    *,
    label: str,
    progress_state: dict[str, str],
    start_time: float,
    total_duration: float | None,
    progress_estimator: ProgressEstimator,
    quiet: bool,
    include_size: bool,
    fallback_fps: float | None = None,
) -> None:
    if quiet:
        return

    out_time = parse_ffmpeg_time(progress_state.get("out_time", "00:00:00.000"))
    if out_time <= 0.0 and fallback_fps and fallback_fps > 0.0:
        frame_count = parse_ffmpeg_int(progress_state.get("frame"))
        if frame_count > 0:
            out_time = frame_count / fallback_fps
    ratio = (out_time / total_duration) if total_duration and total_duration > 0.0 else 0.0
    ratio = min(max(ratio, 0.0), 1.0)
    elapsed = time.time() - start_time
    total_size = parse_ffmpeg_int(progress_state.get("total_size")) if include_size else 0
    speed = progress_state.get("speed", "?")
    if (not speed or speed == "N/A") and elapsed > 0.0 and out_time > 0.0:
        speed = f"{out_time / elapsed:.1f}x"
    speed_factor = parse_ffmpeg_speed(speed)
    eta, expected_size = progress_estimator.update(
        real_time=elapsed,
        media_time=out_time,
        total_media_time=total_duration,
        byte_count=total_size,
        reported_speed=speed_factor,
    )
    if eta <= 0.0 and ratio > 0.0:
        eta = elapsed * (1.0 - ratio) / ratio

    parts = [label]
    if total_duration and total_duration > 0.0:
        parts.append(f"{ratio * 100.0:5.1f}%")
        parts.append(f"ETA {format_duration_hms(eta)}")
    parts.append(f"time {format_duration_hms(out_time)}")
    parts.append(f"real {format_duration_hms(elapsed)}")
    if include_size:
        expected_size_text = format_binary_size(expected_size) if expected_size is not None else "?"
        parts.append(f"size {format_binary_size(total_size)}")
        parts.append(f"expect {expected_size_text}")
    parts.append(f"speed {speed}")
    log(" | ".join(parts), quiet=False)


def stop_ffmpeg_process(process: subprocess.Popen[str], *, label: str, quiet: bool) -> None:
    if process.poll() is not None:
        return

    log(f"{label} interrupted. Stopping FFmpeg...", quiet=quiet)

    if process.stdin is not None:
        try:
            process.stdin.write("q\n")
            process.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            pass
        try:
            process.stdin.close()
        except OSError:
            pass

    try:
        process.wait(timeout=5.0)
        return
    except subprocess.TimeoutExpired:
        pass

    process.terminate()
    try:
        process.wait(timeout=2.0)
        return
    except subprocess.TimeoutExpired:
        pass
    process.kill()
    process.wait()


def start_stderr_collector(
    process: subprocess.Popen[str],
    *,
    sink: list[str],
) -> threading.Thread | None:
    if process.stderr is None:
        return None

    def collect() -> None:
        assert process.stderr is not None
        try:
            for raw_line in process.stderr:
                line = raw_line.strip()
                if line:
                    sink.append(line)
        finally:
            process.stderr.close()

    thread = threading.Thread(target=collect, name="nvp-stderr-collector", daemon=True)
    thread.start()
    return thread


def get_process_read_bytes(process_info: object | None) -> int:
    if process_info is None:
        return 0
    try:
        counters = process_info.io_counters()
    except Exception:
        return 0
    return max(0, int(getattr(counters, "read_bytes", 0) or 0))


def log_input_scan_progress(
    *,
    label: str,
    start_time: float,
    progress_ratio: float,
    input_size: int,
    read_bytes: int,
    dumped_bytes: int,
    total_duration: float | None,
    quiet: bool,
) -> None:
    if quiet:
        return

    ratio = min(max(progress_ratio, 0.0), 1.0)
    elapsed = max(0.0, time.time() - start_time)
    eta_text = "?"
    if 0.001 <= ratio < 1.0:
        eta_text = format_duration_hms(elapsed * (1.0 - ratio) / ratio)
    elif ratio >= 1.0:
        eta_text = "00:00:00"

    parts = [
        label,
        f"{ratio * 100.0:5.1f}%",
        f"ETA {eta_text}",
    ]
    if input_size > 0:
        parts.append(f"read {format_binary_size(min(read_bytes, input_size))} / {format_binary_size(input_size)}")
    else:
        parts.append("read ?")
    parts.append(f"dumped {format_binary_size(dumped_bytes)}")
    parts.append(f"real {format_duration_hms(elapsed)}")
    if total_duration and total_duration > 0.0 and elapsed > 0.0 and ratio >= 0.001:
        parts.append(f"speed {(total_duration * ratio) / elapsed:5.1f}x")
    log(" | ".join(parts), quiet=False)


def cleanup_partial_file(path: Path | None) -> None:
    if path is None:
        return
    try:
        path.unlink()
    except FileNotFoundError:
        return
    except OSError:
        return


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


def parse_int(value: str | None, default: int = 0) -> int:
    try:
        return int((value or "").strip())
    except ValueError:
        return default


def scale_subtitle_coord(value: float, extent: int) -> int:
    if extent <= 0:
        return 0
    scaled = int(round((max(0.0, value) * SUBTITLE_COORD_SCALE) / extent))
    return max(0, min(SUBTITLE_COORD_SCALE, scaled))


def normalize_ass_alignment(value: int, *, legacy: bool = False) -> int:
    legacy_map = {
        1: 1,
        2: 2,
        3: 3,
        5: 7,
        6: 8,
        7: 9,
        9: 4,
        10: 5,
        11: 6,
    }

    if legacy:
        return legacy_map.get(value, 2)
    if 1 <= value <= 9:
        return value
    return legacy_map.get(value, 2)


def parse_ass_timecode(value: str) -> int | None:
    match = re.match(r"^\s*(\d+):(\d+):(\d+)[.,](\d+)\s*$", value)
    if not match:
        return None
    return (
        int(match.group(1)) * 3600
        + int(match.group(2)) * 60
        + int(match.group(3))
    ) * 1000 + int(match.group(4)[:3].ljust(3, "0"))


def split_ass_fields(payload: str, field_count: int) -> list[str]:
    if field_count <= 0:
        return []
    parts = payload.split(",", field_count - 1)
    if len(parts) < field_count:
        parts.extend([""] * (field_count - len(parts)))
    if field_count == 1:
        return [parts[0]]
    return [part.strip() for part in parts[:-1]] + [parts[-1]]


def extract_ass_alignment_and_position(text: str, default_align: int) -> tuple[int, tuple[float, float] | None]:
    align = default_align
    absolute_pos: tuple[float, float] | None = None

    for block in ASS_OVERRIDE_BLOCK_RE.findall(text):
        an_matches = ASS_AN_RE.findall(block)
        if an_matches:
            align = normalize_ass_alignment(parse_int(an_matches[-1], 2))
        else:
            legacy_matches = ASS_A_RE.findall(block)
            if legacy_matches:
                align = normalize_ass_alignment(parse_int(legacy_matches[-1], 2), legacy=True)

        pos_match = ASS_POS_RE.search(block)
        if pos_match:
            absolute_pos = (float(pos_match.group(1)), float(pos_match.group(2)))
            continue

        if absolute_pos is None:
            move_match = ASS_MOVE_RE.search(block)
            if move_match:
                absolute_pos = (float(move_match.group(1)), float(move_match.group(2)))

    return align, absolute_pos


def parse_ass_style(fields: list[str], values: list[str], *, legacy_alignment: bool) -> tuple[str | None, AssStyle]:
    field_map = {
        fields[index]: values[index].strip()
        for index in range(min(len(fields), len(values)))
    }
    name = field_map.get("name", "").strip()
    style = AssStyle(
        align=normalize_ass_alignment(parse_int(field_map.get("alignment"), 2), legacy=legacy_alignment),
        margin_l=max(0, parse_int(field_map.get("marginl"), 0)),
        margin_r=max(0, parse_int(field_map.get("marginr"), 0)),
        margin_v=max(0, parse_int(field_map.get("marginv"), 0)),
    )
    return (name.casefold() or None), style


def build_ass_cue(
    event_fields: list[str],
    values: list[str],
    styles: dict[str, AssStyle],
    play_res_x: int,
    play_res_y: int,
) -> SubtitleCue | None:
    field_map = {
        event_fields[index]: values[index]
        for index in range(min(len(event_fields), len(values)))
    }
    start_ms = parse_ass_timecode(field_map.get("start", ""))
    end_ms = parse_ass_timecode(field_map.get("end", ""))
    if start_ms is None or end_ms is None or end_ms < start_ms:
        return None

    style = styles.get(field_map.get("style", "").strip().casefold(), styles.get("default", AssStyle()))
    margin_l = max(0, parse_int(field_map.get("marginl"), 0))
    margin_r = max(0, parse_int(field_map.get("marginr"), 0))
    margin_v = max(0, parse_int(field_map.get("marginv"), 0))
    if margin_l <= 0:
        margin_l = style.margin_l
    if margin_r <= 0:
        margin_r = style.margin_r
    if margin_v <= 0:
        margin_v = style.margin_v

    text_field = field_map.get("text", "")
    align, absolute_pos = extract_ass_alignment_and_position(text_field, style.align)
    text = sanitize_subtitle_text(text_field)
    if not text:
        return None

    cue = SubtitleCue(start_ms=start_ms, end_ms=end_ms, text=text, align=align)
    if absolute_pos is not None:
        cue.position_mode = SUBTITLE_CUE_POSITION_ABSOLUTE
        cue.pos_x = scale_subtitle_coord(absolute_pos[0], play_res_x)
        cue.pos_y = scale_subtitle_coord(absolute_pos[1], play_res_y)
    else:
        cue.position_mode = SUBTITLE_CUE_POSITION_MARGIN
        cue.margin_l = scale_subtitle_coord(margin_l, play_res_x)
        cue.margin_r = scale_subtitle_coord(margin_r, play_res_x)
        cue.margin_v = scale_subtitle_coord(margin_v, play_res_y)
    return cue


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


def parse_ass(path: Path) -> list[SubtitleCue]:
    lines = path.read_text(encoding="utf-8-sig", errors="replace").splitlines()
    cues: list[SubtitleCue] = []
    styles: dict[str, AssStyle] = {}
    play_res_x = ASS_DEFAULT_PLAYRES_X
    play_res_y = ASS_DEFAULT_PLAYRES_Y
    section = ""
    style_fields: list[str] = []
    style_legacy_alignment = False
    event_fields: list[str] = []

    for raw_line in lines:
        line = raw_line.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line.casefold()
            continue
        key, separator, value = line.partition(":")
        if not separator:
            continue
        key = key.strip().casefold()
        value = value.lstrip()

        if section == "[script info]":
            if key == "playresx":
                play_res_x = max(1, parse_int(value, ASS_DEFAULT_PLAYRES_X))
            elif key == "playresy":
                play_res_y = max(1, parse_int(value, ASS_DEFAULT_PLAYRES_Y))
        elif section in {"[v4+ styles]", "[v4 styles]"}:
            if key == "format":
                style_fields = [field.strip().casefold() for field in value.split(",")]
                style_legacy_alignment = section == "[v4 styles]"
            elif key == "style" and style_fields:
                values = split_ass_fields(value, len(style_fields))
                name, style = parse_ass_style(style_fields, values, legacy_alignment=style_legacy_alignment)
                if name:
                    styles[name] = style
        elif section == "[events]":
            if key == "format":
                event_fields = [field.strip().casefold() for field in value.split(",")]
            elif key == "dialogue" and event_fields:
                cue = build_ass_cue(
                    event_fields,
                    split_ass_fields(value, len(event_fields)),
                    styles,
                    play_res_x,
                    play_res_y,
                )
                if cue:
                    cues.append(cue)
    return cues


def parse_subtitle_file(path: Path) -> list[SubtitleCue]:
    suffix = path.suffix.lower()
    if suffix in {".ass", ".ssa"}:
        return parse_ass(path)
    return parse_srt(path)


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


def subtitle_track_debug_label(track: EmbeddedSubtitleTrackInfo) -> str:
    return f"{track.ordinal}: {track.name} [{track.codec_name}]"


def ffmpeg_filter_escape_path(path: Path) -> str:
    escaped = path.resolve().as_posix()
    for char in ("\\", ":", "'", "[", "]", ",", ";"):
        escaped = escaped.replace(char, f"\\{char}")
    return escaped


def ffmpeg_filter_escape_value(text: str) -> str:
    escaped = text
    for char in ("\\", "'"):
        escaped = escaped.replace(char, f"\\{char}")
    return escaped


def compute_burn_subtitle_metrics(height: int, size_scale: float) -> tuple[int, int, int]:
    clamped_scale = max(0.25, min(size_scale, 4.0))
    font_size = max(10, int(round(height * DEFAULT_BURN_SUBTITLE_HEIGHT_RATIO * clamped_scale)))
    margin_v = max(4, int(round(font_size * 0.6)))
    outline = max(1, int(round(font_size * 0.08)))
    return font_size, margin_v, outline


def bitmap_subtitle_analysis_cache_path(
    *,
    input_path: Path,
    stream_index: int,
    start: float,
    duration: float | None,
) -> Path:
    stat = input_path.stat()
    cache_key = json.dumps(
        {
            "version": BITMAP_SUBTITLE_ANALYSIS_CACHE_VERSION,
            "input_path": str(input_path),
            "input_size": stat.st_size,
            "input_mtime_ns": stat.st_mtime_ns,
            "stream_index": stream_index,
            "start_ms": int(round(start * 1000.0)),
            "duration_ms": None if duration is None else int(round(duration * 1000.0)),
        },
        sort_keys=True,
    ).encode("utf-8")
    digest = hashlib.sha256(cache_key).hexdigest()
    return Path(tempfile.gettempdir()) / "nvp-subtitle-analysis-cache" / f"{digest}.json"


def load_bitmap_subtitle_analysis_cache(cache_path: Path) -> list[BitmapSubtitleSegment] | None:
    try:
        payload = json.loads(cache_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError, TypeError, ValueError):
        return None

    if not isinstance(payload, list):
        return None

    segments: list[BitmapSubtitleSegment] = []
    try:
        for item in payload:
            if not isinstance(item, dict):
                return None
            segments.append(BitmapSubtitleSegment(
                start_s=float(item["start_s"]),
                end_s=float(item["end_s"]),
                x=int(item["x"]),
                y=int(item["y"]),
                w=int(item["w"]),
                h=int(item["h"]),
            ))
    except (KeyError, TypeError, ValueError):
        return None
    return segments


def save_bitmap_subtitle_analysis_cache(cache_path: Path, segments: list[BitmapSubtitleSegment]) -> None:
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(
        json.dumps(
            [
                {
                    "start_s": segment.start_s,
                    "end_s": segment.end_s,
                    "x": segment.x,
                    "y": segment.y,
                    "w": segment.w,
                    "h": segment.h,
                }
                for segment in segments
            ],
            separators=(",", ":"),
        ),
        encoding="utf-8",
    )


def dump_bitmap_subtitle_stream_data(
    *,
    input_path: Path,
    output_path: Path,
    stream_index: int,
    start: float,
    duration: float | None,
    analyze_duration: float | None,
    quiet: bool,
) -> None:
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"]
    if start > 0:
        command += ["-ss", f"{start:.3f}"]
    if duration is not None:
        command += ["-t", f"{duration:.3f}"]
    command += ["-i", str(input_path)]
    command += [
        "-map",
        f"0:s:{stream_index}",
        "-c",
        "copy",
        "-f",
        "data",
        str(output_path),
    ]

    log("Dumping bitmap subtitle packets for analysis...", quiet=quiet)
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    if process.stderr is None:
        raise RuntimeError("Failed to capture FFmpeg bitmap subtitle dump output.")

    stderr_lines: list[str] = []
    stderr_thread = start_stderr_collector(process, sink=stderr_lines)
    extract_start = time.time()
    try:
        input_size = input_path.stat().st_size
    except OSError:
        input_size = 0
    process_info = None
    if psutil is not None:
        try:
            process_info = psutil.Process(process.pid)
        except Exception:
            process_info = None
    last_log_time = 0.0
    best_ratio = 0.0

    try:
        while True:
            return_code = process.poll()
            now = time.time()
            should_log = (return_code is not None) or ((now - last_log_time) >= 1.0)
            if should_log:
                dumped_bytes = output_path.stat().st_size if output_path.exists() else 0
                read_bytes = get_process_read_bytes(process_info)
                if return_code is not None:
                    best_ratio = 1.0
                elif input_size > 0 and read_bytes > 0:
                    ratio = min(0.999, read_bytes / input_size)
                    best_ratio = max(best_ratio, ratio)
                log_input_scan_progress(
                    label="Subtitle scan",
                    start_time=extract_start,
                    progress_ratio=best_ratio,
                    input_size=input_size,
                    read_bytes=read_bytes,
                    dumped_bytes=dumped_bytes,
                    total_duration=analyze_duration,
                    quiet=quiet,
                )
                last_log_time = now
            if return_code is not None:
                break
            time.sleep(0.2)
    except KeyboardInterrupt:
        stop_ffmpeg_process(process, label="Subtitle packet dump", quiet=quiet)
        cleanup_partial_file(output_path)
        raise
    finally:
        if stderr_thread is not None:
            stderr_thread.join(timeout=1.0)

    return_code = process.wait()
    if return_code != 0:
        cleanup_partial_file(output_path)
        raise RuntimeError("\n".join(stderr_lines[-20:]).strip() or "bitmap subtitle packet dump failed")


def pgs_segment_be16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset:offset + 2], "big")


def pgs_segment_be24(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset:offset + 3], "big")


def parse_pgs_object_visible_bounds(
    *,
    width: int,
    height: int,
    rle_data: bytes,
    palette_alpha: dict[int, int],
) -> tuple[int, int, int, int] | None:
    if width <= 0 or height <= 0 or not rle_data:
        return None

    total_pixels = width * height
    pixel_count = 0
    cursor = 0
    min_x = width
    min_y = height
    max_x = -1
    max_y = -1

    while cursor < len(rle_data) and pixel_count < total_pixels:
        color = rle_data[cursor]
        cursor += 1
        run = 1

        if color == 0:
            if cursor >= len(rle_data):
                break
            flags = rle_data[cursor]
            cursor += 1
            run = flags & 0x3F
            if flags & 0x40:
                if cursor >= len(rle_data):
                    break
                run = (run << 8) + rle_data[cursor]
                cursor += 1
            if flags & 0x80:
                if cursor >= len(rle_data):
                    break
                color = rle_data[cursor]
                cursor += 1
            else:
                color = 0

        if run <= 0:
            continue

        run = min(run, total_pixels - pixel_count)
        alpha = palette_alpha.get(color, 255 if color != 0 else 0)
        if alpha > 0:
            remaining = run
            while remaining > 0:
                row = pixel_count // width
                col = pixel_count % width
                span = min(remaining, width - col)
                min_x = min(min_x, col)
                min_y = min(min_y, row)
                max_x = max(max_x, col + span - 1)
                max_y = max(max_y, row)
                pixel_count += span
                remaining -= span
        else:
            pixel_count += run

    if max_x < 0 or max_y < 0:
        return None
    return min_x, min_y, max_x + 1, max_y + 1


def current_pgs_display_bounds(
    *,
    presentation: dict[str, object] | None,
    objects: dict[int, dict[str, object]],
    palettes: dict[int, dict[int, int]],
) -> tuple[int, int, int, int] | None:
    if presentation is None:
        return None

    object_refs = presentation.get("objects")
    if not isinstance(object_refs, list) or not object_refs:
        return None

    palette_id = int(presentation.get("palette_id", 0))
    palette_alpha = palettes.get(palette_id, {})
    bounds: tuple[int, int, int, int] | None = None

    for object_ref in object_refs:
        if not isinstance(object_ref, dict):
            continue
        object_id = int(object_ref.get("id", -1))
        object_x = int(object_ref.get("x", 0))
        object_y = int(object_ref.get("y", 0))
        object_info = objects.get(object_id)
        if not object_info:
            continue

        width = int(object_info.get("w", 0))
        height = int(object_info.get("h", 0))
        rle_data = bytes(object_info.get("rle", b""))
        remaining = int(object_info.get("remaining", 0))
        local_bounds = parse_pgs_object_visible_bounds(
            width=width,
            height=height,
            rle_data=rle_data,
            palette_alpha=palette_alpha,
        )
        if local_bounds is None and width > 0 and height > 0 and remaining > 0:
            local_bounds = (0, 0, width, height)
        if local_bounds is None:
            continue

        x1 = object_x + local_bounds[0]
        y1 = object_y + local_bounds[1]
        x2 = object_x + local_bounds[2]
        y2 = object_y + local_bounds[3]
        if bounds is None:
            bounds = (x1, y1, x2, y2)
        else:
            bounds = (
                min(bounds[0], x1),
                min(bounds[1], y1),
                max(bounds[2], x2),
                max(bounds[3], y2),
            )

    return bounds


def analyze_bitmap_subtitle_segments_from_pgs(
    *,
    input_path: Path,
    stream_index: int,
    start: float,
    duration: float | None,
    analyze_duration: float | None,
    quiet: bool,
) -> list[BitmapSubtitleSegment]:
    with tempfile.TemporaryDirectory(prefix="nvp-pgs-") as temp_dir:
        data_path = Path(temp_dir) / f"track{stream_index}.bin"
        dump_bitmap_subtitle_stream_data(
            input_path=input_path,
            output_path=data_path,
            stream_index=stream_index,
            start=start,
            duration=duration,
            analyze_duration=analyze_duration,
            quiet=quiet,
        )
        data = data_path.read_bytes()

    log("Parsing bitmap subtitle placements...", quiet=quiet)
    objects: dict[int, dict[str, object]] = {}
    palettes: dict[int, dict[int, int]] = {}
    presentation: dict[str, object] | None = None
    segments: list[BitmapSubtitleSegment] = []
    cursor = 0
    segment_index = 0
    parse_start = time.time()
    last_log_time = parse_start

    while cursor + 3 <= len(data):
        segment_type = data[cursor]
        segment_length = pgs_segment_be16(data, cursor + 1)
        payload_start = cursor + 3
        payload_end = payload_start + segment_length
        if payload_end > len(data):
            raise RuntimeError("Truncated PGS segment while parsing extracted subtitle data.")
        payload = data[payload_start:payload_end]

        if segment_type == PGS_SEGMENT_PALETTE:
            if len(payload) >= 2:
                palette_id = payload[0]
                palette = palettes.setdefault(palette_id, {})
                for entry_offset in range(2, len(payload), 5):
                    if entry_offset + 5 > len(payload):
                        break
                    palette[payload[entry_offset]] = payload[entry_offset + 4]
        elif segment_type == PGS_SEGMENT_OBJECT:
            if len(payload) >= 4:
                object_id = pgs_segment_be16(payload, 0)
                sequence_desc = payload[3]
                object_info = objects.setdefault(object_id, {"w": 0, "h": 0, "rle": bytearray(), "remaining": 0})
                if sequence_desc & 0x80:
                    if len(payload) >= 11:
                        object_data_length = pgs_segment_be24(payload, 4)
                        object_width = pgs_segment_be16(payload, 7)
                        object_height = pgs_segment_be16(payload, 9)
                        rle_fragment = payload[11:]
                        object_info["w"] = object_width
                        object_info["h"] = object_height
                        object_info["rle"] = bytearray(rle_fragment)
                        expected_rle = max(0, object_data_length - 4)
                        object_info["remaining"] = max(0, expected_rle - len(rle_fragment))
                else:
                    object_rle = object_info.setdefault("rle", bytearray())
                    if isinstance(object_rle, bytearray):
                        object_rle.extend(payload[4:])
                    object_info["remaining"] = max(0, int(object_info.get("remaining", 0)) - max(0, len(payload) - 4))
        elif segment_type == PGS_SEGMENT_PRESENTATION:
            if len(payload) >= 11:
                state = payload[7] >> 6
                if state != 0:
                    objects.clear()
                    palettes.clear()
                object_count = payload[10]
                refs: list[dict[str, int]] = []
                offset = 11
                for _ in range(object_count):
                    if offset + 8 > len(payload):
                        break
                    composition_flag = payload[offset + 3]
                    refs.append({
                        "id": pgs_segment_be16(payload, offset),
                        "x": pgs_segment_be16(payload, offset + 4),
                        "y": pgs_segment_be16(payload, offset + 6),
                    })
                    offset += 8
                    if composition_flag & 0x80:
                        if offset + 8 > len(payload):
                            break
                        offset += 8
                presentation = {
                    "palette_id": payload[9],
                    "objects": refs,
                }
        elif segment_type == PGS_SEGMENT_DISPLAY:
            bounds = current_pgs_display_bounds(
                presentation=presentation,
                objects=objects,
                palettes=palettes,
            )
            if bounds is not None:
                segment_index += 1
                segments.append(BitmapSubtitleSegment(
                    start_s=float(segment_index),
                    end_s=float(segment_index + 1),
                    x=bounds[0],
                    y=bounds[1],
                    w=max(1, bounds[2] - bounds[0]),
                    h=max(1, bounds[3] - bounds[1]),
                ))

        cursor = payload_end
        now = time.time()
        if not quiet and (now - last_log_time >= 1.0 or cursor >= len(data)):
            ratio = (cursor / len(data)) if data else 1.0
            log(
                f"Subtitle parse {ratio * 100.0:5.1f}% | display sets {len(segments)} | "
                f"real {format_duration_hms(now - parse_start)}",
                quiet=False,
            )
            last_log_time = now

    return segments


def analyze_bitmap_subtitle_segments_via_ffmpeg_bbox(
    *,
    input_path: Path,
    stream_index: int,
    start: float,
    duration: float | None,
    analyze_duration: float | None,
    fallback_end: float,
    quiet: bool,
) -> list[BitmapSubtitleSegment]:
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    command = [ffmpeg, "-hide_banner", "-loglevel", "info", "-progress", "pipe:2", "-nostats"]
    if start > 0:
        command += ["-ss", f"{start:.3f}"]
    command += ["-i", str(input_path)]
    if duration is not None:
        command += ["-t", f"{duration:.3f}"]
    command += [
        "-filter_complex",
        f"[0:s:{stream_index}]bbox",
        "-an",
        "-f",
        "null",
        "-",
    ]
    segments: list[BitmapSubtitleSegment] = []
    active_start: float | None = None
    active_bounds: tuple[int, int, int, int] | None = None
    log("Analyzing bitmap subtitle regions for scaled burn-in...", quiet=quiet)
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    if process.stderr is None:
        raise RuntimeError("Failed to capture FFmpeg subtitle analysis output.")

    stderr_lines: list[str] = []
    progress_state: dict[str, str] = {}
    analysis_start = time.time()
    progress_estimator = ProgressEstimator()

    try:
        for raw_line in process.stderr:
            line = raw_line.strip()
            if not line:
                continue

            if "Parsed_bbox" in line:
                time_match = BBOX_TIME_RE.search(line)
                if time_match is not None:
                    pts_time = float(time_match.group("time"))
                    bbox_match = BBOX_VALUES_RE.search(line)
                    if bbox_match is None:
                        if active_start is not None and active_bounds is not None and pts_time > active_start:
                            segments.append(BitmapSubtitleSegment(
                                start_s=active_start,
                                end_s=pts_time,
                                x=active_bounds[0],
                                y=active_bounds[1],
                                w=active_bounds[2] - active_bounds[0],
                                h=active_bounds[3] - active_bounds[1],
                            ))
                        active_start = None
                        active_bounds = None
                    else:
                        x1 = int(bbox_match.group("x1"))
                        y1 = int(bbox_match.group("y1"))
                        x2 = x1 + int(bbox_match.group("w"))
                        y2 = y1 + int(bbox_match.group("h"))
                        bounds = (x1, y1, x2, y2)
                        if active_start is None:
                            active_start = pts_time
                            active_bounds = bounds
                        elif active_bounds is None:
                            active_bounds = bounds
                        else:
                            active_bounds = (
                                min(active_bounds[0], bounds[0]),
                                min(active_bounds[1], bounds[1]),
                                max(active_bounds[2], bounds[2]),
                                max(active_bounds[3], bounds[3]),
                            )

            if "=" not in line:
                stderr_lines.append(line)
                continue

            key, value = line.split("=", 1)
            progress_state[key] = value
            if key == "progress" and value in {"continue", "end"}:
                log_ffmpeg_progress(
                    label="Subtitle analysis",
                    progress_state=progress_state,
                    start_time=analysis_start,
                    total_duration=analyze_duration,
                    progress_estimator=progress_estimator,
                    quiet=quiet,
                    include_size=False,
                )
    except KeyboardInterrupt:
        stop_ffmpeg_process(process, label="Subtitle analysis", quiet=quiet)
        raise
    finally:
        if process.stderr is not None:
            process.stderr.close()

    return_code = process.wait()
    if return_code != 0:
        raise RuntimeError("\n".join(stderr_lines[-20:]).strip() or "bitmap subtitle analysis failed")

    if active_start is not None and active_bounds is not None and fallback_end > active_start:
        segments.append(BitmapSubtitleSegment(
            start_s=active_start,
            end_s=fallback_end,
            x=active_bounds[0],
            y=active_bounds[1],
            w=active_bounds[2] - active_bounds[0],
            h=active_bounds[3] - active_bounds[1],
        ))

    return [segment for segment in segments if segment.w > 0 and segment.h > 0 and segment.end_s > segment.start_s]


def analyze_bitmap_subtitle_segments(
    *,
    input_path: Path,
    stream_index: int,
    start: float,
    duration: float | None,
    analyze_duration: float | None,
    fallback_end: float,
    quiet: bool,
) -> list[BitmapSubtitleSegment]:
    cache_path = bitmap_subtitle_analysis_cache_path(
        input_path=input_path,
        stream_index=stream_index,
        start=start,
        duration=duration,
    )
    cached_segments = load_bitmap_subtitle_analysis_cache(cache_path)
    if cached_segments is not None:
        log(
            f"Loaded cached bitmap subtitle analysis ({len(cached_segments)} display set(s)).",
            quiet=quiet,
        )
        return cached_segments

    log("Analyzing bitmap subtitle regions for scaled burn-in...", quiet=quiet)
    try:
        segments = analyze_bitmap_subtitle_segments_from_pgs(
            input_path=input_path,
            stream_index=stream_index,
            start=start,
            duration=duration,
            analyze_duration=analyze_duration,
            quiet=quiet,
        )
        if segments:
            save_bitmap_subtitle_analysis_cache(cache_path, segments)
        return segments
    except KeyboardInterrupt:
        raise
    except Exception as error:
        log(
            f"Fast bitmap subtitle analysis failed ({error}). Falling back to FFmpeg bbox scan.",
            quiet=quiet,
        )

    segments = analyze_bitmap_subtitle_segments_via_ffmpeg_bbox(
        input_path=input_path,
        stream_index=stream_index,
        start=start,
        duration=duration,
        analyze_duration=analyze_duration,
        fallback_end=fallback_end,
        quiet=quiet,
    )
    if segments:
        save_bitmap_subtitle_analysis_cache(cache_path, segments)
    return segments


def scaled_bitmap_segment_rect(
    segment: BitmapSubtitleSegment,
    *,
    size_scale: float,
    frame_width: int,
    frame_height: int,
    subtitle_width: int,
    subtitle_height: int,
) -> tuple[int, int, int, int]:
    base_w = segment.w * frame_width / subtitle_width
    base_h = segment.h * frame_height / subtitle_height
    base_x = segment.x * frame_width / subtitle_width
    base_y = segment.y * frame_height / subtitle_height
    scaled_w = max(1, min(frame_width, int(round(base_w * size_scale))))
    scaled_h = max(1, min(frame_height, int(round(base_h * size_scale))))
    x = int(round(base_x - ((scaled_w - base_w) / 2.0)))
    y = int(round(base_y - ((scaled_h - base_h) / 2.0)))
    x = max(0, min(frame_width - scaled_w, x))
    y = max(0, min(frame_height - scaled_h, y))
    return scaled_w, scaled_h, x, y


def padded_bitmap_segment(
    segment: BitmapSubtitleSegment,
    *,
    subtitle_width: int,
    subtitle_height: int,
    padding: int = BITMAP_SUBTITLE_BBOX_PADDING,
) -> BitmapSubtitleSegment:
    if padding <= 0:
        return segment
    x1 = max(0, segment.x - padding)
    y1 = max(0, segment.y - padding)
    x2 = min(subtitle_width, segment.x + segment.w + padding)
    y2 = min(subtitle_height, segment.y + segment.h + padding)
    return BitmapSubtitleSegment(
        start_s=segment.start_s,
        end_s=segment.end_s,
        x=x1,
        y=y1,
        w=max(1, x2 - x1),
        h=max(1, y2 - y1),
    )


def merge_bitmap_subtitle_regions(
    segments: list[BitmapSubtitleSegment],
    *,
    subtitle_width: int,
    subtitle_height: int,
) -> list[BitmapSubtitleSegment]:
    regions: list[list[int]] = []
    for segment in segments:
        padded = padded_bitmap_segment(
            segment,
            subtitle_width=subtitle_width,
            subtitle_height=subtitle_height,
        )
        x1 = padded.x
        y1 = padded.y
        x2 = padded.x + padded.w
        y2 = padded.y + padded.h
        merged = False
        for region in regions:
            rx1, ry1, rx2, ry2 = region
            if not (x2 < rx1 or rx2 < x1 or y2 < ry1 or ry2 < y1):
                region[0] = min(rx1, x1)
                region[1] = min(ry1, y1)
                region[2] = max(rx2, x2)
                region[3] = max(ry2, y2)
                merged = True
                break
        if not merged:
            regions.append([x1, y1, x2, y2])

    changed = True
    while changed:
        changed = False
        next_regions: list[list[int]] = []
        while regions:
            current = regions.pop()
            merged_any = False
            for other in regions:
                if not (current[2] < other[0] or other[2] < current[0] or current[3] < other[1] or other[3] < current[1]):
                    other[0] = min(other[0], current[0])
                    other[1] = min(other[1], current[1])
                    other[2] = max(other[2], current[2])
                    other[3] = max(other[3], current[3])
                    merged_any = True
                    changed = True
                    break
            if not merged_any:
                next_regions.append(current)
        regions = next_regions

    merged_segments = [
        BitmapSubtitleSegment(
            start_s=0.0,
            end_s=0.0,
            x=x1,
            y=y1,
            w=max(1, x2 - x1),
            h=max(1, y2 - y1),
        )
        for x1, y1, x2, y2 in sorted(regions, key=lambda rect: (rect[1], rect[0]))
    ]
    return merged_segments


def preview_mp4_path_for_output(output_path: Path) -> Path:
    return output_path.with_suffix(".preview.mp4")


def write_preview_mp4(bitstream_path: Path, preview_path: Path, fps: float, *, quiet: bool) -> None:
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    preview_path.parent.mkdir(parents=True, exist_ok=True)
    process = subprocess.Popen(
        [
            ffmpeg,
            "-y",
            "-framerate",
            format_fps_value(fps),
            "-i",
            str(bitstream_path),
            "-c:v",
            "copy",
            "-an",
            "-movflags",
            "+faststart",
            str(preview_path),
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    try:
        _, stderr = process.communicate()
    except KeyboardInterrupt:
        stop_ffmpeg_process(process, label="Preview export", quiet=quiet)
        cleanup_partial_file(preview_path)
        raise
    if process.returncode != 0:
        raise RuntimeError((stderr or "").strip() or f"failed to write preview mp4: {preview_path}")


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


def subtitle_extract_suffix(codec_name: str | None) -> str:
    codec = (codec_name or "").strip().lower()
    if codec == "ass":
        return ".ass"
    if codec == "ssa":
        return ".ssa"
    return ".srt"


def resolve_burn_subtitle_source(
    input_path: Path,
    subtitle_arg: str | None,
    selected_tracks: list[int] | None,
) -> BurnSubtitleSource:
    if not subtitle_arg:
        raise RuntimeError("--burn-subtitles requires --subtitle.")

    if subtitle_arg != "embedded":
        subtitle_path = Path(subtitle_arg).resolve()
        return BurnSubtitleSource(
            kind="text_file",
            label=subtitle_path.name,
            path=subtitle_path,
        )

    available_tracks = probe_embedded_subtitle_tracks(input_path)
    if not available_tracks:
        raise RuntimeError("No embedded subtitle tracks were found.")

    if selected_tracks:
        wanted_tracks = list(dict.fromkeys(selected_tracks))
        if len(wanted_tracks) != 1:
            raise RuntimeError("--burn-subtitles only supports one embedded subtitle track. Pass exactly one --subtitle-track.")
        track_index = wanted_tracks[0]
        track_info_map = {track.ordinal: track for track in available_tracks}
        track_info = track_info_map.get(track_index)
        if track_info is None:
            raise RuntimeError(f"Embedded subtitle track {track_index} is not available.")
    else:
        track_info = available_tracks[0]

    return BurnSubtitleSource(
        kind="embedded_text" if track_info.text_supported else "embedded_bitmap",
        label=subtitle_track_debug_label(track_info),
        path=input_path.resolve(),
        stream_index=track_info.ordinal,
    )


def load_subtitle_tracks(
    input_path: Path,
    output_path: Path,
    subtitle_arg: str | None,
    selected_tracks: list[int] | None,
) -> list[SubtitleTrack]:
    if not subtitle_arg:
        return []

    if subtitle_arg != "embedded":
        subtitle_path = Path(subtitle_arg)
        cues = parse_subtitle_file(subtitle_path)
        if not cues:
            return []
        return [SubtitleTrack(name=subtitle_path.stem or "Subtitles", cues=cues)]

    available_tracks = probe_embedded_subtitle_tracks(input_path)
    if not available_tracks:
        raise RuntimeError("No embedded subtitle tracks were found.")

    if selected_tracks:
        wanted_tracks = list(dict.fromkeys(selected_tracks))
    else:
        wanted_tracks = [track.ordinal for track in available_tracks if track.text_supported]
        if not wanted_tracks:
            raise RuntimeError(
                "No supported text embedded subtitle tracks were found. "
                f"Available embedded subtitle tracks: {', '.join(subtitle_track_debug_label(track) for track in available_tracks)}. "
                "Use --burn-subtitles to hardcode bitmap subtitle tracks into the video."
            )
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
        extracted = output_path.parent / f"{output_path.stem}.track{track_index}{subtitle_extract_suffix(track_info.codec_name)}"
        try:
            extract_embedded_subtitle_track(input_path, extracted, track_index)
            cues = parse_subtitle_file(extracted)
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


def resolve_idr_frames(chunk_frames: int, requested_idr_frames: int | None, stream_profile: str) -> int:
    if stream_profile == "intra":
        return 1
    if chunk_frames <= 0:
        raise ValueError("chunk_frames must be greater than zero.")
    if requested_idr_frames is None:
        return min(chunk_frames, DEFAULT_MAX_IDR_FRAMES)
    if requested_idr_frames <= 0:
        raise ValueError("idr_frames must be greater than zero.")
    return min(chunk_frames, requested_idr_frames)


def h264_stream_profile_options(idr_frames: int, stream_profile: str) -> tuple[str | None, str, list[str]]:
    keyint = 1 if stream_profile == "intra" else idr_frames
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
        "no-deblock=1",
        "partitions=none",
        "me=dia",
        "subme=0",
        "no-8x8dct=1",
    ]

    if stream_profile == "fast":
        tune = "fastdecode"
        extra_params = [
            "aq-mode=1",
            "mbtree=1",
            "rc-lookahead=8",
            "sync-lookahead=8",
            "trellis=0",
        ]
    elif stream_profile == "balanced":
        tune = None
        extra_params = [
            "aq-mode=1",
            "mbtree=1",
            "rc-lookahead=12",
            "sync-lookahead=12",
            "trellis=0",
        ]
    elif stream_profile == "quality":
        tune = None
        extra_params = [
            "aq-mode=1",
            "mbtree=1",
            "rc-lookahead=20",
            "sync-lookahead=20",
            "trellis=1",
        ]
    elif stream_profile == "intra":
        tune = None
        extra_params = [
            "aq-mode=1",
            "mbtree=0",
            "rc-lookahead=0",
            "sync-lookahead=0",
            "trellis=1",
        ]
    else:
        raise ValueError(f"Unsupported stream profile: {stream_profile}")

    return tune, ":".join(base_params + extra_params), ["filter_units=remove_types=6"]


def build_ffmpeg_command(
    *,
    input_path: Path,
    output_path: Path,
    source_width: int,
    source_height: int,
    source_fps: float | None,
    width: int,
    height: int,
    fps: float,
    idr_frames: int,
    crf: float,
    preset: str,
    level: str,
    stream_profile: str,
    start: float,
    duration: float | None,
    encode_duration: float | None,
    burn_subtitle: BurnSubtitleSource | None,
    burn_subtitle_size: float,
    quiet: bool,
) -> tuple[list[str], str | None]:
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    post_vf = f"fps={format_fps_value(fps)},scale={width}:{height}:flags=lanczos,setsar=1"
    tune, x264_params, bitstream_filters = h264_stream_profile_options(idr_frames, stream_profile)
    subtitle_font_size, subtitle_margin_v, subtitle_outline = compute_burn_subtitle_metrics(height, burn_subtitle_size)
    command = [ffmpeg, "-y"]
    filter_complex_script: str | None = None
    if start > 0:
        command += ["-ss", f"{start:.3f}"]
    command += ["-i", str(input_path)]
    if duration is not None:
        command += ["-t", f"{duration:.3f}"]
    if burn_subtitle is None:
        command += [
            "-vf",
            post_vf,
            "-r",
            format_fps_value(fps),
            "-an",
            "-sn",
            "-dn",
        ]
    elif burn_subtitle.kind in {"text_file", "embedded_text"}:
        filter_parts = [f"subtitles=filename='{ffmpeg_filter_escape_path(burn_subtitle.path or input_path)}'"]
        if burn_subtitle.kind == "embedded_text" and burn_subtitle.stream_index is not None:
            filter_parts[0] += f":si={burn_subtitle.stream_index}"
        force_style = (
            f"FontSize={subtitle_font_size},"
            f"MarginV={subtitle_margin_v},"
            f"Alignment=2,"
            f"Outline={subtitle_outline},"
            "Shadow=0"
        )
        filter_parts[0] += f":force_style='{ffmpeg_filter_escape_value(force_style)}'"
        command += [
            "-vf",
            ",".join([post_vf] + filter_parts),
            "-r",
            format_fps_value(fps),
            "-an",
            "-dn",
        ]
    elif burn_subtitle.kind == "embedded_bitmap":
        if burn_subtitle.stream_index is None:
            raise RuntimeError("Embedded bitmap subtitle burn requested without a subtitle stream index.")
        if source_width <= 0 or source_height <= 0:
            raise RuntimeError("Bitmap subtitle burn requested without valid source dimensions.")
        if abs(burn_subtitle_size - 1.0) < 1e-6:
            bitmap_filter = (
                f"[0:s:{burn_subtitle.stream_index}]scale={width}:{height}:flags=lanczos,format=rgba[subfit];"
                f"[base][subfit]overlay=x=0:y=0:eof_action=pass[vout]"
            )
        else:
            segment_end = duration if duration is not None else 24.0 * 60.0 * 60.0
            analyzed_segments = analyze_bitmap_subtitle_segments(
                input_path=input_path,
                stream_index=burn_subtitle.stream_index,
                start=start,
                duration=duration,
                analyze_duration=encode_duration,
                fallback_end=segment_end,
                quiet=quiet,
            )
            if not analyzed_segments:
                bitmap_filter = (
                    f"[0:s:{burn_subtitle.stream_index}]scale={width}:{height}:flags=lanczos,format=rgba[subfit];"
                    f"[base][subfit]overlay=x=0:y=0:eof_action=pass[vout]"
                )
            else:
                regions = merge_bitmap_subtitle_regions(
                    analyzed_segments,
                    subtitle_width=source_width,
                    subtitle_height=source_height,
                )
                split_labels = [f"[subsrc{i}]" for i in range(len(regions))]
                bitmap_filter = (
                    f"[0:v]{post_vf}[base];"
                    f"[0:s:{burn_subtitle.stream_index}]format=rgba,"
                    f"split={len(regions)}{''.join(split_labels)};"
                )
                current_base = "[base]"
                for index, region in enumerate(regions):
                    scaled_w, scaled_h, overlay_x, overlay_y = scaled_bitmap_segment_rect(
                        region,
                        size_scale=burn_subtitle_size,
                        frame_width=width,
                        frame_height=height,
                        subtitle_width=source_width,
                        subtitle_height=source_height,
                    )
                    cropped_label = f"[subcrop{index}]"
                    next_base = "[vout]" if index == len(regions) - 1 else f"[base{index + 1}]"
                    bitmap_filter += (
                        f"{split_labels[index]}crop=w={region.w}:h={region.h}:x={region.x}:y={region.y}:exact=1,"
                        f"scale=w={scaled_w}:h={scaled_h}:flags=lanczos{cropped_label};"
                        f"{current_base}{cropped_label}overlay="
                        f"x={overlay_x}:y={overlay_y}:eof_action=pass{next_base};"
                    )
                    current_base = next_base
                filter_complex_script = bitmap_filter
        if filter_complex_script is None:
            filter_complex_script = f"[0:v]{post_vf}[base];" + bitmap_filter
        command += [
            "-filter_complex_script",
            FILTER_COMPLEX_SCRIPT_PLACEHOLDER,
            "-map",
            "[vout]",
            "-r",
            format_fps_value(fps),
            "-an",
            "-dn",
        ]
    else:
        raise RuntimeError(f"Unsupported burn subtitle mode: {burn_subtitle.kind}")
    command += [
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
        str(idr_frames),
        "-keyint_min",
        str(idr_frames),
        "-sc_threshold",
        "0",
        "-bf",
        "0",
        "-refs",
        "1",
        "-force_key_frames",
        f"expr:gte(n,n_forced*{idr_frames})",
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
    return command, filter_complex_script


def encode_h264_bitstream(
    *,
    input_path: Path,
    source_width: int,
    source_height: int,
    source_fps: float | None,
    width: int,
    height: int,
    fps: float,
    idr_frames: int,
    crf: float,
    preset: str,
    level: str,
    stream_profile: str,
    start: float,
    duration: float | None,
    encode_duration: float | None,
    burn_subtitle: BurnSubtitleSource | None,
    burn_subtitle_size: float,
    preview_output_path: Path | None,
    quiet: bool,
) -> bytes:
    with tempfile.TemporaryDirectory(prefix="nvp-h264-") as temp_dir:
        bitstream_path = Path(temp_dir) / "video.264"
        command, filter_complex_script = build_ffmpeg_command(
            input_path=input_path,
            output_path=bitstream_path,
            source_width=source_width,
            source_height=source_height,
            source_fps=source_fps,
            width=width,
            height=height,
            fps=fps,
            idr_frames=idr_frames,
            crf=crf,
            preset=preset,
            level=level,
            stream_profile=stream_profile,
            start=start,
            duration=duration,
            encode_duration=encode_duration,
            burn_subtitle=burn_subtitle,
            burn_subtitle_size=burn_subtitle_size,
            quiet=quiet,
        )
        if filter_complex_script is not None:
            filter_complex_script_path = Path(temp_dir) / "filter_complex.ffscript"
            filter_complex_script_path.write_text(filter_complex_script, encoding="utf-8")
            command = [
                str(filter_complex_script_path) if part == FILTER_COMPLEX_SCRIPT_PLACEHOLDER else part
                for part in command
            ]
        command = command[:1] + ["-hide_banner", "-loglevel", "error", "-progress", "pipe:2", "-nostats"] + command[1:]
        process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        if process.stderr is None:
            raise RuntimeError("Failed to capture FFmpeg progress output.")

        progress_state: dict[str, str] = {}
        stderr_lines: list[str] = []
        encode_start = time.time()
        progress_estimator = ProgressEstimator()
        try:
            for raw_line in process.stderr:
                line = raw_line.strip()
                if not line:
                    continue
                if "=" not in line:
                    stderr_lines.append(line)
                    continue

                key, value = line.split("=", 1)
                progress_state[key] = value
                if key == "progress" and value in {"continue", "end"}:
                    log_ffmpeg_progress(
                        label="FFmpeg",
                        progress_state=progress_state,
                        start_time=encode_start,
                        total_duration=encode_duration,
                        progress_estimator=progress_estimator,
                        quiet=quiet,
                        include_size=True,
                    )
        except KeyboardInterrupt:
            stop_ffmpeg_process(process, label="Encoding", quiet=quiet)
            raise
        finally:
            if process.stderr is not None:
                process.stderr.close()

        return_code = process.wait()
        if return_code != 0:
            raise RuntimeError("\n".join(stderr_lines[-20:]).strip() or "ffmpeg H.264 encoding failed")
        if preview_output_path is not None:
            write_preview_mp4(bitstream_path, preview_output_path, fps, quiet=quiet)
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


def chunk_has_independent_start(unit: AccessUnit) -> bool:
    return unit.contains_type(NAL_IDR) and unit.contains_type(NAL_SPS) and unit.contains_type(NAL_PPS)


def align4(value: int) -> int:
    return (value + 3) & ~3


def access_unit_payload_size(unit: AccessUnit, *, keep_parameter_sets: bool) -> int:
    size = 0
    for nal in unit.nal_units:
        if nal.nal_type == NAL_AUD:
            continue
        if not keep_parameter_sets and nal.nal_type in {NAL_SPS, NAL_PPS, NAL_SEI}:
            continue
        size += len(nal.data)
    return size


def estimate_chunk_blob_size(access_units: list[AccessUnit], stream_profile: str) -> int:
    payload_size = 0
    for index, unit in enumerate(access_units):
        payload_size += access_unit_payload_size(
            unit,
            keep_parameter_sets=(index == 0 or stream_profile != "intra"),
        )
    return align4(4 + (len(access_units) * 4) + payload_size)


def estimate_subtitle_storage_size(subtitle_tracks: list[SubtitleTrack]) -> int:
    if not subtitle_tracks:
        return 0

    size = 2
    for track in subtitle_tracks:
        encoded_name = sanitize_subtitle_text(track.name).encode("ascii", "replace")
        size += struct.calcsize("<HI") + len(encoded_name)
    for track in subtitle_tracks:
        for cue in track.cues:
            encoded = sanitize_subtitle_text(cue.text).encode("ascii", "replace")
            size += struct.calcsize("<IIHBBHHHHH") + len(encoded)
    return size


def estimate_total_output_size(
    chunks: list[list[AccessUnit]],
    *,
    stream_profile: str,
    subtitle_tracks: list[SubtitleTrack],
) -> int:
    chunk_bytes = sum(estimate_chunk_blob_size(chunk, stream_profile) for chunk in chunks)
    return HEADER_STRUCT.size + chunk_bytes + (len(chunks) * CHUNK_INDEX_STRUCT.size) + estimate_subtitle_storage_size(subtitle_tracks)


def split_access_units_into_segments(access_units: list[AccessUnit], stream_profile: str) -> list[ChunkSegment]:
    segments: list[ChunkSegment] = []
    current: list[AccessUnit] = []
    current_first_frame = 0

    for frame_index, unit in enumerate(access_units):
        starts_independent = (stream_profile == "intra") or chunk_has_independent_start(unit)
        if current and starts_independent:
            segments.append(ChunkSegment(
                first_frame=current_first_frame,
                access_units=current,
                blob_size=estimate_chunk_blob_size(current, stream_profile),
            ))
            current = []
        if not current:
            current_first_frame = frame_index
        current.append(unit)

    if current:
        segments.append(ChunkSegment(
            first_frame=current_first_frame,
            access_units=current,
            blob_size=estimate_chunk_blob_size(current, stream_profile),
        ))

    return segments


def group_access_units_into_chunks(
    access_units: list[AccessUnit],
    chunk_frames: int,
    max_chunk_bytes: int | None,
    stream_profile: str,
) -> list[list[AccessUnit]]:
    chunks: list[list[AccessUnit]] = []
    current: list[AccessUnit] = []
    segments = split_access_units_into_segments(access_units, stream_profile)

    if not segments:
        raise RuntimeError("The H.264 bitstream did not produce any `.nvp` chunks.")

    for segment in segments:
        if len(segment.access_units) > chunk_frames:
            raise RuntimeError(
                f"GOP starting at frame {segment.first_frame} spans {len(segment.access_units)} frames, "
                f"which exceeds --chunk-frames={chunk_frames}. Lower --idr-frames or raise the chunk frame limit."
            )
        if max_chunk_bytes and segment.blob_size > max_chunk_bytes:
            raise RuntimeError(
                f"GOP starting at frame {segment.first_frame} stores to {segment.blob_size / 1024:.1f} KiB, "
                f"which exceeds --max-chunk-kib={max_chunk_bytes // 1024}. Lower --idr-frames or raise the chunk size limit."
            )
        if current:
            candidate = current + segment.access_units
            candidate_size = estimate_chunk_blob_size(candidate, stream_profile)
            if len(candidate) > chunk_frames or (max_chunk_bytes and candidate_size > max_chunk_bytes):
                chunks.append(current)
                current = list(segment.access_units)
            else:
                current = candidate
        else:
            current = list(segment.access_units)

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
        if len(chunk) > chunk_frames:
            raise RuntimeError(
                f"Chunk {index} has {len(chunk)} frames, which exceeds chunk_frames={chunk_frames}."
            )
        if max_chunk_bytes and estimate_chunk_blob_size(chunk, stream_profile) > max_chunk_bytes:
            raise RuntimeError(
                f"Chunk {index} exceeds --max-chunk-kib={max_chunk_bytes // 1024} even after regrouping. "
                "Lower --idr-frames or increase the chunk size limit."
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
    preview_output_path = preview_mp4_path_for_output(output_path) if args.preview_mp4 else None
    output_started = False
    stats_started = False

    try:
        burn_subtitle = resolve_burn_subtitle_source(input_path, args.subtitle, args.subtitle_track) if args.burn_subtitles else None
        subtitle_tracks = [] if args.burn_subtitles else load_subtitle_tracks(input_path, output_path, args.subtitle, args.subtitle_track)
        subtitle_count = sum(len(track.cues) for track in subtitle_tracks)
        if subtitle_tracks and args.subtitle == "embedded":
            log(f"Embedding {len(subtitle_tracks)} subtitle track(s).", quiet=args.quiet)
        if burn_subtitle is not None:
            log(f"Burning subtitles into video: {burn_subtitle.label}", quiet=args.quiet)

        video_probe = probe_video(input_path)
        source_width = video_probe.display_width or video_probe.storage_width
        source_height = video_probe.display_height or video_probe.storage_height
        if source_width <= 0 or source_height <= 0:
            raise RuntimeError("Failed to determine input video dimensions.")

        fps = video_probe.fps if isinstance(args.fps, str) and args.fps.lower() == "source" else float(args.fps)
        if fps <= 0:
            raise RuntimeError("Target fps must be greater than zero.")
        if args.burn_subtitle_size <= 0:
            raise RuntimeError("--burn-subtitle-size must be greater than zero.")

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
        idr_frames = resolve_idr_frames(args.chunk_frames, args.idr_frames, args.stream_profile)
        bitstream = encode_h264_bitstream(
            input_path=input_path,
            source_width=video_probe.storage_width,
            source_height=video_probe.storage_height,
            source_fps=video_probe.fps,
            width=target_width,
            height=target_height,
            fps=fps,
            idr_frames=idr_frames,
            crf=args.crf,
            preset=args.preset,
            level=args.level,
            stream_profile=args.stream_profile,
            start=args.start,
            duration=args.duration,
            encode_duration=args.duration if args.duration is not None else max(0.0, video_probe.duration - args.start),
            burn_subtitle=burn_subtitle,
            burn_subtitle_size=args.burn_subtitle_size,
            preview_output_path=preview_output_path,
            quiet=args.quiet,
        )
        log(
            f"FFmpeg produced {len(bitstream) / 1024:.1f} KiB of Annex B H.264 in {time.time() - start_time:.1f}s "
            f"(IDR every {idr_frames} frame(s)).",
            quiet=args.quiet,
        )

        nal_units = parse_annex_b_nalus(bitstream)
        access_units = group_nals_into_access_units(nal_units)
        chunks = group_access_units_into_chunks(
            access_units,
            args.chunk_frames,
            (args.max_chunk_kib * 1024) if args.max_chunk_kib > 0 else None,
            args.stream_profile,
        )

        if not access_units:
            raise RuntimeError("No frames were found in the encoded H.264 bitstream.")

        expected_output_size = estimate_total_output_size(
            chunks,
            stream_profile=args.stream_profile,
            subtitle_tracks=subtitle_tracks,
        )
        log(
            f"Expected output size {format_binary_size(expected_output_size)} "
            f"({len(chunks)} chunks, {len(access_units)} frames).",
            quiet=args.quiet,
        )

        with output_path.open("wb") as output_handle:
            output_started = True
            output_handle.write(b"\0" * HEADER_STRUCT.size)

            chunk_index: list[tuple[int, int, int, int, int, int]] = []
            frame_cursor = 0
            pack_start_time = time.time()
            for chunk_number, access_unit_chunk in enumerate(chunks, start=1):
                chunk_payload, frame_offsets = build_chunk_blob(access_unit_chunk, args.stream_profile)
                stored_chunk_blob = bytearray()
                stored_chunk_blob += struct.pack("<I", len(chunk_payload))
                for frame_offset in frame_offsets:
                    stored_chunk_blob += struct.pack("<I", frame_offset)
                stored_chunk_blob += chunk_payload
                while len(stored_chunk_blob) % 4:
                    stored_chunk_blob.append(0)

                stored_blob = bytes(stored_chunk_blob)
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
                pack_elapsed = time.time() - pack_start_time
                frame_end = frame_cursor + len(access_unit_chunk) - 1
                progress = (chunk_number / len(chunks)) * 100.0
                written_size = output_handle.tell()
                remaining_size = max(0, expected_output_size - written_size)
                pack_rate = ((written_size - HEADER_STRUCT.size) / pack_elapsed) if pack_elapsed > 0.0 else 0.0
                eta = (remaining_size / pack_rate) if pack_rate > 0.0 and remaining_size > 0 else 0.0
                log(
                    f"Chunk {chunk_number:03d}: {progress:5.1f}% | ETA {format_duration_hms(eta)} | "
                    f"frames {frame_cursor}-{frame_end} | real {format_duration_hms(elapsed)} | "
                    f"size {format_binary_size(len(stored_blob))} | "
                    f"total size {format_binary_size(written_size)}/{format_binary_size(expected_output_size)}",
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
                        output_handle.write(
                            struct.pack(
                                "<IIHBBHHHHH",
                                cue.start_ms,
                                cue.end_ms,
                                len(encoded),
                                cue.position_mode,
                                cue.align,
                                cue.pos_x,
                                cue.pos_y,
                                cue.margin_l,
                                cue.margin_r,
                                cue.margin_v,
                            )
                        )
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
        with stats_path.open("w", encoding="utf-8") as stats_handle:
            stats_started = True
            json.dump(asdict(stats), stats_handle, indent=2)
        log(
            f"Wrote {output_path.name}: {bytes_written / (1024 * 1024):.2f} MiB | "
            f"{len(access_units)} frames | {len(chunks)} chunks | raw H.264 {len(bitstream) / 1024:.1f} KiB",
            quiet=args.quiet,
        )
        if preview_output_path is not None:
            log(f"Wrote {preview_output_path.name} for preview.", quiet=args.quiet)
        return stats
    except KeyboardInterrupt:
        if output_started:
            cleanup_partial_file(output_path)
        if stats_started:
            cleanup_partial_file(stats_path)
        raise


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="Input video file")
    parser.add_argument("--output", required=True, help="Output .nvp.tns file")
    parser.add_argument("--subtitle", help="Optional subtitle source path or 'embedded'")
    parser.add_argument("--burn-subtitles", action="store_true", help="Render the selected subtitles directly into the video instead of storing subtitle tracks in the container")
    parser.add_argument("--burn-subtitle-size", type=float, default=DEFAULT_BURN_SUBTITLE_SIZE, help="Relative size multiplier for burned subtitles (1.0 = default output-safe size)")
    parser.add_argument("--subtitle-track", dest="subtitle_track", action="append", type=int, help="Embedded subtitle track index to include; repeat to keep multiple tracks")
    parser.add_argument("--fps", default="12", help="Target framerate or 'source'")
    parser.add_argument("--max-width", type=int, default=SCREEN_W, help="Fit width")
    parser.add_argument("--max-height", type=int, default=SCREEN_H, help="Fit height")
    parser.add_argument("--chunk-frames", type=int, default=48, help="Maximum frames per streamed chunk")
    parser.add_argument("--idr-frames", type=int, help="Maximum frames between forced IDR access units; defaults to min(chunk-frames, 24)")
    parser.add_argument("--max-chunk-kib", type=int, default=DEFAULT_MAX_CHUNK_KIB, help="Maximum stored chunk size target in KiB; 0 disables the byte cap")
    parser.add_argument("--crf", type=float, default=24.0, help="libx264 CRF quality target (fractional values allowed)")
    parser.add_argument("--preset", default="slow", help="libx264 preset")
    parser.add_argument("--level", default="1.3", help="Target H.264 level")
    parser.add_argument("--stream-profile", choices=STREAM_PROFILES, default="fast", help="Decoder-complexity profile: fast is smoothest, balanced/quality trade more CPU for better image quality")
    parser.add_argument("--start", type=float, default=0.0, help="Optional clip start offset in seconds")
    parser.add_argument("--duration", type=float, help="Optional clip duration in seconds")
    parser.add_argument("--quiet", action="store_true", help="Silence progress logging")
    parser.add_argument("--preview-mp4", action="store_true", help="Also write a video-only .preview.mp4 alongside the .nvp.tns output for quick inspection")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    try:
        encode(parse_args(argv or sys.argv[1:]))
    except KeyboardInterrupt:
        log("Encoding interrupted by Ctrl+C.", quiet=False)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
