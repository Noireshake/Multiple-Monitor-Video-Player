# VLC Spanning Player macOS Handoff

## Current implementation

- macOS source: `OSX/main.mm`
- macOS build: `OSX/CMakeLists.txt`
- packaging: `OSX/package-macos.sh`
- launcher: `run.sh`
- one Cocoa window, one LibVLC instance, one media player, one video surface
- LibVLC video is embedded through `libvlc_media_player_set_nsobject()`
- display selection, FIT/CROP/STRETCH, ratio, YouTube quality, controls, and fullscreen are in `OSX/main.mm`

## Canonical build and run

```bash
./run.sh --help
./run.sh /path/to/video.mp4
```

On macOS, `run.sh` always configures and incrementally rebuilds `build-macos`
before launching it. This prevents old experimental build directories from
being selected accidentally. To run an already-built binary without rebuilding:

```bash
VLC_SPANNING_NO_BUILD=1 ./run.sh /path/to/video.mp4
```

Direct build:

```bash
cmake -S OSX -B build-macos -DCMAKE_BUILD_TYPE=Release
cmake --build build-macos --config Release --parallel 2
```

Package:

```bash
OSX/package-macos.sh
```

## Known validation boundary

CLI compilation and packaging have been validated. A physical two-display
playback test is still required to verify the complete rendered frame, mouse
button interaction, settings application, fullscreen, and display arrangement.
Use a test video with a visible border and verify it after initial launch and
after applying Player Settings.

## Pre-fix handoff - 2026-09-11

The current macOS build has two visual issues reported during playback:

	configured spanning target ratio, which can leave black bands above and
	below the video in the app window.

The fix should make FIT fill the configured video border using the selected
target ratio, and make the media bar background more transparent while keeping
its controls readable. Validate with a bordered test video after launch and
after applying Player Settings.

### Fix applied

- macOS FIT mode now applies the configured target aspect ratio through LibVLC,
  filling the video host instead of preserving source-aspect letterboxing.
- The floating media bar alpha is now `0.58` so the video remains visible
  beneath it while the HUD controls stay readable.
- The macOS build completed successfully with `./run.sh --help`.

### Follow-up verification

On a two-display setup, launch a video with a visible frame and confirm that
FIT reaches the video host border without black bands above or below. Open
Player Settings, apply a different target ratio, and confirm the frame updates.
Move the pointer over the media bar and confirm the underlying video is more
visible than before while all controls remain legible.

## Important constraints

Do not replace LibVLC, Cocoa embedding, or the single-player architecture.
Preserve the bundled LibVLC runtime lookup and `@rpath` packaging behavior.