#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
APP="$SCRIPT_DIR/build/vlc_spanning"
VIDEO="${1:-}"

if [[ -n "$VIDEO" && ! "$VIDEO" =~ ^https?:// && ! -f "$VIDEO" ]]; then
    echo "Video not found: $VIDEO"
    exit 1
fi

if [[ ! -x "$APP" ]]; then
    echo "Executable not found. Build first:"
    echo "  cmake -S \"$SCRIPT_DIR\" -B \"$SCRIPT_DIR/build\" -DCMAKE_BUILD_TYPE=Release"
    echo "  cmake --build \"$SCRIPT_DIR/build\" -j\"$(nproc)\""
    exit 1
fi

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"

echo "SDL_VIDEODRIVER=$SDL_VIDEODRIVER"

if [[ -z "$VIDEO" ]]; then
    echo "Starting VLC Spanning Player"
    exec "$APP"
fi

echo "Playing: $VIDEO"

exec "$APP" "$VIDEO"
