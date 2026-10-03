# Codecs

- `h264bsd/`: H.264 decoder, including ARM interpolation routines from Kinoma.
- `hevc_decoder.cpp` and `hevc/`: cooperative H.265 decoder and vendored libde265 sources.
- `codec.h`: small runtime codec interface used by the player/movie layer.
- `mpeg4_xvid.c`: MPEG-4 Part 2 adapter used by version 11 codec-tagged `.nvp` files.
- `xvid/`: vendored Xvid decoder sources.

Player code should call codec adapters from this directory rather than reaching
into vendored decoder internals directly.

## HEVC integration

The libde265 port accepts Main 8-bit 4:2:0 I/P pictures up to 320x240, with
CTU32/64, no reordering, at most three decoded pictures, and no tiles or WPP.
CTU16 is rejected because the upstream filter path produced a chroma mismatch.
Each submitted Annex-B access unit is copied and limited to 256 KiB and 64 NALs.

Decoding yields at CTU boundaries without OS threads. Output planes remain valid
until released. Main playback and previews can share temporary SRAM at those
boundaries; reference pictures and entropy state stay private. Allocation falls
back to the heap when SRAM is unavailable. Build flags are in the Makefile.
