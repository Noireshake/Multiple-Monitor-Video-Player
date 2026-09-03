#!/usr/bin/env bash
set -euo pipefail

VIDEO="${1:-}"

if [[ -n "$VIDEO" && ! -f "$VIDEO" ]]; then
    echo "Video not found: $VIDEO"
    exit 1
fi

if [[ ! -x "./build/vlc_spanning" ]]; then
    echo "Executable not found. Build first:"
    echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
    echo "  cmake --build build -j\"$(nproc)\""
    exit 1
fi

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"

echo "SDL_VIDEODRIVER=$SDL_VIDEODRIVER"

if [[ -z "$VIDEO" ]]; then
    echo "Starting VLC Spanning Player"
    exec ./build/vlc_spanning
fi

echo "Playing: $VIDEO"

exec ./build/vlc_spanning "$VIDEO"
