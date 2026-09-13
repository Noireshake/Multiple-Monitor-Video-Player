# VLC Spanning Player for macOS

VLC Spanning Player creates one Cocoa window and one LibVLC video surface across two selected displays. It supports local media, HTTP(S) URLs, YouTube playback through yt-dlp, display selection, FIT/CROP/STRETCH modes, quality limits, playback controls, seeking, fullscreen, audio tracks, subtitles, and a transparent floating control bar.

The macOS implementation is under `OSX/`. The application uses one LibVLC instance, one media player, and one native window. It does not create separate players or video windows per monitor.

## Requirements

- macOS 10.11 or newer
- CMake 3.20 or newer
- Xcode Command Line Tools
- VLC installed at `/Applications/VLC.app`
- yt-dlp available as `yt-dlp`, `/usr/local/bin/yt-dlp`, or `/opt/homebrew/bin/yt-dlp`

The build currently targets x86_64. Apple Silicon can run it through Rosetta unless an arm64 or universal LibVLC build is supplied.

## Build and Run

From the repository root:

```bash
./run.sh --help
./run.sh /path/to/video.mp4
./run.sh 'https://www.youtube.com/watch?v=VIDEO_ID'
```

The launcher configures and incrementally builds the single canonical `build-macos` directory before launching. To run the existing binary without rebuilding:

```bash
VLC_SPANNING_NO_BUILD=1 ./run.sh /path/to/video.mp4
```

Direct build:

```bash
cmake -S OSX -B build-macos -DCMAKE_BUILD_TYPE=Release
cmake --build build-macos --config Release --parallel 2
```

Supported options include `--fit`, `--crop`, `--stretch`, `--ratio W:H`, and `--quality auto|2160|1440|1080|720|480|360|240`.

## Package the App

The packaging script bundles LibVLC, its plugins and data, the application icon, and the signed app bundle into `dist-macos`:

```bash
OSX/package-macos.sh
```

Create an installable compressed DMG:

```bash
MAKE_DMG=1 OSX/package-macos.sh
```

The output is `VLC-Spanning-macOS.dmg`. The default signing identity is ad hoc. Set `SIGNING_IDENTITY` to a Developer ID Application certificate for distribution and notarization.

## YouTube Playback

YouTube URLs are resolved by yt-dlp before being passed to LibVLC. The resolver prefers H.264/MP4 video with M4A audio, supports separate video and audio streams, and applies the selected maximum video height. Keep URLs containing `&` quoted.

If extraction fails, update yt-dlp:

```bash
brew upgrade yt-dlp
```

## Controls

- `F`: toggle fullscreen across the selected displays
- `Esc`: leave fullscreen, or quit when windowed
- `Space`: pause or resume
- `Q`: quit
- `M`: mute
- Arrow keys: seek and change volume
- `Cmd+O`: open a local file or URL
- Right-click: choose audio and subtitle tracks

## Validation

The command-line build, app packaging, icon embedding, code-signature verification, and DMG generation have been validated. A physical two-display playback test is still required to verify the complete rendered frame, mouse interaction, fullscreen behavior, and monitor arrangement on the target Mac.

See [MACOS_HANDOFF.md](MACOS_HANDOFF.md) for implementation details and handoff notes.
