# Nspire-VideoPlayer-Ndless

Native Ndless video player and PC-side encoder for the TI-Nspire CX and CX II line.

This project targets the **TI-Nspire CX**, **TI-Nspire CX II**, and **TI-Nspire CX II-T**, and plays streamed `.nvp` movies from calculator storage. The player binary and the movie data stay separate:

- `ndvideo.tns`: the Ndless launcher
- `*.nvp.tns`: movie containers produced by the encoder

## Screenshots

| Main menu | Continue watching | Playback controls |
| --- | --- | --- |
| ![Main menu](./examples/screenshots/main-menu.png) | ![Continue watching](./examples/screenshots/continue-watching.png) | ![Playback controls](./examples/screenshots/playback-controls.png) |

| UI overlay | Dialogue scene | Subtitle playback |
| --- | --- | --- |
| ![Playback UI overlay](./examples/screenshots/ui-overlay.png) | ![Dialogue scene](./examples/screenshots/dialogue.png) | ![Subtitle playback](./examples/screenshots/subtitles.png) |

## Current Format

The `.nvp` format used by the current player is:

- H.264 Annex B video bitstream in legacy version 9/10 containers
- H.264 or MPEG-4 Part 2 video in version 11 codec-tagged containers
- chunked container with per-chunk frame tables
- optional text subtitle tracks stored in the container
- raw stored chunk payloads

## Features

- native C/Ndless runtime
- CX and CX II LCD paths through Ndless' native framebuffer modes
- streamed playback from calculator storage
- password-protected videos with authenticated AES-256 encryption
- H.264 decode through `h264bsd`
- MPEG-4 Part 2 decode through vendored Xvid sources
- RGB565 output
- direct storage reads on CX and CX II, with decoded frames prepared ahead for smoother playback
- SRAM lookup tables on CX and CX II
- accurate frame pacing from a hardware-backed monotonic timer
- subtitle support for text subtitle tracks
- built-in subtitle font cycling
- scale modes: `FIT`, `FILL`, `STRETCH`, `1:1`
- playback speed control from `0.25x` to `4.0x`, including `2.5x` and `3.5x`
- automatic CPU clocks: 240 MHz on CX/CX CAS (60 MHz AHB), 492 MHz on CX II variants
- overclocking works while plugged in and is reapplied after in-app standby
- previous clock settings restored on exit
- screen brightness control with `Up` / `Down` and an on-screen percentage overlay
- adjustable warm night filter
- idle dimming, screen-off and automatic standby
- theme color profiles: `DORFic`, `Blue`, `Green`, and `Red`
- picker UI for multiple `.nvp` / `.nvp.tns` files
- picker filename metadata tooltips from bracketed tags
- resume history with saved playback, subtitle, and theme settings
- debug log output and in-player memory/playback overlay

## Current Limits

- no audio playback (CX and CX II calculators have no built-in speakers)

## Battery Life

Battery Life: ~9.5 hours of continuous H.264 playback at 100% brightness on a CX II-T.

## Controls

### Picker

- `Up` / `Down` or keypad `8` / `2`: select movie
- touchpad: move cursor
- touchpad hover: show the title, filename metadata and video duration
- touchpad click: open highlighted movie
- `Enter` or keypad `5`: open movie
- `C`: cycle theme color
- `S`: save a BMP screenshot
- `Scratchpad`: save state and open OS Scratchpad
- `Esc`: exit

### Movie Filename Metadata

The picker hides the full video extension and supports optional bracketed metadata in movie filenames.

Example:

```text
Rick and Morty S07E03 [English SDH].nvp.tns
```

The list row shows `Rick and Morty S07E03`. Hover briefly to see the clean title, `English SDH` metadata and the video duration on separate lines. Multiple bracketed tags are joined with ` | ` in the tooltip.

### Playback

