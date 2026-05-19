# Movie Container

This directory owns the `.nvp` container contract and common movie runtime
state shared by the player and codec adapters.

- `nvp_format.h`: packed on-disk header/index structs, version numbers, and
  codec-tag constants.
- `movie.h`: in-memory `Movie` state, subtitle/chunk caches, and per-codec
  decoder contexts.

Keep wire-format structs, chunk index layout, and codec identity here instead
of embedding them in the player UI loop.
