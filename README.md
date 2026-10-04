# Nspire-VideoPlayer-Ndless

Native Ndless video player and PC-side encoder for the TI-Nspire CX and CX II line.

This project targets the **TI-Nspire CX**, **TI-Nspire CX II**, and **TI-Nspire CX II-T**, and plays streamed `.nvp` movies from calculator storage. The player binary and the movie data stay separate:

- `_ndvideo.tns`: the Ndless launcher
- `*.nvp.tns`: movie containers produced by the encoder

`_ndvideo.tns` includes all four codecs. Smaller player builds are also
available; their filenames list the included codecs (`hevc` means H.265).
Each build is a single file. Multi-codec builds load only the decoder needed
for the current video, leaving the rest of that memory available for buffering.

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
- H.264, H.265 (HEVC), AV1 or MPEG-4 Part 2 video in version 11 codec-tagged containers
- chunked container with per-chunk frame tables
- optional text subtitle tracks stored in the container
- raw stored chunk payloads

## Features

- native Ndless runtime
- CX and CX II LCD paths through Ndless' native framebuffer modes
- streamed playback from calculator storage
- password-protected videos with authenticated AES-256 encryption
- H.264 decode through `h264bsd`
- H.265 / HEVC decode through an optimized `libde265` port
- AV1 decode through an optimized `dav1d` port
- MPEG-4 Part 2 decode through vendored Xvid sources
- RGB565 output
- direct storage reads on CX and CX II, with decoded frames prepared ahead for smoother playback
- SRAM lookup tables on CX and CX II
- accurate frame pacing from a hardware-backed monotonic timer
- subtitle support for text subtitle tracks
- built-in subtitle font cycling
- scale modes: `FIT`, `FILL`, `STRETCH`, `1:1`
- playback speed control from `0.25x` to `4.0x`, including `2.5x` and `3.5x`
- selectable CPU clocks: default 132 MHz on CX/CX CAS and 396 MHz on CX II, selectable up to 240 / 492 MHz
- the selected speed works on battery and USB, and is reapplied after in-app standby
- restore the previous clock speed on exit, or keep the selected speed
- screen brightness control with `Up` / `Down` and an on-screen percentage overlay
- adjustable warm night filter
- idle dimming, screen-off and automatic standby
- theme color profiles: `DORFic`, `Blue`, `Green`, and `Red`
- picker UI for multiple `.nvp` / `.nvp.tns` files
- picker filename metadata tooltips from bracketed tags
- resume history with saved playback, subtitle, and theme settings
- debug log output and in-player memory/playback overlay

## Decoder Loading and Buffering

All four decoders live inside `_ndvideo.tns`, but only the one needed for the
current video is loaded into RAM. Switching formats unloads the previous
decoder once playback and previews have finished using it. There are no extra
files to install, and decoder code stays in RAM throughout playback. The last
decoder is kept ready for reopening videos in the same format.

This saves roughly **1.2–1.3 MiB of RAM during HEVC or AV1 playback** compared
with keeping every decoder loaded. H.264, HEVC and AV1 keep most buffered pictures
in compact YUV form, using 25% less memory per picture than RGB565. The next few
pictures are converted to screen-ready RGB ahead of presentation. The buffer
uses measured free RAM while reserving space for decoding and other player work.

Opening a different format can add a short decoder-loading step. Single-codec
builds load their decoder with the app and remain the smallest download.

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
- `O`: processor speed menu (also available from the MHz label in the picker footer)
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

Seeking shows the frames leading up to the destination. Play/pause works during
that catch-up, and repeated seek presses extend the destination. On arrival,
playback returns to the paused or playing state from before the seek.

### Processor Speed

Click the MHz label or press `O` in the picker, playback or resume screen.
Click the track, drag the knob, or use Left/Right or 4/6. The orange mark shows
the applied speed. **Test** compares the selected speed with the default,
then restores the previous setting. **Apply** changes this session;
the choice is saved in `ndhistory.ts.tns` only on normal app exit. A reset or crash
won't save a new choice. The short benchmark measures speed, not long-term stability.

