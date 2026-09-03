# VLC Spanning Player for Ubuntu

This program creates ONE decorated, resizable X11 window covering the complete
X11 virtual desktop and embeds LibVLC into that window. The result is one
video surface spanning both monitors. Press `F` to switch that same window to
explicit combined-desktop fullscreen.

## Important limitation

`libvlc_media_player_set_xwindow()` is specifically an X11 embedding API.
Ubuntu GNOME normally uses Wayland on modern installations.

The program therefore attempts to use SDL2's X11 backend. If the session has
Xwayland available, run:

    SDL_VIDEODRIVER=x11 ./build/vlc_spanning /path/to/video.mp4

If Xwayland is not available, use an Xorg session.

## Controls

- F: toggle fullscreen across the combined desktop
- ESC: leave fullscreen, or quit when windowed
- SPACE: pause/resume
- Q: quit
- GNOME Alt+F7: move the normal window
- GNOME Alt+F8: resize the normal window

## Display modes

The default mode is `--fit`, which preserves the source aspect ratio and lets
LibVLC letterbox as needed. `--crop` adds VLC's crop-ratio option for the
configured target ratio, while `--stretch` sets the LibVLC display aspect
ratio to the target and may distort the source.

The default target is the exact ratio `2732:768`, not `32:9`.

LibVLC supplies the codec support, so the player accepts the formats handled
by the installed VLC build, including MP4, MKV, AVI, MOV, WebM, MPEG, TS,
M4V, FLV, OGG, and common 3GP/ASF variants. The control bar is translucent,
hover-driven, draggable, and its timeline supports click and drag seeking.

## Install dependencies

For Ubuntu 24.04:

    sudo apt update
    sudo apt install -y \
        build-essential \
        cmake \
        pkg-config \
        libsdl2-dev \
        libvlc-dev \
        libvlccore-dev \
        vlc \
        libx11-dev \
        libgtk-4-dev

Ubuntu's package repository provides libvlc-dev. Do not try to use
`find_package(VLC REQUIRED)` unless you have installed a separate CMake
package that actually exports a VLC CMake config.

## Build

From this directory:

    rm -rf build
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$(nproc)"

Check the executable:

    ./build/vlc_spanning --help

The program itself uses positional video input, so --help is expected to
print the usage line and exit.

## Run

Normal window, FIT mode:

    SDL_VIDEODRIVER=x11 ./build/vlc_spanning "$HOME/Videos/test.mp4"

With no argument, the installed launcher opens a welcome screen with a native
Open Video dialog. The same dialog is available through `Ctrl+O` while playing.

## Install the desktop application

    sudo cmake --install build

The latest preserved package artifact is `vlc-spanning_1.3_amd64.deb` in the
project root. The source is under `src/` and application resources are under
`resources/`.

This installs `vlc_spanning`, the `VLC Spanning Player` GNOME launcher, and an
original application icon. The launcher accepts files opened from GNOME Files
through its registered video MIME types.

CROP mode:

    SDL_VIDEODRIVER=x11 ./build/vlc_spanning --ratio 2732:768 --crop "$HOME/Videos/test.mp4"

STRETCH mode:

    SDL_VIDEODRIVER=x11 ./build/vlc_spanning --ratio 2732:768 --stretch "$HOME/Videos/test.mp4"

For a filename containing spaces, keep the quotes.

## Verify the desktop geometry first

Use:

    echo "$XDG_SESSION_TYPE"
    echo "$DISPLAY"
    echo "$WAYLAND_DISPLAY"
    xrandr --query

You want two displays visible in `xrandr`.

If `XDG_SESSION_TYPE=wayland`, that is not automatically a problem: the
program can run through Xwayland if the X11 backend is available.

## Why the original code was unreliable

1. The `SDL_Rect total_rect` started at {0,0,0,0}. This breaks layouts where
   monitors have negative X/Y coordinates, which is common when a monitor is
   positioned to the left or above the primary monitor.

2. `find_package(VLC REQUIRED)` is not a dependable Ubuntu LibVLC discovery
   method. The Ubuntu development package exposes LibVLC through pkg-config,
   so this project uses `pkg_check_modules(... libvlc)`.

3. `SDL_GetWindowID(window)` is an SDL window ID, not an explicitly retrieved
   native X11 Window handle. For LibVLC's X11 API, we retrieve the actual X11
   window through `SDL_GetWindowWMInfo()`.

4. LibVLC documents that `XInitThreads()` must be called before `libvlc_new()`
   and before Xlib is used. This project calls it before SDL initialization.

5. Native Wayland cannot be passed to `libvlc_media_player_set_xwindow()`.
   The code checks that SDL actually selected X11 and gives a clear error
   otherwise.

6. The original code did not attach the media to the player with
   `libvlc_media_player_set_media()`. This version does that explicitly.

7. The original display-bound calculation can produce incorrect width/height
   for non-zero or negative monitor origins. This version calculates the
   union using left/top/right/bottom coordinates.

## Aspect ratio vs. forced stretching

The default version preserves the video's aspect ratio. Therefore, if the
video aspect ratio does not match the combined monitor aspect ratio, VLC may
letterbox the video.

For a video that was specifically rendered for the exact combined display
resolution/aspect ratio, this is normally what you want.

If you deliberately want the video stretched to every pixel of the combined
window, you can change the LibVLC video-fit behavior, but that can visibly
distort the image. The best source video is one whose aspect ratio matches
the total spanning area.

## Monitor arrangement

Example:

Monitor 1: 1920x1080 at (0,0)
Monitor 2: 1920x1080 at (1920,0)

The program creates approximately:

    3840x1080 at (0,0)

If the second monitor is left of the primary:

Monitor 1: 1920x1080 at (1920,0)
Monitor 2: 1920x1080 at (0,0)

or another X11 arrangement, the union calculation handles the negative
coordinates correctly.

## If you get "SDL is not using X11"

Run:

    SDL_VIDEODRIVER=x11 ./build/vlc_spanning "$HOME/Videos/test.mp4"

Then inspect:

    echo "$DISPLAY"
    xrandr --query

If X11/Xwayland is unavailable, log into an Ubuntu "Ubuntu on Xorg"
session and run the same executable.

## Test LibVLC independently

Before debugging the C++ program, verify VLC itself can play the file:

    vlc "$HOME/Videos/test.mp4"

Then verify LibVLC is visible to pkg-config:

    pkg-config --modversion libvlc
    pkg-config --cflags --libs libvlc

Verify SDL2:

    pkg-config --modversion sdl2

## Expected architecture

    Monitor A ─┐
               │
               ├── X11 virtual desktop
               │
    Monitor B ─┘
                     ↓
              one SDL/X11 window
                     ↓
                 LibVLC
                     ↓
              one video surface

The program is NOT playing two independent VLC instances. It is one media
player and one window whose geometry spans the two displays.
