# Nspire-VideoPlayer-Ndless

Native Ndless video player and PC-side H.264 encoder for the TI-Nspire CX II line.

This project targets the **TI-Nspire CX II-T** and plays streamed `.nvp` movies from calculator storage. The player binary and the movie data stay separate:

- `ndvideo.tns`: the Ndless launcher
- `*.nvp.tns`: movie containers produced by the encoder

## Screenshots

| Subtitle playback | UI overlay | Dialogue scene |
| --- | --- | --- |
| ![Subtitle playback](./examples/screenshots/subtitles.png) | ![Playback UI overlay](./examples/screenshots/ui-overlay.png) | ![Dialogue scene](./examples/screenshots/dialogue.png) |

## Current Format

The `.nvp` format used by the current player is:

- H.264 Annex B video bitstream
- chunked container with per-chunk frame tables
- raw stored chunk payloads (no zlib layer)
- optional text subtitle tracks stored in the container

## Features

- native C/Ndless runtime
- streamed playback from calculator storage
- H.264 decode through `h264bsd`
- RGB565 output
- chunk-byte prefetching for smoother playback
- accurate frame pacing from a hardware-backed monotonic timer
- subtitle support for text subtitle tracks
- built-in subtitle font cycling
- scale modes: `FIT`, `FILL`, `STRETCH`, `1:1`
- playback speed control from `0.25x` to `2.0x`
- picker UI for multiple `.nvp` / `.nvp.tns` files
- debug log output and in-player memory/playback overlay

## Current Limits

- no audio yet
- subtitle rendering is text-only
- the runtime is tuned for streamed local playback, not full in-memory decode

## Controls

### Picker

- `Up` / `Down`: select movie
- `Enter`: open movie
- `Esc`: exit

### Playback

- `Enter`: play / pause
- touchpad click: play / pause, or seek when clicking inside the bottom UI band
- `Left` / `Right`: seek `-5s` / `+5s`
- `Tab`: single-frame step while paused
- `/`: cycle scale mode
- `{` / `}`: decrease / increase playback speed
- `^`: cycle subtitle placement
- `+` / `-`: increase / decrease subtitle size, down to hidden
- `F`: cycle subtitle font
- `M`: toggle memory / playback diagnostics overlay
- `D`: toggle verbose debug logging
- `Catalog`: open / close the help overlay
- `Esc`: close help, or leave the movie if help is not open

## Subtitle Fonts

The built-in subtitle font cycle currently includes:

- `Tinytype`
- `VGA`
- `Thin`
- `Space`
- `Fantasy`

## Repository Layout

- [src/player.c](/C:/Users/GigaZelensky/Documents/GitHub/Nspire-VideoPlayer-Ndless/src/player.c): native player
- [src/h264bsd](/C:/Users/GigaZelensky/Documents/GitHub/Nspire-VideoPlayer-Ndless/src/h264bsd): H.264 decoder sources
- [src/initfini.c](/C:/Users/GigaZelensky/Documents/GitHub/Nspire-VideoPlayer-Ndless/src/initfini.c): startup / shutdown glue
- [tools/encode_ndless_video.py](/C:/Users/GigaZelensky/Documents/GitHub/Nspire-VideoPlayer-Ndless/tools/encode_ndless_video.py): PC-side encoder
- [tools/pack_zehn.py](/C:/Users/GigaZelensky/Documents/GitHub/Nspire-VideoPlayer-Ndless/tools/pack_zehn.py): Zehn packer used by the build
- [examples/screenshots](/C:/Users/GigaZelensky/Documents/GitHub/Nspire-VideoPlayer-Ndless/examples/screenshots): README screenshot assets
- [examples](/C:/Users/GigaZelensky/Documents/GitHub/Nspire-VideoPlayer-Ndless/examples): packaged sample files for quick calculator-side testing
- [Makefile](/C:/Users/GigaZelensky/Documents/GitHub/Nspire-VideoPlayer-Ndless/Makefile): build entry point

## Build

### Requirements

- Ndless SDK
- ARM GCC toolchain available in `PATH`
- `make`
- `bash`
- `python`
- `pyelftools`

### Build Command

```bash
make
```

### Build Output

The build writes to [dist](/C:/Users/GigaZelensky/Documents/GitHub/Nspire-VideoPlayer-Ndless/dist):

- `ndvideo.tns`
- `ndvideo.elf`
- `ndvideo.zehn`

## Encoder

The encoder turns a normal video file into a streamed H.264 `.nvp.tns` movie.

### Python Requirements

```bash
pip install imageio-ffmpeg numpy pillow
```

### Basic Example

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mp4" --output ".\dist\video.nvp.tns"
```

### Embedded Subtitles

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --subtitle embedded --output ".\dist\video.nvp.tns"
```

### Preserve Source Framerate

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --subtitle embedded --fps source --output ".\dist\video.nvp.tns"
```

### Recommended Full-Episode Example

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --subtitle embedded --output ".\dist\video.nvp.tns" --fps 16 --max-width 320 --max-height 180 --chunk-frames 72 --stream-profile quality --crf 14.5 --preset veryslow --level 1.3
```

### Main Encoder Options

- `--output`
- `--subtitle`
- `--subtitle-track`
- `--fps`
- `--max-width`
- `--max-height`
- `--chunk-frames`
- `--idr-frames`
- `--max-chunk-kib`
- `--crf`
- `--preset`
- `--level`
- `--stream-profile`
- `--start`
- `--duration`
- `--quiet`

Run `python tools/encode_ndless_video.py --help` for the full CLI.

## Diagnostics

The player can write a debug log next to the movie file as `ndvideo-debug.log`.

The `M` overlay shows:

- total RAM usage
- cache usage
- current frame
- contiguous decoded runway
- decode target
- lag count
- ring-hit vs direct-decode counts
- whether verbose debug logging is currently enabled

Verbose debug logging is off by default. Press `D` during playback to enable it; normal playback exits do not write `ndvideo-debug.log` unless logging was enabled or the player hits an error.

The [examples](/C:/Users/GigaZelensky/Documents/GitHub/Nspire-VideoPlayer-Ndless/examples) folder also includes a short packaged sample movie and a matching `ndvideo.tns` for quick on-device smoke testing.

## Install On Calculator

1. Build `ndvideo.tns`.
2. Encode one or more videos into `.nvp.tns`.
3. Copy `ndvideo.tns` and the movie files to the calculator.
4. Launch `ndvideo.tns` through Ndless.
5. Pick a movie and play it locally from storage.

## License

Unless noted otherwise, the software in this repository is licensed under the GNU General Public License, version 3. See [LICENSE](/C:/Users/GigaZelensky/Documents/GitHub/Nspire-VideoPlayer-Ndless/LICENSE).