Default uses the normal unplugged speed even on USB: 132 MHz on CX and
396 MHz on CX II. Exit restores the OS's original clock settings unless
**Keep speed after exit** is checked. This option is saved with the selected speed.

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
`Return` always presses Unlock. Playback shortcuts stay disabled while entering a password.
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
- [src/codecs](src/codecs): codec adapters and vendored decoder sources
- [src/codecs/h264bsd](src/codecs/h264bsd): H.264 decoder sources
- [src/codecs/hevc](src/codecs/hevc): H.265 / HEVC decoder sources
- [src/initfini.c](src/initfini.c): startup / shutdown glue
- [tools/encode_ndless_video.py](tools/encode_ndless_video.py): PC-side encoder
- [tools/pack_zehn.py](tools/pack_zehn.py): Zehn packer used by the build
- [examples/screenshots](examples/screenshots): README screenshot assets
- [examples](examples): packaged sample files for quick calculator-side testing
- [Makefile](Makefile): build entry point

## Build

If you just want to run the player on a calculator, you do not have to build it yourself. The latest GitHub Actions run uploads `_ndvideo.tns` as an artifact in the repository's `Actions` tab.

### Requirements

- Ndless SDK
- ARM GCC toolchain available in `PATH`
- `make`
- `bash`
- Python 3.10 or newer
- `pyelftools`

### Build Command

```bash
make
```

Build only the codecs needed, or produce the complete release set:

```bash
make CODECS=h264
make CODECS="h264 hevc"
make release
```

Codec choices are `h264`, `mpeg4`, `hevc` and `av1`, in any order. Each combination
uses separate build objects. `make release` builds all 15 combinations
and packages them in `release/`.

### Build Output

The build writes to [dist](dist):

- `_ndvideo.tns`
- `ndvideo.elf`
- `ndvideo.zehn`

The release contains these player builds:

| File | Included codecs |
| --- | --- |
| `_ndvideo.tns` | H.264, MPEG-4 Part 2, H.265 / HEVC, AV1 |
| `_ndvideo-h264.tns` | H.264 |
| `_ndvideo-mpeg4.tns` | MPEG-4 Part 2 |
| `_ndvideo-hevc.tns` | H.265 / HEVC |
| `_ndvideo-av1.tns` | AV1 |

Every two- and three-codec combination is included too, named in the same
order: for example, `_ndvideo-h264-hevc-av1.tns`.

The smaller builds have the same controls and features. A video needing an
omitted codec shows an in-player message naming it; OK returns to the menu,
including when the video was opened automatically. Movies use the
same format across builds, including encrypted movies.

`ndvideo-symbols.zip` holds the matching ELF and Zehn files for all 15 builds.
It also includes the decoder modules used by the multi-codec builds.
Checksums and shared license texts are included alongside the players.

## Encoder

The encoder turns a normal video file into a streamed `.nvp.tns` movie. H.264 is
the default and writes version 10 containers. `--codec hevc` selects H.265;
`--codec av1` selects AV1; `--codec mpeg4` selects MPEG-4 Part 2.
These alternatives use version 11 containers.

### Python Requirements

Use Python 3.10 or newer, with:

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

### H.265 / HEVC

HEVC trades more decoding work for smaller files at similar image quality.
It is especially useful for animation at modest frame rates; H.264 remains the
better general-purpose choice when smooth playback matters more than file size.
Detailed scenes and higher frame rates can still exceed the calculator's speed,
even with an overclock. Smaller files do not necessarily decode faster.

For example, a 320x180, 16 FPS animation encode went from 23.29 MiB in H.264 to
12.39 MiB in HEVC at similar quality, with no lags reported at 1x on a CX II-T
running at 492 MHz.

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\animation.mkv" `
  --codec hevc --fps 16 --max-width 320 --max-height 180 `
  --stream-profile quality --crf 29.5 --preset veryslow `
  --max-chunk-kib 64 --idr-frames auto --subtitle embedded `
  --output ".\dist\animation-hevc.nvp.tns"
```

This is a starting point, not a fixed quality target. Lower CRF gives higher
quality and larger files; CRF values are not equivalent between H.264 and HEVC.
`--stream-profile fast` reduces decoding work at a detail cost. `balanced` and
`quality` allow smaller coding blocks and spend more encoding time on compression.

