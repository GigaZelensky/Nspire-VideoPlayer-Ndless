# Nspire-VideoPlayer-Ndless

This is the standalone native Ndless repo split out from the Lua/TNS generator repo.

It does not use ScriptApp image resources, giant `.tns` payloads, or `_R.IMG`. The calculator executable is a normal Ndless program, and the movie is a separate streamed container file stored on the calculator filesystem.

## Design

- Runtime: native C app in [`src/player.c`](./src/player.c)
- Encoder: PC-side packer in [`tools/encode_ndless_video.py`](./tools/encode_ndless_video.py)
- Storage model: copy one `ndvideo.tns` launcher plus one or more `.nvp` movie files onto the calculator
- RAM model: only the current framebuffer, subtitle table, and current chunk stay in memory

The current codec is deliberately runtime-cheap:

- RGB565 output
- chunked movie file
- every chunk starts with a keyframe
- predicted frames store only changed blocks
- keyframes and blocks use a tiny 16-bit RLE so the player does not depend on TI document image decoding

## Controls

- `Enter`: play/pause
- touchpad center click: play/pause, or seek when the progress bar is visible and selected
- `Left` / `Right`: seek `-5s` / `+5s`
- `Tab`: single-frame step while paused
- `/`: cycle scale mode (`fit`, `fill`, `native`)
- `+` / `-`: subtitle size (`3` levels)
- moving the touchpad shows the playback UI while the video is playing
- touchpad crosshair: seek directly on the visible progress bar
- `Esc`: back to the movie picker / exit
- `Up` / `Down`: choose a movie in the picker

## Build

This path uses the official Ndless SDK for the ARM compile/link stage, but it does not depend on the upstream host `genzehn` binary. The repo-local packer in [`tools/pack_zehn.py`](./tools/pack_zehn.py) writes the Zehn payload and wraps it with the official `zehn_loader.tns`.

Prerequisites:

- official Ndless SDK cloned at `./external/Ndless/ndless-sdk`
- ARM GCC toolchain available and `_NDLESS_TOOLCHAIN_PATH` pointed at its `bin` directory
- `make`
- `bash`
- `python`
- `pyelftools`

```bash
make
```

The build output lands in [`dist`](./dist):

- `ndvideo.tns`: calculator launcher
- `ndvideo.elf`: native ARM ELF
- `ndvideo.zehn`: raw Zehn payload

## Encode

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mp4" --output ".\movies\video.nvp"
```

With subtitles:

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --subtitle embedded --output ".\movies\video.nvp"
```

To preserve the source framerate instead of targeting a fixed one:

```powershell
python .\tools\encode_ndless_video.py "C:\path\to\video.mkv" --subtitle embedded --fps source --output ".\movies\video.nvp"
```

The encoder automatically fits the video into the configured canvas, converts it to RGB565, uses chunked inter-frame compression plus chunk-level zlib compression, and writes a single streamed `.nvp` file plus a JSON stats file.

## Install On Calculator

1. Build `ndvideo.tns`.
2. Encode your movie into a `.nvp` file.
3. If TI's desktop software refuses raw `.nvp` files, rename or copy them as `.nvp.tns` for transfer.
4. Copy `ndvideo.tns` and the movie file(s) into the same folder on the calculator.
5. Launch `ndvideo.tns` through Ndless.
6. Pick the movie and play it locally from calculator storage.

Current ready-to-copy test outputs in [`dist`](./dist):

- `ndvideo.tns`
- `cs2-30s.nvp`
- `cs2-30s.nvp.tns`
- `family-guy-30s-subs.nvp`
- `family-guy-30s-subs.nvp.tns`

No PC connection is needed during playback. The PC is only used to preprocess the source video into the calculator-friendly container.

## Current Limits

- No audio yet
- Subtitle rendering supports text tracks only
- The runtime is optimized for low RAM and predictable decode cost first; compression can be pushed harder later
