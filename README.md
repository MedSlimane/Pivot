# Pivot

Menu bar utility for monitor **rotation** and **arrangement** — built for
monitor arms that move a lot.

- Menu bar icon (SF Symbol `rectangle.landscape.rotate`)
- Per-display rotation submenu: 0° / 90° / 180° / 270°, applied instantly
- **Arrange canvas**: open "Arrange Displays…" and drag display rectangles
  to reposition them; double-click (or right-click) a display to rotate it.
  Edges snap when dropped close together.
- "Displays Settings…" shortcut for everything else

## How it works

- **Rotation** — MonitorPanel private framework (`MPDisplay setOrientation:`,
  probed live on this machine, works under SIP, no permissions). Applied
  immediately, exactly what System Settings uses under the hood.
- **Arrangement** — public CoreGraphics API
  (`CGBeginDisplayConfiguration` + `CGConfigureDisplayOrigin` +
  `CGCompleteDisplayConfiguration`, permanent), same as `displayplacer`.
- **Zero polling** — the UI refreshes only on
  `NSApplicationDidChangeScreenParametersNotification` (WindowServer event)
  and your actions. No timers, no loops, blocked on RunLoop otherwise.
- Pure AppKit, `LSUIElement` (no Dock icon), no SwiftUI, no dependencies.
  ~0% CPU idle.

## Build & run

```sh
./build.sh           # app only
./build.sh --dmg     # app + drag-and-drop installer (Pivot-1.0.dmg)
open Pivot.app
```

## Install

Open `Pivot-1.0.dmg` and drag Pivot into Applications.
Login at boot: Settings → General → Login Items → + → Pivot.

## Notes

- `canChangeOrientation` is honored: displays that can't rotate show their
  rotation items disabled (and double-click beeps).
- The binary calls the rotation SPI through a tiny ObjC shim
  (`Sources/MPShim.m`) that runtime-loads MonitorPanel; if Apple ever removes
  it, rotation items fail gracefully while arrangement keeps working.
