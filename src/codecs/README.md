# Codecs

- `h264bsd/`: legacy H.264 decoder used by version 9/10 `.nvp` files.
- `codec.h`: small runtime codec interface used by the player/movie layer.
- `mpeg4_xvid.c`: MPEG-4 Part 2 adapter used by version 11 codec-tagged `.nvp` files.
- `xvid/`: vendored Xvid decoder sources.

Player code should call codec adapters from this directory rather than reaching
into vendored decoder internals directly.
