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
VERSION = 6
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
FRAME_RD_BYTES_WEIGHT = 20
FRAME_RD_EDGE_WEIGHT = 2
INTRA_CANDIDATE_BIAS = 4096
GLOBAL_MOTION_MIN_CONFIDENCE = 0.02
SCENE_CUT_FRAME_CHANGE_RATIO = 0.46
SCENE_CUT_HISTOGRAM_DELTA = 0.24
SCENE_CUT_MOTION_CONFIDENCE_MAX = 0.12
DRIFT_RESET_COST_PER_BLOCK = 220
EDGE_THRESHOLD = 12
FLATNESS_STDDEV_LIMIT = 24.0
SUBTITLE_LINE_BREAK_RE = re.compile(r"(?i)<br\s*/?>|\\N|\\n")
SUBTITLE_TAG_RE = re.compile(r"(?s)<[^>]+>")
SUBTITLE_ASS_OVERRIDE_RE = re.compile(r"\{\\[^}]*\}")


@dataclass(slots=True)
class SubtitleCue:
    start_ms: int
    end_ms: int
    text: str


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
    frame_type_counts: dict[str, int]
    frame_type_bytes: dict[str, int]
    scene_cuts: int
    drift_resets: int
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


@dataclass(slots=True)
class FrameAnalysis:
    block_change_ratio: np.ndarray
    block_edge_density: np.ndarray
    block_flatness: np.ndarray
    frame_change_ratio: float
    changed_block_ratio: float
    histogram_delta: float
    global_motion: tuple[int, int] | None
    global_motion_confidence: float


@dataclass(slots=True)
class FrameCandidate:
    tag: str
    blob: bytes
    reconstructed: np.ndarray
    changed_units: int
    motion_copies: int
    literal_blocks: int
    visual_penalty: int
    edge_penalty: int
    exact: bool


@dataclass(slots=True)
class ChunkEncodeResult:
    blob: bytes
    frame_offsets: list[int]
    keyframes: int
    predicted_frames: int
    repeated_frames: int
    block_updates: int
    motion_copies: int
    literal_blocks: int
    frame_type_counts: dict[str, int]
    frame_type_bytes: dict[str, int]
    scene_cuts: int
    drift_resets: int


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