- `Space`: play / pause with a small pause indicator, or restart when the movie has ended
- touchpad: move cursor and show the UI
- `Enter` / keypad `5` / touchpad click: play / pause, restart at end, click hovered controls, or seek inside the bottom UI band
- `Left` / `Right` or keypad `4` / `6`: seek `-5s` / `+5s`
- keypad `7` / `9`: switch to the previous / next video in the current directory
- `Up` / `Down` or keypad `8` / `2`: increase / decrease screen brightness
- `N`: toggle night mode
- `Ctrl` + touchpad `Up` / `Down`: enable night mode, then adjust its intensity; hold to repeat. At 0%, night mode is off.
- `Tab`: single-frame step while paused, hold to repeat
- `P`: cycle playback mode: `PLAY ONCE`, `REPLAY`, `AUTO NEXT`
- `R`: toggle realtime sync, allowing displayed-frame drops instead of slowdown
- `/`: cycle scale mode
- `Ctrl` + keypad `1`-`9`: align video
- `{` / `}`: decrease / increase playback speed
- `^`: cycle subtitle placement
- `+` / `-`: increase / decrease subtitle size, down to hidden
- `F`: cycle subtitle font
- `T`: cycle subtitle track
- `M`: toggle memory / playback diagnostics overlay
- `C`: cycle theme color
- `D`: toggle performance recording and diagnostic logging (off by default)
- `S`: save a BMP screenshot
- `Catalog`: open / close the help overlay
- `Scratchpad`: save state and open OS Scratchpad
- `Esc`: close help, or leave the movie if help is not open
- `On`: smoothly turn the screen off or back on; while off, `Esc` saves history and returns to the OS home menu

Night mode also works in the picker and resume prompt. Its intensity is remembered while the app is open.

### Resume Prompt

- `Left` / `Right` or keypad `4` / `6`: choose `CONTINUE` or `START OVER`
- touchpad: move cursor
- touchpad click: activate the highlighted button
- `Enter` or keypad `5`: confirm the selected button
- `C`: cycle theme color
- `S`: save a BMP screenshot
- `Scratchpad`: save state and open OS Scratchpad
- `Esc`: cancel and return

### Encrypted Videos

Locked movies ask for their password before loading. Type with the keyboard;
hold `Shift` for uppercase and use `Tab` for symbols. `Enter` or a touchpad click presses the hovered
control, or the outlined default (Unlock unless you select a symbol with the arrow keys).
`Del` deletes and `Esc` cancels. Unlocking takes a while on the calculator;
the progress indicator stays responsive. The key is cleared when you leave the video.
Screenshots are disabled while an encrypted video is visible.

Install `cryptography` with `python -m pip install cryptography`, then add
`--encrypt` to your usual encoder command. It prompts for a password of up to
128 printable ASCII characters. For scripts, use `--password-file PATH`.

To encrypt an existing movie without re-encoding:

```sh
python tools/nve_crypto.py movie.nvp.tns locked.nvp.tns
```

Video, subtitles and the internal index are encrypted in independently verified
16 KiB records, so seeking still works. File size grows by about 0.2%.
The filename and duration stay visible; original files and any requested preview
are kept unencrypted.

## Idle Power Management

In the picker, resume prompt or paused playback, the screen dims after one minute, turns off after two, and enters standby after three. Input wakes the screen; active playback keeps it awake. On the verified CX II-T firmware, standby resumes directly in the player. Other firmware uses the normal OS suspend path.

## Subtitle Fonts

The built-in subtitle font cycle currently includes:

- `Tinytype`
- `VGA`
- `Thin`
- `Space`
- `Fantasy`

## Repository Layout

- [src/player](src/player): native player shell and playback/UI implementation
- [src/movie](src/movie): `.nvp` container format definitions
- [src/codecs](src/codecs): codec adapters and vendored MPEG-4/Xvid decoder sources
- [src/codecs/h264bsd](src/codecs/h264bsd): H.264 decoder sources
- [src/initfini.c](src/initfini.c): startup / shutdown glue
- [tools/encode_ndless_video.py](tools/encode_ndless_video.py): PC-side encoder
- [tools/pack_zehn.py](tools/pack_zehn.py): Zehn packer used by the build
- [examples/screenshots](examples/screenshots): README screenshot assets
- [examples](examples): packaged sample files for quick calculator-side testing
- [Makefile](Makefile): build entry point

## Build

If you just want to run the player on a calculator, you do not have to build it yourself. The latest GitHub Actions run uploads `ndvideo.tns` as an artifact in the repository's `Actions` tab.

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

The build writes to [dist](dist):

- `ndvideo.tns`
- `ndvideo.elf`
- `ndvideo.zehn`

## Encoder

The encoder turns a normal video file into a streamed `.nvp.tns` movie. H.264 is still the default and writes legacy version 10 containers for compatibility. MPEG-4 Part 2 can be selected with `--codec mpeg4` and writes version 11 codec-tagged containers.

### Python Requirements

```bash
pip install imageio-ffmpeg numpy pillow
```

