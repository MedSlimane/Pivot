# Pivot for Windows

Tray utility for monitor **rotation** and **arrangement** — Windows port of the
macOS [Pivot](../macos) app. Native Win32 C++17, zero dependencies, ~0% CPU idle.

- Tray icon with per-display rotation submenu: 0° / 90° / 180° / 270°, applied instantly
- **Arrange window**: "Arrange Displays..." lets you drag display rectangles to
  reposition them; double-click (or right-click) a display to rotate it.
  Edges snap when dropped close together.
- "Display Settings..." shortcut for everything else
- CLI for scripting: `Pivot.exe --list`, `Pivot.exe --rotate N DEGREES`

## How it works

- **Rotation** — `ChangeDisplaySettingsEx` + `DEVMODE.dmDisplayOrientation`
  (width/height swapped on 90°/270°), same mechanism as Display Settings.
- **Arrangement** — `ChangeDisplaySettingsEx` + `DEVMODE.dmPosition`
  (`CDS_UPDATEREGISTRY`), permanent across reboots.
- **Rotation gating** — each display is probed with `CDS_TEST`; displays the
  driver won't rotate show disabled rotation items (like `canChangeOrientation`
  on macOS). Laptop panels typically can't rotate; USB/DP monitors usually can.
- **Zero polling** — UI refreshes only on `WM_DISPLAYCHANGE` (debounced) and
  your actions. No timers, no loops.
- Per-monitor-V2 DPI aware, so canvas coordinates are real device pixels.

## Build & run

```powershell
winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT  # MinGW g++, one time
.\build.ps1              # -> Pivot.exe
.\Pivot.exe              # tray app (single instance)
```

MSVC (`cl`) also works — `build.ps1` auto-detects it if `g++` is missing.

## Install

```powershell
.\build.ps1 -Install     # copies to %LOCALAPPDATA%\Pivot + runs at login
.\build.ps1 -Uninstall   # removes both
```

This writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Pivot`.
To start manually instead, drop a shortcut in `shell:startup`.

## Notes

- Rotation applies immediately and persists (`CDS_UPDATEREGISTRY`).
- If rotation fails, the tray shows a balloon with the driver error code and
  everything else keeps working.
- Primary display is drawn with a thicker border and tagged in its label.
- `--rotate` uses the 0-based index from `--list`.