def rgb565_luma(words: np.ndarray) -> np.ndarray:
    values = np.asarray(words, dtype=np.uint16)
    red = (((values >> 11) & 0x1F).astype(np.int16) << 3)
    green = (((values >> 5) & 0x3F).astype(np.int16) << 2)
    blue = ((values & 0x1F).astype(np.int16) << 3)
    return ((red * 3 + green * 4 + blue) // 8).astype(np.int16)


def block_edge_density(words: np.ndarray) -> float:
    if words.size <= 1:
        return 0.0
    luma = rgb565_luma(words)
    horizontal = np.abs(luma[:, 1:] - luma[:, :-1])
    vertical = np.abs(luma[1:, :] - luma[:-1, :])
    total_edges = horizontal.size + vertical.size
    if total_edges <= 0:
        return 0.0
    edge_hits = int(np.count_nonzero(horizontal >= EDGE_THRESHOLD) + np.count_nonzero(vertical >= EDGE_THRESHOLD))
    return edge_hits / float(total_edges)


def block_flatness(words: np.ndarray) -> float:
    if words.size <= 1:
        return 1.0
    stddev = float(np.std(rgb565_luma(words)))
    return max(0.0, 1.0 - min(1.0, stddev / FLATNESS_STDDEV_LIMIT))


def frame_histogram_delta(current_frame: np.ndarray, reference_frame: np.ndarray) -> float:
    current_hist = np.bincount((rgb565_luma(current_frame).reshape(-1) >> 4).astype(np.int16), minlength=16).astype(np.float32)
    reference_hist = np.bincount((rgb565_luma(reference_frame).reshape(-1) >> 4).astype(np.int16), minlength=16).astype(np.float32)
    current_hist /= max(1.0, float(current_hist.sum()))
    reference_hist /= max(1.0, float(reference_hist.sum()))
    return float(np.abs(current_hist - reference_hist).sum() * 0.5)


def animation_motion_penalties(
    changed_pixels: int,
    weighted_error: int,
    colorfulness: float,
    edge_density_score: float,
    flatness_score: float,
) -> tuple[int, int]:
    base_penalty = motion_penalty_from_error(changed_pixels, weighted_error, colorfulness)
    visual_penalty = int(base_penalty * max(0.60, 1.05 - flatness_score * 0.30))
    edge_penalty = int(base_penalty * edge_density_score * 2.40)
    return visual_penalty, edge_penalty


def candidate_cost(candidate: FrameCandidate, drift_score: int) -> int:
    drift_penalty = 0
    if not candidate.exact and drift_score > 0:
        drift_penalty = int((candidate.visual_penalty + candidate.edge_penalty) * min(1.2, drift_score / 18000.0))
    score = (
        len(candidate.blob) * FRAME_RD_BYTES_WEIGHT
        + candidate.visual_penalty
        + candidate.edge_penalty * FRAME_RD_EDGE_WEIGHT
        + drift_penalty
    )
    if candidate.tag == "I":
        score += INTRA_CANDIDATE_BIAS
    return score


def motion_penalty_from_error(changed_pixels: int, weighted_error: int, colorfulness: float) -> int:
    chroma_scale = 0.20 + min(1.0, max(0.0, colorfulness)) * 0.60
    visual_penalty = int((weighted_error * chroma_scale) // MOTION_VISUAL_ERROR_DIVISOR)
    return max(changed_pixels * MOTION_ERROR_WEIGHT, visual_penalty)


def pack_palette_indices(indices: list[int], bits_per_index: int) -> bytes:
    output = bytearray()
    accumulator = 0
    accumulator_bits = 0
    mask = (1 << bits_per_index) - 1

    for index in indices:
        accumulator |= (index & mask) << accumulator_bits
        accumulator_bits += bits_per_index
        while accumulator_bits >= 8:
            output.append(accumulator & 0xFF)
            accumulator >>= 8
            accumulator_bits -= 8
    if accumulator_bits:
        output.append(accumulator & 0xFF)
    return bytes(output)


def best_palette_run(flat: np.ndarray, start: int) -> tuple[int, list[int], bytes] | None:
    total = flat.size
    limit = min(total - start, 128)
    palette_map: dict[int, int] = {}
    palette: list[int] = []
    indices: list[int] = []
    best: tuple[int, list[int], bytes] | None = None
    best_saving = 0

    for length in range(1, limit + 1):
        value = int(flat[start + length - 1])
        if value not in palette_map:
            if len(palette) >= 16:
                break
            palette_map[value] = len(palette)
            palette.append(value)
        indices.append(palette_map[value])
        if length < 8 or len(palette) < 2:
            continue
        if (length & 7) != 0 and length != limit:
            continue
        bits_per_index = 1 if len(palette) <= 2 else 2 if len(palette) <= 4 else 4
        packed = pack_palette_indices(indices, bits_per_index)
        encoded_size = 1 + 1 + 2 + len(palette) * 2 + len(packed)
        raw_size = 1 + length * 2
        saving = raw_size - encoded_size
        if saving > best_saving:
            best_saving = saving
            best = (length, palette.copy(), packed)

    return best


def rle16_encode(words: np.ndarray) -> bytes:
    flat = np.asarray(words, dtype="<u2").reshape(-1)
    output = bytearray()
    index = 0
    total = flat.size

    while index < total:
        repeat_len = 1
        value = flat[index]
        while index + repeat_len < total and flat[index + repeat_len] == value and repeat_len < 127:
            repeat_len += 1
        if repeat_len >= 3:
            output.append(0x80 | (repeat_len - 1))
            output += struct.pack("<H", int(value))
            index += repeat_len
            continue

        palette_run = best_palette_run(flat, index)
        if palette_run is not None:
            run_length, palette, packed = palette_run
            output.append(0xFF)
            output.append(len(palette))
            output += struct.pack("<H", run_length)
            for palette_value in palette:
                output += struct.pack("<H", palette_value)
            output += packed
            index += run_length
            continue

        literal_start = index
        literal_len = 0
        while index < total and literal_len < 128:
            repeat_len = 1
            value = flat[index]
            while index + repeat_len < total and flat[index + repeat_len] == value and repeat_len < 127:
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
    if best_error <= max_error and (best_dx != 0 or best_dy != 0):
        for refine_dy in range(max(-search_step + 1, -1), min(search_step, 2)):
            dy = best_dy + refine_dy
            src_y = y0 + dy
            if src_y < 0 or src_y + block_h > previous_frame.shape[0]:
                continue
            for refine_dx in range(max(-search_step + 1, -1), min(search_step, 2)):
                dx = best_dx + refine_dx
                src_x = x0 + dx
                if (dx == 0 and dy == 0) or src_x < 0 or src_x + block_w > previous_frame.shape[1]:
                    continue
                candidate = previous_frame[src_y:src_y + block_h, src_x:src_x + block_w]
                error = int(np.count_nonzero(block != candidate))
                if error < best_error:
                    best_error = error
                    best_dx = dx
                    best_dy = dy
    if best_error <= max_error:
        return best_dx, best_dy, best_error
    return None


def estimate_global_motion(
    current_frame: np.ndarray,
    previous_frame: np.ndarray,
    *,
    search_radius: int,
    search_step: int,
) -> tuple[tuple[int, int] | None, float]:
    if search_radius <= 0:
        return None, 0.0
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
        return None, 0.0
    if baseline_error <= 0:
        return None, 0.0

    confidence = 1.0 - (best_error / baseline_error)
    if confidence <= 0:
        return None, 0.0
    if confidence < GLOBAL_MOTION_MIN_CONFIDENCE:
        return None, float(confidence)
    return (best_dx * sample_step, best_dy * sample_step), float(confidence)


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


def analyze_frame_transition(
    current_frame: np.ndarray,
    reference_frame: np.ndarray | None,
    *,
    block_size: int,
    motion_search_radius: int,
    motion_search_step: int,
) -> FrameAnalysis:
    frame_height, frame_width = current_frame.shape
    grid_w = math.ceil(frame_width / block_size)
    grid_h = math.ceil(frame_height / block_size)
    block_change_ratio = np.ones((grid_h, grid_w), dtype=np.float32)
    block_edge_density_map = np.zeros((grid_h, grid_w), dtype=np.float32)
    block_flatness_map = np.zeros((grid_h, grid_w), dtype=np.float32)

    total_changed_pixels = frame_width * frame_height if reference_frame is None else int(np.count_nonzero(current_frame != reference_frame))
    changed_blocks = 0

    for by in range(grid_h):
        for bx in range(grid_w):
            x0 = bx * block_size
            y0 = by * block_size
            x1 = min(x0 + block_size, frame_width)
            y1 = min(y0 + block_size, frame_height)
            current_block = current_frame[y0:y1, x0:x1]
            if reference_frame is None:
                change_ratio = 1.0
            else:
                change_ratio = float(np.count_nonzero(current_block != reference_frame[y0:y1, x0:x1])) / float(current_block.size)
            block_change_ratio[by, bx] = change_ratio
            if change_ratio > 0.0:
                changed_blocks += 1
            block_edge_density_map[by, bx] = block_edge_density(current_block)
            block_flatness_map[by, bx] = block_flatness(current_block)

    global_motion: tuple[int, int] | None = None
    global_motion_confidence = 0.0
    histogram_delta = 1.0
    if reference_frame is not None:
        global_motion, global_motion_confidence = estimate_global_motion(
            current_frame,
            reference_frame,
            search_radius=motion_search_radius,
            search_step=motion_search_step,
        )
        histogram_delta = frame_histogram_delta(current_frame, reference_frame)

    frame_change_ratio = total_changed_pixels / float(frame_width * frame_height)
    changed_block_ratio = changed_blocks / float(grid_w * grid_h)
    return FrameAnalysis(
        block_change_ratio=block_change_ratio,
        block_edge_density=block_edge_density_map,
        block_flatness=block_flatness_map,
        frame_change_ratio=frame_change_ratio,
        changed_block_ratio=changed_block_ratio,
        histogram_delta=histogram_delta,
        global_motion=global_motion,
        global_motion_confidence=global_motion_confidence,
    )


def is_scene_cut(analysis: FrameAnalysis, keyframe_block_ratio: float) -> bool:
    return (
        analysis.changed_block_ratio >= keyframe_block_ratio
        and analysis.frame_change_ratio >= SCENE_CUT_FRAME_CHANGE_RATIO
        and analysis.histogram_delta >= SCENE_CUT_HISTOGRAM_DELTA
        and analysis.global_motion_confidence <= SCENE_CUT_MOTION_CONFIDENCE_MAX
    )


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
    reference_frame: np.ndarray,
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
) -> tuple[bytes, int, int, int, int, np.ndarray] | None:
    records: list[bytes] = []
    changed_parts = 0
    motion_copies = 0
    literal_blocks = 0
    total_visual_penalty = 0
    total_edge_penalty = 0
    split_change_ratio = max(change_ratio * 0.65, 0.0)
    split_error_ratio = max(motion_error_ratio * 0.5, 0.0)
    split_search_radius = max(2, motion_search_radius // 2)
    reconstructed = reference_frame[y0:y1, x0:x1].copy()

    for quarter_index, sx0, sy0, sx1, sy1 in split_block_regions(x0, y0, x1, y1):
        current_block = current_frame[sy0:sy1, sx0:sx1]
        reference_block = reference_frame[sy0:sy1, sx0:sx1]
        edge_density_score = block_edge_density(current_block)
        flatness_score = block_flatness(current_block)
        effective_change_ratio = min(0.94, max(0.03, split_change_ratio * (0.95 + flatness_score * 0.65 - edge_density_score * 0.18)))
        effective_error_ratio = min(0.48, max(0.02, split_error_ratio * (1.35 + flatness_score * 0.80 - edge_density_score * 0.35)))
        changed_pixels, _, block_colorfulness = rgb565_block_metrics(current_block, reference_block)
        if changed_pixels <= int(current_block.size * effective_change_ratio):
            continue
        motion = find_motion_vector(
            current_frame,
            reference_frame,
            x0=sx0,
            y0=sy0,
            x1=sx1,
            y1=sy1,
            search_radius=split_search_radius + (1 if flatness_score > 0.60 else 0),
            search_step=motion_search_step,
            error_ratio=effective_error_ratio,
        )
        payload = rle16_encode(current_block)
        literal_record = struct.pack("<BBH", quarter_index, 0, len(payload)) + payload
        best_record = literal_record
        best_score = len(literal_record) * FRAME_RD_BYTES_WEIGHT
        best_reconstructed = current_block.copy()
        best_motion_copies = 0
        best_literal_blocks = 1
        best_visual_penalty = 0
        best_edge_penalty = 0

        if motion is not None:
            motion_block = reference_frame[sy0 + motion[1]:sy1 + motion[1], sx0 + motion[0]:sx1 + motion[0]]
            motion_changed_pixels, motion_weighted_error, _ = rgb565_block_metrics(current_block, motion_block)
            visual_penalty, edge_penalty = animation_motion_penalties(
                motion_changed_pixels,
                motion_weighted_error,
                block_colorfulness,
                edge_density_score,
                flatness_score,
            )
            motion_record = struct.pack("<BBbb", quarter_index, 1, motion[0], motion[1])
            motion_score = len(motion_record) * FRAME_RD_BYTES_WEIGHT + visual_penalty + edge_penalty * FRAME_RD_EDGE_WEIGHT
            if motion_score < best_score or (motion_score == best_score and len(motion_record) < len(best_record)):
                best_record = motion_record
                best_score = motion_score
                best_reconstructed = motion_block.copy()
                best_motion_copies = 1
                best_literal_blocks = 0
                best_visual_penalty = visual_penalty
                best_edge_penalty = edge_penalty

        records.append(best_record)
        reconstructed[sy0 - y0:sy1 - y0, sx0 - x0:sx1 - x0] = best_reconstructed
        motion_copies += best_motion_copies
        literal_blocks += best_literal_blocks
        total_visual_penalty += best_visual_penalty
        total_edge_penalty += best_edge_penalty
        changed_parts += 1

    if changed_parts == 0:
        return None
    return (
        struct.pack("<BBBB", 2, bx, by, changed_parts) + b"".join(records),
        motion_copies,
        literal_blocks,
        total_visual_penalty,
        total_edge_penalty,
        reconstructed,
    )


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
    analysis: FrameAnalysis,
) -> tuple[bytes, np.ndarray, int, int, int, int, int]:
    grid_w = math.ceil(frame_width / block_size)
    grid_h = math.ceil(frame_height / block_size)
    record_bytes: list[bytes] = []
    changed_blocks = 0
    motion_copies_local = 0
    literal_blocks_local = 0
    visual_penalty_total = 0
    edge_penalty_total = 0
    reconstructed = reference_frame.copy()

    for by in range(grid_h):
        for bx in range(grid_w):
            x0 = bx * block_size
            y0 = by * block_size
            x1 = min(x0 + block_size, frame_width)
            y1 = min(y0 + block_size, frame_height)
            current_block = current_frame[y0:y1, x0:x1]
            reference_block = reference_frame[y0:y1, x0:x1]
            edge_density_score = float(analysis.block_edge_density[by, bx])
            flatness_score = float(analysis.block_flatness[by, bx])
            effective_change_ratio = min(0.95, max(0.03, change_ratio * (0.96 + flatness_score * 0.72 - edge_density_score * 0.16)))
            effective_motion_error_ratio = min(0.52, max(0.02, motion_error_ratio * (1.40 + flatness_score * 0.90 - edge_density_score * 0.35)))
            effective_search_radius = motion_search_radius + (2 if flatness_score > 0.65 else 0)
            changed_pixels, _, block_colorfulness = rgb565_block_metrics(current_block, reference_block)
            if changed_pixels <= int(current_block.size * effective_change_ratio):
                continue
            motion = find_motion_vector(
                current_frame,
                reference_frame,
                x0=x0,
                y0=y0,
                x1=x1,
                y1=y1,
                search_radius=effective_search_radius,
                search_step=motion_search_step,
                error_ratio=effective_motion_error_ratio,
            )
            payload = rle16_encode(current_block)
            literal_record = struct.pack("<BBBH", 0, bx, by, len(payload)) + payload
            best_record = literal_record
            best_score = len(literal_record) * FRAME_RD_BYTES_WEIGHT
            best_reconstructed = current_block.copy()
            best_motion_copies = 0
            best_literal_blocks = 1
            best_visual_penalty = 0
            best_edge_penalty = 0

            if motion is not None:
                motion_block = reference_frame[y0 + motion[1]:y1 + motion[1], x0 + motion[0]:x1 + motion[0]]
                motion_changed_pixels, motion_weighted_error, _ = rgb565_block_metrics(current_block, motion_block)
                visual_penalty, edge_penalty = animation_motion_penalties(
                    motion_changed_pixels,
                    motion_weighted_error,
                    block_colorfulness,
                    edge_density_score,
                    flatness_score,
                )
                motion_record = struct.pack("<BBBbb", 1, bx, by, motion[0], motion[1])
                motion_score = len(motion_record) * FRAME_RD_BYTES_WEIGHT + visual_penalty + edge_penalty * FRAME_RD_EDGE_WEIGHT
                if motion_score < best_score or (motion_score == best_score and len(motion_record) < len(best_record)):
                    best_record = motion_record
                    best_score = motion_score
                    best_reconstructed = motion_block.copy()
                    best_motion_copies = 1
                    best_literal_blocks = 0
                    best_visual_penalty = visual_penalty
                    best_edge_penalty = edge_penalty

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
                split_record, split_motion_copies, split_literal_count, split_visual_penalty, split_edge_penalty, split_reconstructed = split_result
                split_score = len(split_record) * FRAME_RD_BYTES_WEIGHT + split_visual_penalty + split_edge_penalty * FRAME_RD_EDGE_WEIGHT
                if split_score < best_score or (split_score == best_score and len(split_record) < len(best_record)):
                    best_record = split_record
                    best_score = split_score
                    best_reconstructed = split_reconstructed
                    best_motion_copies = split_motion_copies
                    best_literal_blocks = split_literal_count
                    best_visual_penalty = split_visual_penalty
                    best_edge_penalty = split_edge_penalty

            record_bytes.append(best_record)
            reconstructed[y0:y1, x0:x1] = best_reconstructed
            motion_copies_local += best_motion_copies
            literal_blocks_local += best_literal_blocks
            visual_penalty_total += best_visual_penalty
            edge_penalty_total += best_edge_penalty
            changed_blocks += 1

    return (
        b"".join(record_bytes),
        reconstructed,
        changed_blocks,
        motion_copies_local,
        literal_blocks_local,
        visual_penalty_total,
        edge_penalty_total,
    )


def iter_chunks(items: list[np.ndarray], size: int) -> Iterator[list[np.ndarray]]:
    for start in range(0, len(items), size):
        yield items[start:start + size]


def empty_frame_type_stats() -> dict[str, int]:
    return {tag: 0 for tag in ("I", "D", "H", "M", "G", "R", "N")}


def build_intra_candidate(frame_words: np.ndarray, full_payload: bytes) -> FrameCandidate:
    return FrameCandidate(
        tag="I",
        blob=b"I" + struct.pack("<I", len(full_payload)) + full_payload,
        reconstructed=frame_words.copy(),
        changed_units=1,
        motion_copies=0,
        literal_blocks=1,
        visual_penalty=0,
        edge_penalty=0,
        exact=True,
    )


def build_row_diff_candidate(
    tag: str,
    frame_words: np.ndarray,
    reference_frame: np.ndarray,
    *,
    global_motion: tuple[int, int] | None,
    gap_limit: int,
    full_row_ratio: float,
) -> FrameCandidate | None:
    working_reference = reference_frame if global_motion is None else apply_global_motion_reference(reference_frame, global_motion[0], global_motion[1])
    payload, changed_rows, literal_spans, _ = encode_row_diff_records(
        frame_words,
        working_reference,
        gap_limit=gap_limit,
        full_row_ratio=full_row_ratio,
    )
    if changed_rows == 0:
        if global_motion is None:
            return FrameCandidate(
                tag="N",
                blob=b"N",
                reconstructed=reference_frame.copy(),
                changed_units=0,
                motion_copies=0,
                literal_blocks=0,
                visual_penalty=0,
                edge_penalty=0,
                exact=True,
            )
        return FrameCandidate(
            tag=tag,
            blob=b"H" + struct.pack("<bb", global_motion[0], global_motion[1]) + payload,
            reconstructed=frame_words.copy(),
            changed_units=0,
            motion_copies=0,
            literal_blocks=0,
            visual_penalty=0,
            edge_penalty=0,
            exact=True,
        )
    if global_motion is None:
        blob = b"D" + payload
    else:
        blob = b"H" + struct.pack("<bb", global_motion[0], global_motion[1]) + payload
    return FrameCandidate(
        tag=tag,
        blob=blob,
        reconstructed=frame_words.copy(),
        changed_units=changed_rows,
        motion_copies=0,
        literal_blocks=literal_spans,
        visual_penalty=0,
        edge_penalty=0,
        exact=True,
    )


def build_motion_candidate(
    tag: str,
    frame_words: np.ndarray,
    reference_frame: np.ndarray,
    *,
    global_motion: tuple[int, int] | None,
    frame_width: int,
    frame_height: int,
    block_size: int,
    change_ratio: float,
    motion_search_radius: int,
    motion_search_step: int,
    motion_error_ratio: float,
    analysis: FrameAnalysis,
) -> FrameCandidate | None:
    working_reference = reference_frame if global_motion is None else apply_global_motion_reference(reference_frame, global_motion[0], global_motion[1])
    payload, reconstructed, changed_blocks, motion_copies, literal_blocks, visual_penalty, edge_penalty = encode_motion_records(
        frame_words,
        working_reference,
        frame_width=frame_width,
        frame_height=frame_height,
        block_size=block_size,
        change_ratio=change_ratio,
        motion_search_radius=motion_search_radius,
        motion_search_step=motion_search_step,
        motion_error_ratio=motion_error_ratio,
        analysis=analysis,
    )
    if changed_blocks == 0:
        return None
    if global_motion is None:
        blob = b"M" + struct.pack("<H", changed_blocks) + payload
    else:
        blob = b"G" + struct.pack("<bbH", global_motion[0], global_motion[1], changed_blocks) + payload
    return FrameCandidate(
        tag=tag,
        blob=blob,
        reconstructed=reconstructed,
        changed_units=changed_blocks,
        motion_copies=motion_copies,
        literal_blocks=literal_blocks,
        visual_penalty=visual_penalty,
        edge_penalty=edge_penalty,
        exact=(visual_penalty == 0 and edge_penalty == 0),
    )


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
) -> ChunkEncodeResult:
    frame_height, frame_width = frames[0].shape
    previous_frame: np.ndarray | None = None if initial_previous_frame is None else initial_previous_frame.copy()
    analysis_list: list[FrameAnalysis] = []
    analysis_reference = previous_frame
    for frame_words in frames:
        analysis = analyze_frame_transition(
            frame_words,
            analysis_reference,
            block_size=block_size,
            motion_search_radius=motion_search_radius,
            motion_search_step=motion_search_step,
        )
        analysis_list.append(analysis)
        analysis_reference = frame_words
    encoded = bytearray()
    frame_offsets: list[int] = []
    keyframes = 0
    predicted = 0
    repeated = 0
    block_updates = 0
    motion_copies = 0
    literal_blocks = 0
    frame_type_counts = empty_frame_type_stats()
    frame_type_bytes = empty_frame_type_stats()
    scene_cuts = 0
    drift_resets = 0
    drift_score = 0
    anchor_frame: np.ndarray | None = None

    for local_index, (frame_words, analysis) in enumerate(zip(frames, analysis_list)):
        frame_offsets.append(len(encoded))
        full_payload = rle16_encode(frame_words)
        intra_candidate = build_intra_candidate(frame_words, full_payload)
        hard_interval = previous_frame is None or ((frame_index_base + local_index) % keyframe_interval == 0)
        scene_cut = previous_frame is not None and is_scene_cut(analysis, keyframe_block_ratio)
        drift_limit = math.ceil(frame_width / block_size) * math.ceil(frame_height / block_size) * DRIFT_RESET_COST_PER_BLOCK
        force_keyframe = hard_interval or scene_cut or drift_score >= drift_limit
        if scene_cut:
            scene_cuts += 1
        if previous_frame is not None and drift_score >= drift_limit:
            drift_resets += 1

        chosen_candidate = intra_candidate
        if not force_keyframe and previous_frame is not None:
            gap_limit = max(1, min(6, int(round(motion_error_ratio * 32.0)) or 1))
            full_row_ratio = min(0.84, max(0.18, change_ratio * 4.0))
            candidates: list[FrameCandidate] = [
                intra_candidate,
            ]
            row_candidate = build_row_diff_candidate(
                "D",
                frame_words,
                previous_frame,
                global_motion=None,
                gap_limit=gap_limit,
                full_row_ratio=full_row_ratio,
            )
            if row_candidate is not None:
                candidates.append(row_candidate)

            global_motion = analysis.global_motion if analysis.global_motion_confidence >= GLOBAL_MOTION_MIN_CONFIDENCE else None
            if global_motion is not None:
                global_row_candidate = build_row_diff_candidate(
                    "H",
                    frame_words,
                    previous_frame,
                    global_motion=global_motion,
                    gap_limit=max(1, gap_limit - 1),
                    full_row_ratio=min(0.92, full_row_ratio + 0.10),
                )
                if global_row_candidate is not None:
                    candidates.append(global_row_candidate)

            motion_candidate = build_motion_candidate(
                "M",
                frame_words,
                previous_frame,
                global_motion=None,
                frame_width=frame_width,
                frame_height=frame_height,
                block_size=block_size,
                change_ratio=change_ratio,
                motion_search_radius=motion_search_radius,
                motion_search_step=motion_search_step,
                motion_error_ratio=motion_error_ratio,
                analysis=analysis,
            )
            if motion_candidate is not None:
                candidates.append(motion_candidate)

            if anchor_frame is not None:
                anchor_motion_candidate = build_motion_candidate(
                    "R",
                    frame_words,
                    anchor_frame,
                    global_motion=None,
                    frame_width=frame_width,
                    frame_height=frame_height,
                    block_size=block_size,
                    change_ratio=change_ratio,
                    motion_search_radius=motion_search_radius,
                    motion_search_step=motion_search_step,
                    motion_error_ratio=motion_error_ratio,
                    analysis=analysis,
                )
                if anchor_motion_candidate is not None:
                    candidates.append(anchor_motion_candidate)

            if global_motion is not None:
                global_motion_candidate = build_motion_candidate(
                    "G",
                    frame_words,
                    previous_frame,
                    global_motion=global_motion,
                    frame_width=frame_width,
                    frame_height=frame_height,
                    block_size=block_size,
                    change_ratio=change_ratio,
                    motion_search_radius=motion_search_radius,
                    motion_search_step=motion_search_step,
                    motion_error_ratio=motion_error_ratio,
                    analysis=analysis,
                )
                if global_motion_candidate is not None:
                    candidates.append(global_motion_candidate)

            chosen_candidate = min(candidates, key=lambda candidate: (candidate_cost(candidate, drift_score), len(candidate.blob)))

        encoded += chosen_candidate.blob
        frame_type_counts[chosen_candidate.tag] += 1
        frame_type_bytes[chosen_candidate.tag] += len(chosen_candidate.blob)
        block_updates += chosen_candidate.changed_units
        motion_copies += chosen_candidate.motion_copies
        literal_blocks += chosen_candidate.literal_blocks

        if chosen_candidate.tag == "I":
            keyframes += 1
            drift_score = 0
        else:
            predicted += 1
            repeated += 1 if chosen_candidate.tag == "N" else 0
            drift_score = 0 if chosen_candidate.exact else drift_score + chosen_candidate.visual_penalty + chosen_candidate.edge_penalty

        previous_frame = chosen_candidate.reconstructed.copy()
        if chosen_candidate.tag == "I":
            anchor_frame = chosen_candidate.reconstructed.copy()

    return ChunkEncodeResult(
        blob=bytes(encoded),
        frame_offsets=frame_offsets,
        keyframes=keyframes,
        predicted_frames=predicted,
        repeated_frames=repeated,
        block_updates=block_updates,
        motion_copies=motion_copies,
        literal_blocks=literal_blocks,
        frame_type_counts=frame_type_counts,
        frame_type_bytes=frame_type_bytes,
        scene_cuts=scene_cuts,
        drift_resets=drift_resets,
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

    subtitle_cues: list[SubtitleCue] = []
    if args.subtitle:
        if args.subtitle == "embedded":
            extracted = output_path.with_suffix(".srt")
            log("Extracting embedded subtitles...", quiet=args.quiet)
            extract_embedded_subtitles(input_path, extracted)
            subtitle_cues = parse_srt(extracted)
        else:
            subtitle_cues = parse_srt(Path(args.subtitle))

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
    frame_type_counts = empty_frame_type_stats()
    frame_type_bytes = empty_frame_type_stats()
    scene_cuts = 0
    drift_resets = 0
    start_time = time.time()
    previous_chunk_last_frame: np.ndarray | None = None

    def flush_chunk() -> None:
        nonlocal chunk_count, keyframes, predicted, repeated, block_updates, motion_copies, literal_blocks
        nonlocal frame_type_counts, frame_type_bytes, scene_cuts, drift_resets
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
        chunk_result = encode_chunk(
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
        stored_chunk_blob += struct.pack("<I", len(chunk_result.blob))
        for frame_offset in chunk_result.frame_offsets:
            stored_chunk_blob += struct.pack("<I", frame_offset)
        stored_chunk_blob += chunk_result.blob
        while len(stored_chunk_blob) % 4:
            stored_chunk_blob.append(0)
        stored_blob = compress_chunk_payload(bytes(stored_chunk_blob), args.zlib_level)
        offset = output_handle.tell()
        output_handle.write(stored_blob)
        chunk_index.append((offset, len(stored_blob), len(stored_chunk_blob), first_frame, len(current_chunk_frames), 0))
        total_size_bytes = output_handle.tell()
        chunk_count += 1
        keyframes += chunk_result.keyframes
        predicted += chunk_result.predicted_frames
        repeated += chunk_result.repeated_frames
        block_updates += chunk_result.block_updates
        motion_copies += chunk_result.motion_copies
        literal_blocks += chunk_result.literal_blocks
        scene_cuts += chunk_result.scene_cuts
        drift_resets += chunk_result.drift_resets
        for tag in frame_type_counts:
            frame_type_counts[tag] += chunk_result.frame_type_counts[tag]
            frame_type_bytes[tag] += chunk_result.frame_type_bytes[tag]
        elapsed = time.time() - start_time
        progress = min(100.0, (frame_count / expected_frames) * 100.0) if expected_frames else 0.0
        fps_done = frame_count / elapsed if elapsed > 0 else 0.0
        eta_frames = max(0, expected_frames - frame_count) if expected_frames else 0
        eta = (eta_frames / fps_done) if fps_done > 0 and eta_frames else 0.0
        log(
            f"Chunk {chunk_count:03d}: {progress:5.1f}% | ETA {format_duration_hms(eta)} | "
            f"frames {first_frame}-{frame_count - 1} | size {len(stored_blob) / 1024:.1f} KiB | "
            f"total size {total_size_bytes / (1024 * 1024):.2f} MiB | "
            f"I={chunk_result.frame_type_counts['I']} D={chunk_result.frame_type_counts['D']} "
            f"H={chunk_result.frame_type_counts['H']} M={chunk_result.frame_type_counts['M']} "
            f"G={chunk_result.frame_type_counts['G']} R={chunk_result.frame_type_counts['R']} "
            f"N={chunk_result.frame_type_counts['N']} | "
            f"cuts={chunk_result.scene_cuts} drift={chunk_result.drift_resets}",
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
    for cue in subtitle_cues:
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
        subtitle_count=len(subtitle_cues),
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
        frame_type_counts=frame_type_counts,
        frame_type_bytes=frame_type_bytes,
        scene_cuts=scene_cuts,
        drift_resets=drift_resets,
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
        f"I={frame_type_counts['I']} D={frame_type_counts['D']} H={frame_type_counts['H']} "
        f"M={frame_type_counts['M']} G={frame_type_counts['G']} R={frame_type_counts['R']} "
        f"N={frame_type_counts['N']} | "
        f"scene cuts={scene_cuts} | drift resets={drift_resets} | "
        f"{block_updates} changed units | {motion_copies} motion copies | {literal_blocks} literal blocks",
        quiet=args.quiet,
    )
    return stats


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="Input video file")
    parser.add_argument("--output", required=True, help="Output .nvp.tns file")
    parser.add_argument("--subtitle", help="Optional .srt path or 'embedded'")
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
