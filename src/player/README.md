# Player

This directory contains the Ndless application shell: picker, playback loop,
input handling, UI drawing, history, subtitles, screenshots, and debug overlay.

The old single-file player has been split into separately compiled C modules.
`player_internal.h` is the private integration header for this layer,
`player_state.c` owns the globals that used to live in the monolithic source,
and codec open/reset/destroy/decode dispatch now goes through `MovieCodecOps`.

- `main.c`: application entry point and top-level SDL/font/SRAM setup
- `platform_debug.c`: platform/display hooks, debug logging, clocks, and path helpers
- `input_timing_memory.c`: pointer input, hover guards, memory stats, and frame timing
- `movie_resources.c`: movie lifetime, fonts, SRAM, and codec global init
- `codec_streaming.c`: chunk loading, prefetch, H.264/MPEG-4 decode, and seek preview decode
- `movie_open_scan.c`: `.nvp` opening, subtitle loading, file scanning, and picker cache model
- `subtitles.c`: subtitle layout, wrapping, caching, and drawing
- `render_primitives.c`: RGB565 drawing primitives, theme palette, panels, text metrics, and video rects
- `playback_ui.c`: playback UI animation, badges, help, progress rendering, and movie rendering
- `picker_ui.c`: picker row layout, transitions, loading animation, and picker rendering
- `history_screenshots.c`: history/theme persistence, screenshots, and seek-bar hover preview
- `picker_loop.c`: interactive movie picker loop
- `resume_prompt.c`: resume prompt drawing and selection
- `playback_loop.c`: movie playback loop

The split is intentionally conservative: behavior-facing code stayed close to
the original ordering, while common movie state moved to `src/movie/movie.h`
and codec-specific state lives behind the H.264 and MPEG-4 decoder contexts.
