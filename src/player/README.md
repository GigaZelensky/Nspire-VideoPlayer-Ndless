# Player

This directory contains the Ndless application shell: picker, playback loop,
input handling, UI drawing, history, subtitles, screenshots, and debug overlay.

`player_internal.h` is the private integration header for this layer,
`player_state.c` owns shared state, and codec open/reset/destroy/decode dispatch
goes through `MovieCodecOps`.

- `main.c`: application entry point and top-level SDL/font/SRAM setup
- `platform_debug.c`: platform/display hooks, debug logging, clocks, and path helpers
- `input_timing_memory.c`: pointer input, hover guards, memory stats, and frame timing
- `movie_resources.c`: movie lifetime, fonts, SRAM, and codec global init
- `codec_streaming.c`: chunk loading, prefetch, color conversion, and seek preview decode
- `video_decoder.c`: shared decoder stepping, planar output and HEVC/AV1 seek handling
- `video_lookahead.c`: shared H.264/HEVC/AV1 decode-ahead queue
- `movie_open_scan.c`: `.nvp` opening, subtitle loading, file scanning, and picker cache model
- `subtitles.c`: subtitle layout, wrapping, caching, and drawing
- `render_primitives.c`: RGB565 drawing primitives, theme palette, panels, text metrics, and video rects
- `playback_ui.c`: playback UI animation, badges, help, progress rendering, and movie rendering
- `picker_ui.c`: picker row layout, transitions, loading animation, and picker rendering
- `history_screenshots.c`: history/theme persistence, screenshots, and seek-bar hover preview
- `picker_loop.c`: interactive movie picker loop
- `resume_prompt.c`: resume prompt drawing and selection
- `playback_loop.c`: movie playback loop
- `playback_seek.c`: resumable seeks and input during catch-up

Common movie state lives in `src/movie/movie.h`. Decoder contexts own the
H.264, HEVC, AV1 and MPEG-4 state; multi-codec builds keep the corresponding
module loaded until every playback and preview context has been released.
HEVC and AV1 buffer owned YUV pictures behind a small RGB front queue, using
measured free RAM to set capacity.
