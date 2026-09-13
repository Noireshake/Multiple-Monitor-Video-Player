#!/bin/bash
set -euo pipefail

export PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:${PATH:-}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VIDEO="${1:-}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This project contains the macOS build only." >&2
    exit 1
fi

BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build-macos}"
if [[ "${VLC_SPANNING_NO_BUILD:-0}" != "1" ]]; then
    cmake -S "$SCRIPT_DIR/OSX" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR" --config Release --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
fi

APP="$BUILD_DIR/vlc_spanning_macos.app/Contents/MacOS/vlc_spanning_macos"
if [[ ! -x "$APP" ]]; then
    echo "macOS executable not found after build: $APP" >&2
    exit 1
fi
echo "Using macOS build: $APP"

if [[ -n "$VIDEO" && "$VIDEO" != "--help" && ! "$VIDEO" =~ ^https?:// && ! -f "$VIDEO" ]]; then
    echo "Video not found: $VIDEO"
    exit 1
fi

if [[ ! -x "$APP" ]]; then
    echo "Executable not found. Build first:"
    echo "  cmake -S \"$SCRIPT_DIR/OSX\" -B \"$SCRIPT_DIR/build-macos\" -DCMAKE_BUILD_TYPE=Release"
    echo "  cmake --build \"$SCRIPT_DIR/build-macos\" --config Release"
    exit 1
fi

if [[ -z "$VIDEO" ]]; then
    echo "Starting VLC Spanning Player"
    exec "$APP"
fi

if [[ "$VIDEO" == "--help" ]]; then
    exec "$APP" --help
fi

echo "Playing: $VIDEO"

exec "$APP" "$VIDEO"