### Basic Example

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mp4" --output ".\dist\video.nvp.tns"
```

### MPEG-4 Part 2 Example

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mp4" --codec mpeg4 --output ".\dist\video-mpeg4.nvp.tns"
```

### Embedded Subtitles

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --subtitle embedded --output ".\dist\video.nvp.tns"
```

### Burn Subtitles Into Video

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --subtitle embedded --burn-subtitles --output ".\dist\video.nvp.tns"
```

### Burn Larger Subtitles Into Video

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --subtitle embedded --burn-subtitles --burn-subtitle-size 1.5 --output ".\dist\video.nvp.tns"
```

`--burn-subtitle-size` scales burned subtitles relative to the default output-safe size. It works for text subtitle burns and embedded bitmap subtitle burns.

### Write a Preview MP4

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --subtitle embedded --burn-subtitles --preview-mp4 --output ".\dist\video.nvp.tns"
```

`--preview-mp4` also writes a video-only `.preview.mp4` next to the `.nvp.tns` output so you can quickly inspect subtitle burn, framing, and quality on PC before copying the movie to the calculator.

### Preserve Source Framerate

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --subtitle embedded --fps source --output ".\dist\video.nvp.tns"
```

### Recommended Full-Episode Example

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --subtitle embedded --output ".\dist\video.nvp.tns" --fps 16 --max-width 320 --max-height 180 --max-chunk-kib 64 --stream-profile quality --crf 14.5 --preset veryslow --level 1.3
```

### Target A Specific Size With 2-Pass ABR

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --output ".\dist\video.nvp.tns" --fps 16 --max-width 320 --max-height 180 --max-chunk-kib 64 --idr-frames byte-auto --stream-profile quality --bitrate-kbps 140 --two-pass --preset veryslow --level 1.3
```

Use CRF when you want the best quality-per-bit without caring about the exact final size. Use `--bitrate-kbps ... --two-pass` when you need a tighter size target.

Chunks are packed by `--max-chunk-kib`; `64` is a useful starting point for smooth
playback. `--chunk-frames` adds an optional frame-count ceiling (`0` disables it).
`--idr-frames auto` estimates a fixed keyframe interval from the bitrate and byte cap.

`--idr-frames byte-auto` measures frame sizes and splits oversized GOPs while
leaving the others alone. Files that already fit skip refinement. Analysis is
cached for repeat encodes and nearby bitrate changes; every final chunk is still
checked. `--max-chunk-overshoot-percent` permits small near-misses without adding
more keyframes just to shave off a few bytes.

When `--fps` caps or changes the framerate, the encoder timeline-samples frames and then verifies the encoded frame count against the intended duration. The `.json` sidecar records source fps, target fps, expected frames, actual frames, and drift in milliseconds.

### Main Encoder Options

- `--output`
- `--subtitle`
- `--burn-subtitles`
- `--burn-subtitle-size`
- `--subtitle-track`
- `--fps`
- `--max-width`
- `--max-height`
- `--active-aspect`
- `--crop`
- `--chunk-frames`
- `--idr-frames`
- `--max-chunk-kib`
- `--max-chunk-overshoot-percent`
- `--crf`
- `--bitrate-kbps`
- `--two-pass`
- `--preset`
- `--level`
- `--stream-profile`
- `--start`
- `--duration`
- `--timeline-drift-tolerance-ms`
- `--preview-mp4`
- `--quiet`

Run `python tools/encode_ndless_video.py --help` for the full CLI.

## Diagnostics

Debug logging is off by default. Press `D` during playback to start or stop a performance recording, then leave the movie normally to save `ndvideo-debug.log` beside it. No diagnostic files are written unless you enable `D`.

The `M` overlay shows:

- tracked movie-buffer usage (not total OS memory)
- cache usage
- current frame
- contiguous decoded runway
- decode target
- lag count
- ring-hit vs direct-decode counts
- whether verbose debug logging is currently enabled

The [examples](examples) folder also includes a short packaged sample movie and a matching `ndvideo.tns` for quick on-device smoke testing.

## Install On Calculator

1. Download `ndvideo.tns` from the latest GitHub Actions artifact, or build it locally.
2. Encode one or more videos into `.nvp.tns`.
3. Copy `ndvideo.tns` and the movie files to the calculator.
4. Launch `ndvideo.tns` through Ndless.
5. Pick a movie and play it locally from storage.

## License

Unless noted otherwise, the software in this repository is licensed under the GNU General Public License, version 3. See [LICENSE](LICENSE).
