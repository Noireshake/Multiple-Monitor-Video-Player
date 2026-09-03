# VLC Spanning Player: AI Development Context

You are modifying an existing Ubuntu C++17 project. Preserve the working architecture and make focused, reversible changes.

## Product

VLC Spanning Player is a LibVLC 3.x video player that renders one video into one SDL2/X11 window spanning all detected monitors. The target machine uses GNOME Shell on Wayland through XWayland:

- SDL video backend: X11
- LibVLC: 3.0.x
- Two monitors: 1366x768 at (0,0) and 1366x768 at (1366,0)
- Virtual desktop: 2732x768
- Canonical target ratio: 2732:768, decimal 3.557292, not exactly 32:9

## Source Layout

- `src/main.cpp`: application lifecycle, SDL window, LibVLC player, display geometry, EWMH fullscreen, keyboard events
- `src/media_controls.cpp`: stable X11 child media bar, Xlib drawing, hover timeout, mouse buttons, bar dragging, timeline seeking, GTK4 native file dialog
- `src/media_controls.hpp`: media-bar public interface
- `resources/com.noirshake.VLCSpanning.desktop`: GNOME launcher and video MIME associations
- `resources/com.noirshake.VLCSpanning.svg`: original application icon
- `CMakeLists.txt`: CMake, pkg-config dependencies, install rules, CPack DEB configuration
- `run.sh`: development launcher; accepts an optional video path or starts the welcome path
- `vlc-spanning_1.3_amd64.deb`: preserved latest Debian artifact; regenerate after source changes

## Non-Negotiable Architecture

Keep one SDL window, one native X11 Window, one LibVLC instance, one media player, and one video output. Do not replace LibVLC, SDL2, X11 embedding, or the working fullscreen implementation with Qt, GTK video rendering, OpenGL video, multiple windows, or multiple VLC processes.

The LibVLC embedding order is important: call `XInitThreads()`, initialize SDL with X11, create the SDL window, obtain the native X11 Window through `SDL_SysWMinfo`, create LibVLC/media/player, call `libvlc_media_player_set_xwindow()`, then play.

## Fullscreen Rules

F and the media-bar fullscreen action must use the same function. Do not use `SDL_SetWindowFullscreen`, `SDL_WINDOW_FULLSCREEN`, or `SDL_WINDOW_FULLSCREEN_DESKTOP`.

Fullscreen must send X11 EWMH ClientMessages in this order:

1. `_NET_WM_FULLSCREEN_MONITORS` with the full detected monitor index range. For two monitors: top=0, bottom=1, left=0, right=1.
2. `_NET_WM_STATE` ADD/REMOVE for `_NET_WM_STATE_FULLSCREEN`.

Fullscreen must retain the same X11 drawable and LibVLC player. Save normal window geometry before entering fullscreen, and restore native decorations plus the saved normal position/size after leaving fullscreen. GNOME/XWayland processes these requests asynchronously, so log actual geometry and do not claim physical behavior without testing on the target desktop.

## Media Behavior

Default mode is FIT. Preserve source aspect ratio unless the requested mode is STRETCH. CROP uses the LibVLC 3-compatible media crop option. The configured ratio is rational width/height data and must not be silently changed to 32:9.

The stable media bar is an X11 child of the video window. It must remain a single surface above the video, appear near the bottom by default, show on mouse activity, hide after roughly 2.5 seconds of inactivity, remain visible while dragging, and support direct mouse input. Avoid introducing a second top-level overlay or a GTK widget overlay over LibVLC.

The timeline track must support click-to-seek and drag-to-seek. Programmatic playback updates must not recursively call LibVLC seek or cause playback timestamp churn.

GTK4 is used only for the native Open File dialog. LibVLC remains the authoritative media pipeline. It should accept paths selected from the dialog and reuse the existing player instead of recreating the window or LibVLC instance.

## Controls

Preserve: F fullscreen, ESC fullscreen exit/windowed quit, SPACE play/pause, Q quit. Existing useful shortcuts include Ctrl+O, M mute, Left/Right seek, Up/Down volume.

## Window Behavior

Normal mode must be a real managed, decorated, resizable GNOME window with native close/minimize/maximize controls. Do not use override-redirect or always-on-top for the main video window. The close protocol must route to SDL event handling and the normal cleanup path.

## Build and Package

Required dependencies are SDL2, libvlc, X11, and GTK4, discovered through pkg-config/CMake. Build with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cmake --build build --target package
```

The package is generated as `build/vlc-spanning_1.3_amd64.deb` unless the project version changes. Validate with `dpkg-deb --info`, `dpkg-deb --contents`, `desktop-file-validate`, and editor diagnostics. Install using `sudo apt install ./build/vlc-spanning_1.3_amd64.deb` or copy it to `/tmp` first to avoid the `_apt` home-directory sandbox notice.

Do not delete or overwrite unrelated user changes. Do not commit, create ZIP files, or create duplicate projects. Report which physical GNOME/two-monitor tests were not possible in the current environment.
