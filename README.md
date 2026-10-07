# Pivot

Menu bar / tray utility for monitor **rotation** and **arrangement** — built
for monitor arms that move a lot. One repo, one native app per OS: zero
dependencies, no polling, ~0% CPU idle.

|                   | macOS (`macos/`)                              | Windows (`windows/`)                          |
|-------------------|-----------------------------------------------|-----------------------------------------------|
| UI                | menu-bar extra (`LSUIElement`, AppKit)        | tray icon (`Shell_NotifyIcon`, Win32)         |
| Rotation          | MonitorPanel SPI via ObjC shim (`MPShim.m`)   | `ChangeDisplaySettingsEx` + `dmDisplayOrientation` |
| Arrangement       | `CGConfigureDisplayOrigin` (permanent)        | `ChangeDisplaySettingsEx` + `dmPosition` (permanent) |
| Rotation gating   | `canChangeOrientation` disables items         | `CDS_TEST` probe disables items               |
| Refresh           | `didChangeScreenParametersNotification`       | `WM_DISPLAYCHANGE` (debounced)                |
| Build             | `./build.sh` (swiftc + clang)                 | `.\build.ps1` (MinGW g++ or MSVC cl)          |
| Install           | drag `Pivot-1.0.dmg` app to Applications      | `.\build.ps1 -Install` (login item)           |

## Layout

```text
Pivot/
  macos/     Swift + ObjC shim, Info.plist, build.sh — see macos/README.md
  windows/   single-file Win32 C++17, build.ps1 — see windows/README.md
  .github/workflows/build.yml   CI: builds + uploads both apps
```

## Behavior contract

Keep the ports in sync:

- Tray/menu-bar menu: status line, one submenu per display
  (`name — WxH, DEG°`), rotation items 0° / 90° / 180° / 270° with the current
  one checked and unsupported ones disabled, a disabled `Place at x, y` row,
  then Arrange, Settings, Quit.
- Arrange canvas: drag rectangles to reposition, 24 px edge snap,
  double-click or right-click a display to rotate, primary display emphasized.
- Rotation applies instantly and persists; failures degrade gracefully
  (tray balloon on Windows) and never break arrangement.
- No polling: refresh on OS display-change events plus user actions only.
- CLI parity where it makes sense (`windows`: `--list`, `--rotate N DEG`).