HEVC needs FFmpeg with `libx265` and a player version with HEVC support; older
releases cannot play these files. The encoder selects the supported Main 8-bit
settings automatically. Subtitles, encryption, previews and two-pass bitrate
encoding work as usual. Encoding can take considerably longer than H.264.

Use `--idr-frames auto` or a frame count. Oversized groups of frames are shortened
automatically to fit the chunk limit. `--idr-frames byte-auto` and `--level` are
H.264-only.

### AV1

AV1 offers another option for smaller files, at a higher decoding cost. It has
been tested on a CX II-T, but demanding scenes can still stutter even at
492 MHz. Animation at modest frame rates is a useful starting point; H.264
remains the better choice for playback speed.

Use a build whose name includes `av1`, or the complete `_ndvideo.tns`. The
example below uses 16 FPS; `--fps source` keeps the original frame rate.

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\animation.mkv" `
  --codec av1 --fps 16 --max-width 320 --max-height 180 `
  --stream-profile quality --crf 42 --preset slow `
  --max-chunk-kib 64 --idr-frames auto --subtitle embedded `
  --output ".\dist\animation-av1.nvp.tns"
```

AV1 requires FFmpeg with `libaom-av1`. CRF is an integer from 0 to 63; lower
values mean higher quality and larger files. `slow`, `veryslow` and `placebo`
map to encoder speeds 2, 1 and 0. The encoder selects 8-bit 4:2:0, avoids hidden
frames and expensive filters, and keeps chunks independently seekable.
Subtitles, encryption, previews and two-pass bitrate encoding work as usual.
CRF values are not comparable across codecs, and AV1 encoding can take a long
time. Smaller output does not guarantee faster playback.

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

Chunks are packed by `--max-chunk-kib`; `64` is a starting point, not a fixed
performance target. Very small chunks can force frequent keyframes, increasing
file size and decoding work, especially with HEVC and AV1. Larger chunks trade
more RAM and longer seek catch-up for fewer keyframes. `--chunk-frames` adds an
optional frame-count ceiling (`0` disables it).
`--idr-frames auto` estimates a fixed keyframe interval from the bitrate and byte cap.

`--idr-frames byte-auto` measures frame sizes and splits oversized GOPs while
leaving the others alone. Files that already fit skip refinement. Analysis is
cached for repeat encodes and nearby bitrate changes; every final chunk is still
checked. `--max-chunk-overshoot-percent` permits small near-misses without adding
more keyframes just to shave off a few bytes.

When `--fps` caps or changes the framerate, the encoder timeline-samples frames and then verifies the encoded frame count against the intended duration. The `.json` sidecar records source fps, target fps, expected frames, actual frames, and drift in milliseconds.

### Main Encoder Options

- `--codec`
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

Debug logging is off by default. Press `D` during playback to start or stop a
recording, then leave the movie normally to save `ndvideo-debug.log` beside it.
Recording ends with that movie; auto-next does not overwrite its files.
Totals cover the entire run; frame details keep 128 frames before each lag
and two seconds afterward. Overlapping windows are merged. Storage is bounded,
and the log reports if older detail was replaced. Routine recordings require
`D`; failures can also save `ndvideo-error.log.tns`. Standby details are included
in the same debug log.

The `M` overlay shows:

- tracked movie-buffer usage (not total OS memory)
- cache usage
- current frame
- contiguous decoded runway
- decode target
- lag count
- ring-hit vs direct-decode counts
- whether verbose debug logging is currently enabled

The [examples](examples) folder includes a short packaged sample movie to try on your calculator.

## Install On Calculator

1. Download `_ndvideo.tns` from the latest GitHub Actions artifact, or build it locally.
2. Encode one or more videos into `.nvp.tns`.
3. Copy `_ndvideo.tns` and the movie files to the calculator.
4. Launch `_ndvideo.tns` through Ndless.
5. Pick a movie and play it locally from storage.

## License

Unless noted otherwise, the software in this repository is licensed under the GNU General Public License, version 3. See [LICENSE](LICENSE).

Bundled components' credits and licenses are collected in
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
