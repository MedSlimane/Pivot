#!/bin/zsh
set -euo pipefail
cd "$(dirname "$0")"

APP="Pivot.app"
BIN="$APP/Contents/MacOS/Pivot"

mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" build
cp Info.plist "$APP/Contents/Info.plist"
/bin/echo -n "APPL????" > "$APP/Contents/PkgInfo"

ICON_SRC="Assets/icon-1024.png"
if [[ ! -f "$ICON_SRC" ]]; then
  (cd Assets && swift make-icon.swift)
fi
if [[ -f "$ICON_SRC" ]]; then
  ICONSET="$(mktemp -d)/PivotIcon.iconset"
  mkdir -p "$ICONSET"
  sips -z 16 16     "$ICON_SRC" --out "$ICONSET/icon_16x16.png" >/dev/null
  sips -z 32 32     "$ICON_SRC" --out "$ICONSET/icon_16x16@2x.png" >/dev/null
  sips -z 32 32     "$ICON_SRC" --out "$ICONSET/icon_32x32.png" >/dev/null
  sips -z 64 64     "$ICON_SRC" --out "$ICONSET/icon_32x32@2x.png" >/dev/null
  sips -z 128 128   "$ICON_SRC" --out "$ICONSET/icon_128x128.png" >/dev/null
  sips -z 256 256   "$ICON_SRC" --out "$ICONSET/icon_128x128@2x.png" >/dev/null
  sips -z 256 256   "$ICON_SRC" --out "$ICONSET/icon_256x256.png" >/dev/null
  sips -z 512 512   "$ICON_SRC" --out "$ICONSET/icon_256x256@2x.png" >/dev/null
  sips -z 512 512   "$ICON_SRC" --out "$ICONSET/icon_512x512.png" >/dev/null
  cp "$ICON_SRC" "$ICONSET/icon_512x512@2x.png"
  iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/PivotIcon.icns"
  rm -rf "$(dirname "$ICONSET")"
  echo "Icon: $APP/Contents/Resources/PivotIcon.icns"
fi

# Rotation shim (MonitorPanel, private — runtime-loaded) + AppKit UI.
clang -fobjc-arc -O -c Sources/MPShim.m -o build/MPShim.o
swiftc -O -whole-module-optimization \
  Sources/main.swift build/MPShim.o \
  -o "$BIN" -framework AppKit -framework CoreGraphics

codesign --force --deep --sign - "$APP" 2>/dev/null || true
echo "Built: $BIN ($(du -h "$BIN" | cut -f1))"

if [[ "${1:-}" == "--dmg" ]]; then
  DMG="Pivot-1.0.dmg"
  rm -f "$DMG"
  create-dmg \
    --volname "Pivot" \
    --volicon "$APP/Contents/Resources/PivotIcon.icns" \
    --window-size 660 400 \
    --icon-size 110 \
    --icon "$APP" 170 180 \
    --app-drop-link 490 180 \
    "$DMG" \
    "$APP"
  echo "DMG: $DMG ($(du -h "$DMG" | cut -f1))"
fi
