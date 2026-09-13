#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-macos}"
STAGE_DIR="${STAGE_DIR:-$ROOT_DIR/dist-macos}"
VLC_ROOT="${VLC_ROOT:-/Applications/VLC.app/Contents/MacOS}"
SIGNING_IDENTITY="${SIGNING_IDENTITY:--}"
APP_NAME="vlc_spanning_macos.app"
APP="$STAGE_DIR/$APP_NAME"

if [[ ! -f "$VLC_ROOT/lib/libvlc.dylib" ]]; then
    echo "LibVLC runtime not found under $VLC_ROOT" >&2
    exit 1
fi

cmake -S "$ROOT_DIR/OSX" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$STAGE_DIR"
cmake --build "$BUILD_DIR" --config Release
rm -rf "$STAGE_DIR"
cmake --install "$BUILD_DIR"

mkdir -p "$APP/Contents/Frameworks"
cp "$VLC_ROOT/lib/"*.dylib "$APP/Contents/Frameworks/"
cp -R "$VLC_ROOT/plugins" "$APP/Contents/"
cp -R "$VLC_ROOT/share" "$APP/Contents/"
# The source cache contains absolute paths and becomes stale after dylib rewriting.
rm -f "$APP/Contents/plugins/plugins.dat"

for library in "$APP/Contents/Frameworks/"*.dylib; do
    name="$(basename "$library")"
    install_name_tool -id "@rpath/$name" "$library"
done
install_name_tool -add_rpath "@executable_path/../Frameworks" \
    "$APP/Contents/MacOS/vlc_spanning_macos" 2>/dev/null || true

codesign --force --deep --sign "$SIGNING_IDENTITY" "$APP"
codesign --verify --deep --strict "$APP"

if [[ "${MAKE_DMG:-0}" == "1" ]]; then
    hdiutil create -volname "VLC Spanning Player" -srcfolder "$APP" \
        -ov -format UDZO "$ROOT_DIR/VLC-Spanning-macOS.dmg"
fi

echo "Packaged: $APP"
echo "Signing identity: $SIGNING_IDENTITY"
