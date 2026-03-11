#!/usr/bin/env python3
"""Encode a normal video file into a streamed Ndless-native movie container."""

from __future__ import annotations

import argparse
import html
import json
import math
import re
import struct
import subprocess
import sys
import time
import unicodedata
import zlib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterator

import imageio_ffmpeg
import numpy as np
from PIL import Image


MAGIC = b"NVP1"
VERSION = 8
SCREEN_W = 320
SCREEN_H = 240
HEADER_STRUCT = struct.Struct("<4sHHHHHHHHHHHHIIIII")
CHUNK_INDEX_STRUCT = struct.Struct("<IIIIII")
MOTION_ERROR_WEIGHT = 8
COLOR_ERROR_WEIGHT = 6
GREEN_ERROR_WEIGHT = 4
MOTION_VISUAL_ERROR_DIVISOR = 96
ROW_DIFF_GAP_LIMIT = 3
ROW_DIFF_FULL_ROW_RATIO = 0.34
ROW_DIFF_MAX_RUNS_PER_ROW = 24
SUBTITLE_LINE_BREAK_RE = re.compile(r"(?i)<br\s*/?>|\\N|\\n")
SUBTITLE_TAG_RE = re.compile(r"(?s)<[^>]+>")
SUBTITLE_ASS_OVERRIDE_RE = re.compile(r"\{\\[^}]*\}")


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
    keyframes: int
    predicted_frames: int
    repeated_frames: int
    block_updates: int
    motion_copies: int
    literal_blocks: int
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
    if not any(marker in text for marker in ("Ã", "â", "€", "™", "œ", "ž")):
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


def extract_embedded_subtitles(input_path: Path, output_path: Path) -> Path:
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    command = [
        ffmpeg,
        "-y",
        "-i",
        str(input_path),
        "-map",
        "0:s:0",
        str(output_path),
    ]
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr.strip() or "ffmpeg subtitle extraction failed")
    return output_path


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


def load_subtitle_tracks(input_path: Path, output_path: Path, subtitle_arg: str | None, selected_tracks: list[int] | None) -> list[SubtitleTrack]:
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


def fit_dimensions(source_width: int, source_height: int, max_width: int, max_height: int) -> tuple[int, int, int, int]:
    scale = min(max_width / source_width, max_height / source_height)
    width = max(1, int(round(source_width * scale)))
    height = max(1, int(round(source_height * scale)))
    x = (max_width - width) // 2
    y = (max_height - height) // 2
    return width, height, x, y


def probe_video(input_path: Path) -> VideoProbe:
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
            r"(?P<w>\d+)x(?P<h>\d+)(?:\s*\[SAR\s*(?P<sar_n>\d+):(?P<sar_d>\d+)\s*DAR\s*(?P<dar_n>\d+):(?P<dar_d>\d+)\])?",
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


def iter_video_frames(
    input_path: Path,
    *,
    width: int,
    height: int,
    fps: float,
    start: float = 0.0,
    duration: float | None = None,
) -> Iterator[dict | np.ndarray]:
    input_params: list[str] = []
    if start > 0:
        input_params += ["-ss", f"{start:.3f}"]
    if duration is not None:
        input_params += ["-t", f"{duration:.3f}"]
    generator = imageio_ffmpeg.read_frames(
        str(input_path),
        pix_fmt="rgb24",
        input_params=input_params,
        output_params=["-vf", f"fps={fps},scale=trunc(ih*dar/2)*2:ih,setsar=1,scale={width}:{height}:flags=lanczos"],
    )
    metadata = next(generator)
    yield metadata  # type: ignore[misc]
    for frame_bytes in generator:
        yield np.frombuffer(frame_bytes, dtype=np.uint8).reshape((height, width, 3))


def image_to_rgb565_words(frame: np.ndarray) -> np.ndarray:
    image = np.asarray(Image.fromarray(frame, mode="RGB"), dtype=np.uint8)
    red = (image[:, :, 0].astype(np.uint16) >> 3) << 11
    green = (image[:, :, 1].astype(np.uint16) >> 2) << 5
    blue = image[:, :, 2].astype(np.uint16) >> 3
    return (red | green | blue).astype("<u2")


