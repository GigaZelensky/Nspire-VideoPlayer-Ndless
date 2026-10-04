# Codecs

- `h264bsd/`: H.264 decoder, including ARM interpolation routines from Kinoma.
- `hevc_decoder.cpp` and `hevc/`: cooperative H.265 decoder and vendored libde265 sources.
- `av1_decoder.c` and `av1/`: cooperative AV1 decoder and vendored dav1d sources.
- `video_frame.h`: shared planar frame view for HEVC and AV1.
- `codec.h`: small runtime codec interface used by the player/movie layer.
- `mpeg4_xvid.c`: MPEG-4 Part 2 adapter used by version 11 codec-tagged `.nvp` files.
- `xvid/`: vendored Xvid decoder sources.

Player code should call codec adapters from this directory rather than reaching
into vendored decoder internals directly.

Multi-codec builds append relocatable decoder images to the same player file.
The host loads and verifies an image when its first decoder is created; live
playback and preview contexts keep it loaded. Switching codecs releases idle
images. Modules use host-owned allocation and SRAM through the C interfaces in
`modules/`; decoder calls never read code from storage. Single-codec builds
link their decoder directly.

## HEVC integration

The libde265 port accepts Main 8-bit 4:2:0 I/P pictures up to 320x240, with
CTU32/64, no reordering, at most three decoded pictures, and no tiles or WPP.
CTU16 is rejected because the upstream filter path produced a chroma mismatch.
Each submitted Annex-B access unit is copied and limited to 256 KiB and 64 NALs.

Decoding yields at CTU boundaries without OS threads. Output planes remain valid
until released. Main playback and previews can share temporary SRAM at those
boundaries; reference pictures and entropy state stay private. Allocation falls
back to the heap when SRAM is unavailable. Build flags are in the Makefile.

## AV1 integration

The dav1d port accepts Main 8-bit 4:2:0 pictures up to 320x240. Each packet
contains one visible frame; hidden frames, extra layers and film grain are
rejected. Input OBUs are copied and limited to 256 KiB and 64 OBUs per frame.
The encoder emits matching streams with repeated sequence headers at keyframes.

Decoding yields after complete superblocks while keeping prediction and entropy
state private. Partially reconstructed pictures stay hidden, and completed
planes remain valid until released. Playback and previews use separate decoder
instances; neither starts OS threads. Only 8-bit sources are built, with ARM926
routines for common motion filters.
