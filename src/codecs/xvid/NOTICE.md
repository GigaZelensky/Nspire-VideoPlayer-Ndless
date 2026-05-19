# Xvid MPEG-4 Part 2 Decoder

The sources in this directory are vendored Xvid decoder sources.

They are derived from the Xvid MPEG-4 video codec project. Preserve the
upstream copyright and license notices in each file and in `LICENSE` when
modifying or redistributing this tree.

This project uses the decoder side only, through `src/codecs/mpeg4_xvid.c`.
The local build defines `XVID_DECODER_ONLY`, so the public encoder entry point
is stubbed out and the upstream encoder sources/plugins are not compiled.