def preprocess_frame(frame: np.ndarray, posterize_bits: int) -> np.ndarray:
    if posterize_bits <= 0 or posterize_bits >= 8:
        return frame
    levels = (1 << posterize_bits) - 1
    image = frame.astype(np.uint16)
    quantized = ((image * levels + 127) // 255)
    restored = (quantized * 255 + (levels // 2)) // levels
    return restored.astype(np.uint8)


def rgb565_block_metrics(current_block: np.ndarray, reference_block: np.ndarray) -> tuple[int, int, float]:
    current_words = np.asarray(current_block, dtype=np.uint16)
    reference_words = np.asarray(reference_block, dtype=np.uint16)
    current_red = ((current_words >> 11) & 0x1F).astype(np.int16)
    current_green = ((current_words >> 5) & 0x3F).astype(np.int16)
    current_blue = (current_words & 0x1F).astype(np.int16)
    reference_red = ((reference_words >> 11) & 0x1F).astype(np.int16)
    reference_green = ((reference_words >> 5) & 0x3F).astype(np.int16)
    reference_blue = (reference_words & 0x1F).astype(np.int16)
    red_diff = np.abs(current_red - reference_red)
    green_diff = np.abs(current_green - reference_green)
    blue_diff = np.abs(current_blue - reference_blue)
    changed_pixels = int(np.count_nonzero((red_diff + blue_diff + (green_diff // 2)) > 1))
    colorfulness = float(
        np.mean(
            np.maximum.reduce([current_red, current_green // 2, current_blue])
            - np.minimum.reduce([current_red, current_green // 2, current_blue])
        )
    ) / 31.0
    weighted_error = int(
        np.sum(red_diff * COLOR_ERROR_WEIGHT)
        + np.sum(green_diff * GREEN_ERROR_WEIGHT)
        + np.sum(blue_diff * COLOR_ERROR_WEIGHT)
    )
    return changed_pixels, weighted_error, colorfulness


def motion_penalty_from_error(changed_pixels: int, weighted_error: int, colorfulness: float) -> int:
    chroma_scale = 0.20 + min(1.0, max(0.0, colorfulness)) * 0.60
    visual_penalty = int((weighted_error * chroma_scale) // MOTION_VISUAL_ERROR_DIVISOR)
    return max(changed_pixels * MOTION_ERROR_WEIGHT, visual_penalty)


def rle16_encode(words: np.ndarray) -> bytes:
    flat = np.asarray(words, dtype="<u2").reshape(-1)
    output = bytearray()
    index = 0
    total = flat.size

    while index < total:
        repeat_len = 1
        value = flat[index]
        while index + repeat_len < total and flat[index + repeat_len] == value and repeat_len < 128:
            repeat_len += 1
        if repeat_len >= 3:
            output.append(0x80 | (repeat_len - 1))
            output += struct.pack("<H", int(value))
            index += repeat_len
            continue

        literal_start = index
        literal_len = 0
        while index < total and literal_len < 128:
            repeat_len = 1
            value = flat[index]
            while index + repeat_len < total and flat[index + repeat_len] == value and repeat_len < 128:
                repeat_len += 1
            if repeat_len >= 3 and literal_len > 0:
                break
            literal_len += 1
            index += 1
        output.append(literal_len - 1)
        output += flat[literal_start:literal_start + literal_len].tobytes()

    return bytes(output)


def build_row_runs(mask: np.ndarray, *, gap_limit: int, full_row_ratio: float) -> list[tuple[int, int]]:
    changed = np.flatnonzero(mask)
    if changed.size == 0:
        return []
    if changed.size >= int(mask.size * full_row_ratio):
        return [(0, int(mask.size))]

    runs: list[tuple[int, int]] = []
    start = int(changed[0])
    end = start + 1

    for index in changed[1:]:
        column = int(index)
        if column <= end + gap_limit:
            end = column + 1
            continue
        runs.append((start, end))
        start = column
        end = column + 1
    runs.append((start, end))

    if len(runs) > ROW_DIFF_MAX_RUNS_PER_ROW:
        return [(0, int(mask.size))]
    return runs


def encode_row_diff_records(
    current_frame: np.ndarray,
    reference_frame: np.ndarray,
    *,
    gap_limit: int,
    full_row_ratio: float,
) -> tuple[bytes, int, int, int]:
    row_records: list[bytes] = []
    changed_rows = 0
    span_count = 0
    changed_pixels = 0

    for row_index in range(current_frame.shape[0]):
        current_row = current_frame[row_index]
        reference_row = reference_frame[row_index]
        mask = current_row != reference_row
        if not np.any(mask):
            continue
        runs = build_row_runs(mask, gap_limit=gap_limit, full_row_ratio=full_row_ratio)
        if not runs:
            continue
        row_blob = bytearray()
        row_blob += struct.pack("<BH", row_index, len(runs))
        changed_rows += 1
        span_count += len(runs)
        for x0, x1 in runs:
            payload = rle16_encode(current_row[x0:x1])
            row_blob += struct.pack("<HHH", x0, x1 - x0, len(payload))
            row_blob += payload
            changed_pixels += x1 - x0
        row_records.append(bytes(row_blob))

    return struct.pack("<H", changed_rows) + b"".join(row_records), changed_rows, span_count, changed_pixels


def find_motion_vector(
    current_frame: np.ndarray,
    previous_frame: np.ndarray,
    *,
    x0: int,
    y0: int,
    x1: int,
    y1: int,
    search_radius: int,
    search_step: int,
    error_ratio: float,
) -> tuple[int, int, int] | None:
    block = current_frame[y0:y1, x0:x1]
    block_h, block_w = block.shape
    best_dx = 0
    best_dy = 0
    best_error = block.size + 1
    max_error = int(block.size * error_ratio)

    if search_radius <= 0:
        return None

    for dy in range(-search_radius, search_radius + 1, search_step):
        src_y = y0 + dy
        if src_y < 0 or src_y + block_h > previous_frame.shape[0]:
            continue
        for dx in range(-search_radius, search_radius + 1, search_step):
            src_x = x0 + dx
            if (dx == 0 and dy == 0) or src_x < 0 or src_x + block_w > previous_frame.shape[1]:
                continue
            candidate = previous_frame[src_y:src_y + block_h, src_x:src_x + block_w]
            error = int(np.count_nonzero(block != candidate))
            if error < best_error:
                best_error = error
                best_dx = dx
                best_dy = dy
                if error == 0:
                    return best_dx, best_dy, 0
    if best_error <= max_error:
        return best_dx, best_dy, best_error
    return None


def find_global_motion_vector(
    current_frame: np.ndarray,
    previous_frame: np.ndarray,
    *,
    search_radius: int,
    search_step: int,
) -> tuple[int, int] | None:
    if search_radius <= 0:
        return None
    sample_step = 4
    sample_search_radius = max(1, search_radius // sample_step)
    sample_search_step = max(1, search_step // sample_step) if search_step > 1 else 1
    current_sample = current_frame[::sample_step, ::sample_step].astype(np.int32)
    previous_sample = previous_frame[::sample_step, ::sample_step].astype(np.int32)
    sample_h, sample_w = current_sample.shape
    baseline_error = float(np.count_nonzero(current_sample != previous_sample))
    best_dx = 0
    best_dy = 0
    best_error = baseline_error

    for dy in range(-sample_search_radius, sample_search_radius + 1, sample_search_step):
        if dy == 0:
            dest_y0 = 0
            src_y0 = 0
            overlap_h = sample_h
        elif dy > 0:
            dest_y0 = 0
            src_y0 = dy
            overlap_h = sample_h - dy
        else:
            dest_y0 = -dy
            src_y0 = 0
            overlap_h = sample_h + dy
        if overlap_h < 8:
            continue
        for dx in range(-sample_search_radius, sample_search_radius + 1, sample_search_step):
            if dx == 0 and dy == 0:
                continue
            if dx == 0:
                dest_x0 = 0
                src_x0 = 0
                overlap_w = sample_w
            elif dx > 0:
                dest_x0 = 0
                src_x0 = dx
                overlap_w = sample_w - dx
            else:
                dest_x0 = -dx
                src_x0 = 0
                overlap_w = sample_w + dx
            if overlap_w < 8:
                continue
            current_view = current_sample[dest_y0:dest_y0 + overlap_h, dest_x0:dest_x0 + overlap_w]
            previous_view = previous_sample[src_y0:src_y0 + overlap_h, src_x0:src_x0 + overlap_w]
            error = float(np.count_nonzero(current_view != previous_view))
            if error < best_error:
                best_error = error
                best_dx = dx
                best_dy = dy

    if best_dx == 0 and best_dy == 0:
        return None
    if baseline_error <= 0:
        return None
    if best_error >= baseline_error * 0.86:
        return None
    return best_dx * sample_step, best_dy * sample_step


def apply_global_motion_reference(previous_frame: np.ndarray, dx: int, dy: int) -> np.ndarray:
    frame_h, frame_w = previous_frame.shape
    shifted = previous_frame.copy()
    if dy == 0:
        dest_y0 = 0
        src_y0 = 0
        overlap_h = frame_h
    elif dy > 0:
        dest_y0 = 0
        src_y0 = dy
        overlap_h = frame_h - dy
    else:
        dest_y0 = -dy
        src_y0 = 0
        overlap_h = frame_h + dy
    if dx == 0:
        dest_x0 = 0
        src_x0 = 0
        overlap_w = frame_w
    elif dx > 0:
        dest_x0 = 0
        src_x0 = dx
        overlap_w = frame_w - dx
    else:
        dest_x0 = -dx
        src_x0 = 0
        overlap_w = frame_w + dx
    if overlap_h > 0 and overlap_w > 0:
        shifted[dest_y0:dest_y0 + overlap_h, dest_x0:dest_x0 + overlap_w] = previous_frame[
            src_y0:src_y0 + overlap_h,
            src_x0:src_x0 + overlap_w,
        ]
    return shifted


def split_block_regions(x0: int, y0: int, x1: int, y1: int) -> list[tuple[int, int, int, int, int]]:
    block_w = x1 - x0
    block_h = y1 - y0
    if block_w < 2 or block_h < 2:
        return []
    left_w = (block_w + 1) // 2
    right_w = block_w - left_w
    top_h = (block_h + 1) // 2
    bottom_h = block_h - top_h
    regions: list[tuple[int, int, int, int, int]] = []
    if left_w > 0 and top_h > 0:
        regions.append((0, x0, y0, x0 + left_w, y0 + top_h))
    if right_w > 0 and top_h > 0:
        regions.append((1, x0 + left_w, y0, x1, y0 + top_h))
    if left_w > 0 and bottom_h > 0:
        regions.append((2, x0, y0 + top_h, x0 + left_w, y1))
    if right_w > 0 and bottom_h > 0:
        regions.append((3, x0 + left_w, y0 + top_h, x1, y1))
    return regions


def encode_split_block(
    current_frame: np.ndarray,
    previous_frame: np.ndarray,
    *,
    bx: int,
    by: int,
    x0: int,
    y0: int,
    x1: int,
    y1: int,
    change_ratio: float,
    motion_search_radius: int,
    motion_search_step: int,
    motion_error_ratio: float,
) -> tuple[bytes, int, int, int] | None:
    records: list[bytes] = []
    changed_parts = 0
    motion_copies = 0
    literal_blocks = 0
    total_penalty = 0
    split_change_ratio = max(change_ratio * 0.65, 0.0)
    split_error_ratio = max(motion_error_ratio * 0.5, 0.0)
    split_search_radius = max(2, motion_search_radius // 2)

    for quarter_index, sx0, sy0, sx1, sy1 in split_block_regions(x0, y0, x1, y1):
        current_block = current_frame[sy0:sy1, sx0:sx1]
        previous_block = previous_frame[sy0:sy1, sx0:sx1]
        changed_pixels, _, block_colorfulness = rgb565_block_metrics(current_block, previous_block)
        if changed_pixels <= int(current_block.size * split_change_ratio):
            continue
        motion = find_motion_vector(
            current_frame,
            previous_frame,
            x0=sx0,
            y0=sy0,
            x1=sx1,
            y1=sy1,
            search_radius=split_search_radius,
            search_step=motion_search_step,
            error_ratio=split_error_ratio,
        )
        if motion is not None:
            motion_block = previous_frame[sy0 + motion[1]:sy1 + motion[1], sx0 + motion[0]:sx1 + motion[0]]
            motion_changed_pixels, motion_weighted_error, _ = rgb565_block_metrics(current_block, motion_block)
            records.append(struct.pack("<BBbb", quarter_index, 1, motion[0], motion[1]))
            motion_copies += 1
            total_penalty += motion_penalty_from_error(motion_changed_pixels, motion_weighted_error, block_colorfulness)
        else:
            payload = rle16_encode(current_block)
            records.append(struct.pack("<BBH", quarter_index, 0, len(payload)) + payload)
            literal_blocks += 1
        changed_parts += 1

    if changed_parts == 0:
        return None
    return struct.pack("<BBBB", 2, bx, by, changed_parts) + b"".join(records), motion_copies, literal_blocks, total_penalty


def encode_motion_records(
    current_frame: np.ndarray,
    reference_frame: np.ndarray,
    *,
    frame_width: int,
    frame_height: int,
    block_size: int,
    change_ratio: float,
    motion_search_radius: int,
    motion_search_step: int,
    motion_error_ratio: float,
) -> tuple[bytes, int, int, int]:
    grid_w = math.ceil(frame_width / block_size)
    grid_h = math.ceil(frame_height / block_size)
    record_bytes: list[bytes] = []
    changed_blocks = 0
    motion_copies_local = 0
    literal_blocks_local = 0

    for by in range(grid_h):
        for bx in range(grid_w):
            x0 = bx * block_size
            y0 = by * block_size
            x1 = min(x0 + block_size, frame_width)
            y1 = min(y0 + block_size, frame_height)
            current_block = current_frame[y0:y1, x0:x1]
            reference_block = reference_frame[y0:y1, x0:x1]
            changed_pixels, _, block_colorfulness = rgb565_block_metrics(current_block, reference_block)
            if changed_pixels <= int(current_block.size * change_ratio):
                continue
            motion = find_motion_vector(
                current_frame,
                reference_frame,
                x0=x0,
                y0=y0,
                x1=x1,
                y1=y1,
                search_radius=motion_search_radius,
                search_step=motion_search_step,
                error_ratio=motion_error_ratio,
            )
            payload = rle16_encode(current_block)
            literal_record = struct.pack("<BBBH", 0, bx, by, len(payload)) + payload
            best_record = literal_record
            best_score = len(literal_record)
            best_motion_copies = 0
            best_literal_blocks = 1

            if motion is not None:
                motion_block = reference_frame[y0 + motion[1]:y1 + motion[1], x0 + motion[0]:x1 + motion[0]]
                motion_changed_pixels, motion_weighted_error, _ = rgb565_block_metrics(current_block, motion_block)
                motion_record = struct.pack("<BBBbb", 1, bx, by, motion[0], motion[1])
                motion_score = len(motion_record) + motion_penalty_from_error(
                    motion_changed_pixels,
                    motion_weighted_error,
                    block_colorfulness,
                )
                if motion_score < best_score or (motion_score == best_score and len(motion_record) < len(best_record)):
                    best_record = motion_record
                    best_score = motion_score
                    best_motion_copies = 1
                    best_literal_blocks = 0

            split_result = encode_split_block(
                current_frame,
                reference_frame,
                bx=bx,
                by=by,
                x0=x0,
                y0=y0,
                x1=x1,
                y1=y1,
                change_ratio=change_ratio,
                motion_search_radius=motion_search_radius,
                motion_search_step=motion_search_step,
                motion_error_ratio=motion_error_ratio,
            )
            if split_result is not None:
                split_record, split_motion_copies, split_literal_count, split_penalty = split_result
                split_score = len(split_record) + split_penalty
                if split_score < best_score or (split_score == best_score and len(split_record) < len(best_record)):
                    best_record = split_record
                    best_score = split_score
                    best_motion_copies = split_motion_copies
                    best_literal_blocks = split_literal_count

            record_bytes.append(best_record)
            motion_copies_local += best_motion_copies
            literal_blocks_local += best_literal_blocks
            changed_blocks += 1

    return b"".join(record_bytes), changed_blocks, motion_copies_local, literal_blocks_local


def iter_chunks(items: list[np.ndarray], size: int) -> Iterator[list[np.ndarray]]:
    for start in range(0, len(items), size):
        yield items[start:start + size]


def encode_chunk(
    frames: list[np.ndarray],
    *,
    block_size: int,
    keyframe_interval: int,
    change_ratio: float,
    keyframe_block_ratio: float,
    motion_search_radius: int,
    motion_search_step: int,
    motion_error_ratio: float,
    frame_index_base: int = 0,
    initial_previous_frame: np.ndarray | None = None,
) -> tuple[bytes, list[int], int, int, int, int, int, int]:
    frame_height, frame_width = frames[0].shape
    previous_frame: np.ndarray | None = None if initial_previous_frame is None else initial_previous_frame.copy()
    encoded = bytearray()
    frame_offsets: list[int] = []
    keyframes = 0
    predicted = 0
    repeated = 0
    block_updates = 0
    motion_copies = 0
    literal_blocks = 0

    for local_index, frame_words in enumerate(frames):
        frame_offsets.append(len(encoded))
        full_payload = rle16_encode(frame_words)
        force_keyframe = previous_frame is None or ((frame_index_base + local_index) % keyframe_interval == 0)

        if not force_keyframe and previous_frame is not None:
            best_inter_blob: bytes | None = None
            best_changed_rows = 0
            best_global_shifts = 0
            best_literal_spans = 0

            gap_limit = max(1, min(6, int(round(motion_error_ratio * 32.0)) or 1))
            full_row_ratio = min(0.80, max(0.18, change_ratio * 4.0))

            predicted_payload, changed_rows, literal_spans_local, _ = encode_row_diff_records(
                frame_words,
                previous_frame,
                gap_limit=gap_limit,
                full_row_ratio=full_row_ratio,
            )
            if changed_rows == 0:
                best_inter_blob = b"N"
                best_changed_rows = 0
                best_global_shifts = 0
                best_literal_spans = 0
            elif len(predicted_payload) + 1 < len(full_payload):
                best_inter_blob = b"D" + predicted_payload
                best_changed_rows = changed_rows
                best_global_shifts = 0
                best_literal_spans = literal_spans_local

            global_motion = find_global_motion_vector(
                frame_words,
                previous_frame,
                search_radius=motion_search_radius,
                search_step=motion_search_step,
            )
            if global_motion is not None:
                shifted_reference = apply_global_motion_reference(previous_frame, global_motion[0], global_motion[1])
                global_payload, global_changed_rows, global_literal_spans, _ = encode_row_diff_records(
                    frame_words,
                    shifted_reference,
                    gap_limit=max(1, gap_limit - 1),
                    full_row_ratio=min(0.90, full_row_ratio + 0.10),
                )
                global_blob = b"H" + struct.pack("<bb", global_motion[0], global_motion[1]) + global_payload
                if len(global_blob) + 1 < len(full_payload) and (
                    best_inter_blob is None or len(global_blob) < len(best_inter_blob)
                ):
                    best_inter_blob = global_blob
                    best_changed_rows = global_changed_rows
                    best_global_shifts = 1
                    best_literal_spans = global_literal_spans

            if best_inter_blob is not None:
                encoded += best_inter_blob
                predicted += 1
                repeated += 1 if best_inter_blob == b"N" else 0
                block_updates += best_changed_rows
                motion_copies += best_global_shifts
                literal_blocks += best_literal_spans
                previous_frame = frame_words.copy()
                continue

        encoded += b"I" + struct.pack("<I", len(full_payload)) + full_payload
        keyframes += 1
        previous_frame = frame_words.copy()

    return bytes(encoded), frame_offsets, keyframes, predicted, repeated, block_updates, motion_copies, literal_blocks


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
    block_size: int,
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
            block_size,
            chunk_frames,
            frame_count,
            chunk_count,
            subtitle_count,
            index_offset,
            subtitle_offset,
        )
    )


def encode(args: argparse.Namespace) -> EncodeStats:
    input_path = Path(args.input).resolve()
    output_path = normalize_output_path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    stats_path = output_path.with_suffix(".json")

    subtitle_tracks = load_subtitle_tracks(input_path, output_path, args.subtitle, args.subtitle_track)
    subtitle_count = sum(len(track.cues) for track in subtitle_tracks)
    if subtitle_tracks and args.subtitle == "embedded":
        log(f"Embedding {len(subtitle_tracks)} subtitle track(s).", quiet=args.quiet)

    probe = iter_video_frames(input_path, width=16, height=16, fps=1.0)
    metadata = next(probe)
    if hasattr(probe, "close"):
        probe.close()
    video_probe = probe_video(input_path)
    source_width = video_probe.display_width or metadata["source_size"][0]
    source_height = video_probe.display_height or metadata["source_size"][1]
    duration = video_probe.duration or metadata.get("duration", 0.0) or 0.0
    fps = video_probe.fps if isinstance(args.fps, str) and args.fps.lower() == "source" else float(args.fps)
    if fps <= 0:
        raise RuntimeError("Target fps must be greater than zero.")
    target_width, target_height, video_x, video_y = fit_dimensions(
        source_width,
        source_height,
        args.max_width,
        args.max_height,
    )
    expected_frames = int(round((args.duration or duration) * fps)) if duration else 0

    log(
        f"Encoding {input_path.name} -> {target_width}x{target_height} @ {fps:.3f}fps "
        f"(source {video_probe.storage_width}x{video_probe.storage_height}, display {source_width}x{source_height})",
        quiet=args.quiet,
    )

    frame_stream = iter_video_frames(
        input_path,
        width=target_width,
        height=target_height,
        fps=fps,
        start=args.start,
        duration=args.duration,
    )
    next(frame_stream)

    output_handle = output_path.open("wb")
    output_handle.write(b"\0" * HEADER_STRUCT.size)

    chunk_index: list[tuple[int, int, int, int, int, int]] = []
    current_chunk_frames: list[np.ndarray] = []
    frame_count = 0
    chunk_count = 0
    keyframes = 0
    predicted = 0
    repeated = 0
    block_updates = 0
    motion_copies = 0
    literal_blocks = 0
    start_time = time.time()
    previous_chunk_last_frame: np.ndarray | None = None

    def flush_chunk() -> None:
        nonlocal chunk_count, keyframes, predicted, repeated, block_updates, motion_copies, literal_blocks
        nonlocal current_chunk_frames, previous_chunk_last_frame
        if not current_chunk_frames:
            return
        first_frame = frame_count - len(current_chunk_frames)
        prepared_frames = [
            image_to_rgb565_words(preprocess_frame(frame, args.posterize_bits))
            for frame in current_chunk_frames
        ]
        initial_previous_frame = None
        if previous_chunk_last_frame is not None:
            initial_previous_frame = image_to_rgb565_words(
                preprocess_frame(previous_chunk_last_frame, args.posterize_bits)
            )
        chunk_blob, frame_offsets, chunk_keyframes, chunk_predicted, chunk_repeated, chunk_blocks, chunk_motion, chunk_literal = encode_chunk(
            prepared_frames,
            block_size=args.block_size,
            keyframe_interval=args.keyframe_interval,
            change_ratio=args.change_ratio,
            keyframe_block_ratio=args.keyframe_block_ratio,
            motion_search_radius=args.motion_search_radius,
            motion_search_step=args.motion_search_step,
            motion_error_ratio=args.motion_error_ratio,
            frame_index_base=first_frame,
            initial_previous_frame=initial_previous_frame,
        )
        stored_chunk_blob = bytearray()
        stored_chunk_blob += struct.pack("<I", len(chunk_blob))
        for frame_offset in frame_offsets:
            stored_chunk_blob += struct.pack("<I", frame_offset)
        stored_chunk_blob += chunk_blob
        while len(stored_chunk_blob) % 4:
            stored_chunk_blob.append(0)
        stored_blob = compress_chunk_payload(bytes(stored_chunk_blob), args.zlib_level)
        offset = output_handle.tell()
        output_handle.write(stored_blob)
        chunk_index.append((offset, len(stored_blob), len(stored_chunk_blob), first_frame, len(current_chunk_frames), 0))
        total_size_bytes = output_handle.tell()
        chunk_count += 1
        keyframes += chunk_keyframes
        predicted += chunk_predicted
        repeated += chunk_repeated
        block_updates += chunk_blocks
        motion_copies += chunk_motion
        literal_blocks += chunk_literal
        elapsed = time.time() - start_time
        progress = min(100.0, (frame_count / expected_frames) * 100.0) if expected_frames else 0.0
        fps_done = frame_count / elapsed if elapsed > 0 else 0.0
        eta_frames = max(0, expected_frames - frame_count) if expected_frames else 0
        eta = (eta_frames / fps_done) if fps_done > 0 and eta_frames else 0.0
        log(
            f"Chunk {chunk_count:03d}: {progress:5.1f}% | ETA {format_duration_hms(eta)} | "
            f"frames {first_frame}-{frame_count - 1} | size {len(stored_blob) / 1024:.1f} KiB | "
            f"total size {total_size_bytes / (1024 * 1024):.2f} MiB | "
            f"I={chunk_keyframes} P={chunk_predicted - chunk_repeated} N={chunk_repeated} M={chunk_motion} L={chunk_literal}",
            quiet=args.quiet,
        )
        previous_chunk_last_frame = current_chunk_frames[-1].copy()
        current_chunk_frames = []

    for frame in frame_stream:
        current_chunk_frames.append(frame.copy())
        frame_count += 1
        if len(current_chunk_frames) >= args.chunk_frames:
            flush_chunk()

    flush_chunk()

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
        block_size=args.block_size,
        chunk_frames=args.chunk_frames,
        frame_count=frame_count,
        chunk_count=chunk_count,
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
        frame_count=frame_count,
        chunk_count=chunk_count,
        keyframes=keyframes,
        predicted_frames=predicted,
        repeated_frames=repeated,
        block_updates=block_updates,
        motion_copies=motion_copies,
        literal_blocks=literal_blocks,
        bytes_written=bytes_written,
        average_bytes_per_frame=bytes_written / frame_count,
    )
    stats_path.write_text(json.dumps(asdict(stats), indent=2), encoding="utf-8")
    log(
        f"Wrote {output_path.name}: {bytes_written / (1024 * 1024):.2f} MiB | "
        f"{frame_count} frames | {chunk_count} chunks | "
        f"{keyframes} I / {predicted - repeated} P / {repeated} N | "
        f"{block_updates} changed blocks | {motion_copies} motion copies | {literal_blocks} literal blocks",
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
    parser.add_argument("--block-size", type=int, default=16, help="Delta block size")
    parser.add_argument("--chunk-frames", type=int, default=24, help="Frames per streamed chunk")
    parser.add_argument("--keyframe-interval", type=int, default=48, help="Force a keyframe every N local frames")
    parser.add_argument("--change-ratio", type=float, default=0.05, help="Changed-pixel ratio per block to keep the block")
    parser.add_argument("--keyframe-block-ratio", type=float, default=0.42, help="Promote to keyframe if this fraction of blocks changed")
    parser.add_argument("--motion-search-radius", type=int, default=6, help="Pixel radius to search for block motion reuse")
    parser.add_argument("--motion-search-step", type=int, default=2, help="Pixel step when searching motion vectors")
    parser.add_argument("--motion-error-ratio", type=float, default=0.08, help="Allowed differing-pixel ratio for motion copied blocks")
    parser.add_argument("--posterize-bits", type=int, default=0, help="Optional 1-7 bit posterization per RGB channel before encoding")
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
