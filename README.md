# IslandBar

A floating split taskbar for Windows 11. It clips the real taskbar with `SetWindowRgn` into two rounded islands, one for apps and one for the tray. The island bounds come from the actual button rectangles read through UI Automation, so they stay correct with centered or left alignment, any number of apps, any DPI and several monitors.

## Run

```powershell
.\build\Release\islandbar.exe
```

- Opens the **Settings** window. Changes apply live and are saved automatically.
  - **Style:** split islands (dynamic, sized to the real buttons) or one single rounded bar.
  - **Tray island:** always shown, shown on hover, or hidden. Win+F2 toggles it.
  - **Widgets island:** app-side buttons standing apart from the main group (e.g. Widgets when centered) get their own island.
  - **Shape:** corner radius, top and bottom gap, and side spacing. A negative gap pushes that edge's corners off the window, so the bar sits flush against it.
  - **Extend taskbar when a window is maximised:** per monitor, the full taskbar comes back while a maximised window is visible.
  - **Extend during Alt+Tab / Task View.**
  - **Auto-hide:** the taskbar is clipped away until the mouse reaches it, or Start or a flyout opens.
  - **Start with Windows.**
- Keeps running in the notification area. Left-click the icon for Settings, right-click it for the menu (Enabled, Show tray island, Extend when maximised, Auto-hide, Reload config, Open config folder, Start with Windows, Exit).
- Launching it again while it is running just brings up the Settings window. This matters if you hid the tray island, because the icon then lives in a hidden area.
- `islandbar.exe --reset` removes the clip from every taskbar and exits. Use it if IslandBar was killed from Task Manager.
- `islandbar.exe --background` starts without opening Settings. "Start with Windows" uses this.

Config file: `%APPDATA%\IslandBar\config.json` (logical pixels, scaled per monitor DPI). Log file: `%APPDATA%\IslandBar\log.txt`.

## Build

Requirements: Visual Studio 2022 Build Tools (C++ workload and Windows 11 SDK) and CMake 3.21 or newer.

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100"
winget install --id Kitware.CMake

cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Diagnostics

```powershell
.\build\Release\uia-dump.exe --out dump.txt   # full UIA tree of every taskbar
.\build\Release\uia-dump.exe --islands        # the island rectangles IslandBar would use
```

After a Windows update breaks detection, compare a fresh dump with the identifiers in `src/core/match_rules.h`. All matching rules live in that one file.

## Layout

| Path | What |
|---|---|
| `src/core/match_rules.h` | every UIA identifier IslandBar depends on |
| `src/core/bounds.*` | UIA → island rectangles, with sanity checks |
| `src/core/region.*` | rounded regions, `SetWindowRgn`, clearing |
| `src/core/config.*`, `log.*` | config.json and the rolling log |
| `src/islandbar/engine.*` | taskbar discovery, WinEvent hooks, apply only on change |
| `src/islandbar/settings_window.*` | the Settings window |
| `src/islandbar/main.cpp` | tray icon, message loop, timers, safety nets |
| `src/uia-dump/` | diagnostic tool |

## Known limits

- Region corners are not antialiased (a `SetWindowRgn` limitation).
- Auto-hide is not supported.
- With left alignment the app island touches the screen edge, because the buttons themselves start at x=0.
- Tools that change taskbar composition, such as TranslucentTB, may conflict.
