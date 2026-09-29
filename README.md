# FloatBar: IslandBar for Windows 11

IslandBar turns the native Windows 11 taskbar into floating, rounded **islands**: one for your apps and one for the tray (icons and clock), with the desktop visible in between. It works with centered and left alignment, any number of open apps, every DPI scale and multiple monitors, and it keeps up with changes as they happen.

It doesn't replace or patch the taskbar. It reads where the real buttons are and clips the real taskbar window around them, so everything keeps working exactly like Windows intends: Start, search, jump lists, drag and drop, thumbnails, tray flyouts.

---

## Contents

- [Features](#features)
- [Download and verify](#download-and-verify)
- [Quick start](#quick-start)
- [Settings reference](#settings-reference)
- [Custom colours, gradients and smooth corners](#custom-colours-gradients-and-smooth-corners)
- [How it works](#how-it-works)
- [Privacy, logging and debug reports](#privacy-logging-and-debug-reports)
- [Troubleshooting](#troubleshooting)
- [Known limitations](#known-limitations)
- [Building from source](#building-from-source)
- [Project layout](#project-layout)
- [License](#license)

---

## Features

| | |
|---|---|
| **Split islands** | App island (Start, search, task view, pinned and running apps) and tray island (hidden-icons chevron, tray icons, clock), each sized to the actual buttons. |
| **Single bar** | Alternatively, one rounded bar across the whole taskbar, inset from the screen edges. |
| **Widgets island** | Buttons that sit apart from the main group (such as Widgets at the far left when centered) get their own island instead of stretching the app island. |
| **Tray on hover** | Show the tray island always, only while the mouse is over the taskbar, or never. **Win+F2** toggles it. |
| **Shape** | Corner radius, top gap, bottom gap and side spacing. A negative gap pushes that edge's corners off-screen for a flat, docked edge. |
| **Extend when maximised** | While a maximised window is on a monitor, that monitor's taskbar returns to full width. |
| **Extend during Alt+Tab / Task View** | Full width while switching windows. |
| **Auto-hide** | The taskbar disappears until the mouse reaches it, or until Start, search or a tray flyout opens. |
| **Custom background** | Solid colour or gradient (4 directions), opacity, border width, colour and opacity, all drawn with antialiased corners. Needs [TranslucentTB](#custom-colours-gradients-and-smooth-corners). |
| **Multi-monitor** | Every taskbar is handled separately; secondary taskbars get an app island and a clock island when the clock is shown there. |
| **Per-monitor DPI** | All sizes are in logical pixels and scaled for each monitor. |
| **Safe by design** | Restores the normal taskbar on exit, sign-out, crash, or when detection fails. `islandbar.exe --reset` fixes anything left behind. |
| **Diagnostics** | Opt-in verbose logging and a privacy-redacted debug report you can attach to an issue. |

## Download and verify

Get the latest release from the [Releases page](https://github.com/Zyneq/FloatBar/releases). Each release contains:

| File | What it is |
|---|---|
| `IslandBar-<version>-win-x64.zip` | `islandbar.exe`, `uia-dump.exe`, this README and the license |
| `islandbar.exe` | the app (single file, no installer, no runtime needed) |
| `uia-dump.exe` | diagnostic tool, see [Troubleshooting](#troubleshooting) |
| `SHA256SUMS.txt` | SHA-256 checksum of every file above |

Releases are built by the public GitHub Actions workflow in [`.github/workflows/release.yml`](.github/workflows/release.yml), straight from the tagged source. Nobody builds them by hand. You can check that in two ways.

**1. Checksum:** the hash must match the line in `SHA256SUMS.txt`.

```powershell
Get-FileHash .\islandbar.exe -Algorithm SHA256
```

**2. Build provenance:** a signed attestation proving the file was produced by this repository's workflow. It needs the [GitHub CLI](https://cli.github.com).

```powershell
gh attestation verify .\islandbar.exe --repo Zyneq/FloatBar
```

The binaries are not code-signed with a paid certificate, so Windows SmartScreen may warn on first launch ("Windows protected your PC" → *More info* → *Run anyway*). The two checks above are how you confirm the file is genuine.

## Quick start

1. Put `islandbar.exe` anywhere, for example `%LOCALAPPDATA%\Programs\IslandBar\`.
2. Run it. The **Settings** window opens and the taskbar splits into islands right away.
3. Adjust whatever you like. Changes apply live and are saved automatically.
4. Tick **Start with Windows** to keep it.

IslandBar then lives in the notification area:

- **Left-click** the icon to open Settings.
- **Right-click** it for the menu: Enabled, Show tray island, Extend when maximised, Auto-hide, Reload config, Open config folder, Create debug report, Start with Windows, Exit.
- Running `islandbar.exe` again while it's running just opens Settings. This is useful when the tray island, and with it the icon, is hidden.

### Command line

| Command | Effect |
|---|---|
| `islandbar.exe` | Start and open Settings |
| `islandbar.exe --background` | Start without opening Settings (used by *Start with Windows*) |
| `islandbar.exe --reset` | Remove the clip from every taskbar and exit |

## Settings reference

Settings are stored in `%APPDATA%\IslandBar\config.json`. You can edit the file by hand and choose *Reload config* from the tray menu. Sizes are logical pixels (at 100 % scaling) and are scaled per monitor.

| Setting | Key | Values (default) | Description |
|---|---|---|---|
| Enabled | `enabled` | `true` / `false` (`true`) | Master switch. Off means a normal taskbar. |
| Style | `mode` | `islands`, `bar` (`islands`) | Split islands sized to the buttons, or one rounded bar. |
| Tray island | `trayMode` | `show`, `hover`, `hide` (`show`) | Islands style only. `hover` shows it while the mouse is on the taskbar. |
| Widgets island | `showWidgets` | `true` / `false` (`true`) | Show button groups separate from the main app group. |
| Corner radius | `cornerRadius` | 0–30 (12) | |
| Top gap | `marginTop` | -30–20 (6) | Space above the islands. Negative pushes the top corners off the window. |
| Bottom gap | `marginBottom` | -30–20 (6) | Space below. At about 8 or more, the running-app indicators get cut off. |
| Side spacing | `islandPadding` | 0–40 (6) | Islands: extra space around the buttons. Bar: inset from the screen edges. |
| Extend when maximised | `fillOnMaximise` | `true` / `false` (`false`) | Per monitor. |
| Extend during Alt+Tab | `fillOnTaskSwitch` | `true` / `false` (`false`) | |
| Auto-hide | `autoHide` | `true` / `false` (`false`) | Use this instead of Windows' own auto-hide, which IslandBar doesn't support. |
| Background | `background` | `default`, `solid`, `gradient` (`default`) | See below. |
| Colour 1 / 2 | `color1`, `color2` | `#RRGGBB` | Colour 2 is the gradient end. |
| Direction | `gradientDirection` | `horizontal`, `vertical`, `diagonalDown`, `diagonalUp` | |
| Opacity | `opacity` | 0–100 % (85) | Fill opacity. |
| Border width | `borderWidth` | 0–4 (1) | |
| Border colour | `borderColor` | `#RRGGBB` (`#FFFFFF`) | |
| Border opacity | `borderOpacity` | 0–100 % (15) | |
| Verbose debug logging | `debugLogging` | `true` / `false` (`false`) | Asks for consent when enabled from Settings. |
| Poll interval | `pollIntervalMs` | 250–10000 (1000) | Safety-net re-check interval. File only. |

**Hotkey:** Win+F2 toggles the tray island between shown and hidden.

## Custom colours, gradients and smooth corners

On Windows 11 the taskbar background (Mica/acrylic) is drawn by explorer itself, and no outside program can recolour it without injecting code into explorer. IslandBar deliberately doesn't do that. The clip it applies also can't have antialiased edges: `SetWindowRgn` is pixel-exact, so rounded corners look slightly stepped.

For smooth corners and your own colours, IslandBar teams up with **[TranslucentTB](https://github.com/TranslucentTB/TranslucentTB)**:

1. Install TranslucentTB: from the Microsoft Store, or run `winget install --id CharlesMilette.TranslucentTB`.
2. In TranslucentTB's tray menu, set **Desktop → Clear**. Set the other states (visible window, maximised window, Start opened, …) to **Clear** too, or they'll paint over IslandBar's background in those situations.
3. In IslandBar's Settings → **Appearance**, pick *Solid colour* or *Gradient*.

IslandBar then draws the island backgrounds itself, antialiased and with per-pixel opacity, in a click-through window placed directly beneath each taskbar. The now-transparent taskbar shows them through, with its icons on top. The Settings status line warns you if a custom background is selected but TranslucentTB isn't running.

## How it works

```
 ┌──────────────── UI Automation (read-only) ───────────────┐
 │ Shell_TrayWnd → TaskbarFrame → buttons … SystemTray.*    │
 └───────────────┬──────────────────────────────────────────┘
                 │ button rectangles (physical px)
                 ▼
     cluster app-side buttons by gaps   ─┐
     union tray buttons (minus the      │  sanity checks:
     Show Desktop sliver)               │  inside the taskbar, no overlap,
                 │                      ─┘  otherwise leave it unclipped
                 ▼
     decide state: islands / full width (maximised, Alt+Tab) / hidden (auto-hide)
                 │
      ┌──────────┴───────────┐
      ▼                      ▼
 SetWindowRgn on the     optional backdrop window
 real taskbar            (UpdateLayeredWindow, antialiased,
 (rounded rectangles)    colour/gradient, just below the taskbar)
```

1. **Finding the buttons.** IslandBar asks UI Automation (the accessibility API screen readers use) for the taskbar's element tree and reads the on-screen rectangle of every button. That's why it doesn't care about alignment, app count or scaling: it measures, it doesn't estimate. Every identifier it relies on is kept in one file, [`src/core/match_rules.h`](src/core/match_rules.h), so a Windows update that renames something is a one-line fix.
2. **Building islands.** App-side buttons are grouped wherever there's a visible gap; the group containing Start is the app island. Tray buttons form the tray island. If the result looks wrong (empty, outside the taskbar, overlapping), IslandBar leaves the taskbar unclipped rather than guess.
3. **Clipping.** A rounded rectangle is built for each island, they're merged, and the result goes to `SetWindowRgn` on the taskbar window. Clicks in the gaps go through to the desktop. The region is only reapplied when something actually changed.
4. **Tracking changes.** WinEvent hooks on explorer's process report taskbar changes (new buttons, moves, animations). They're debounced to about 100 ms, with a 1-second polling safety net. Behaviour options add system-wide hooks for foreground, minimise/maximise and window movement. The `TaskbarCreated` broadcast re-attaches after explorer restarts; display, DPI and settings changes and resume from sleep trigger a re-scan.
5. **Safety.** On start, any clip left over from a previous run is cleared first. On exit, sign-out or crash, the full taskbar is restored. `--reset` does the same by hand.

IslandBar doesn't inject into explorer, modify system files, edit the registry (except the optional *Start with Windows* entry under `HKCU\…\Run`), or open network connections.

## Privacy, logging and debug reports

IslandBar contains **no network code**. Nothing is ever sent anywhere automatically.

- **Standard log**, `%APPDATA%\IslandBar\log.txt`: start and exit, taskbar re-attach events, the island rectangles when they change, and errors. It rotates at 1 MB (one old copy kept as `log.old.txt`).
- **Verbose debug logging** (off by default, asks for consent): also records every update decision, the timing of each UI Automation read, and the *window class* and state of the foreground window. It never records window titles or anything you type.
- **Debug report** (Settings → *Debug report…*, or the tray menu): after you agree to a summary of what's included, IslandBar writes `debug-report-<date>.txt` in the config folder with:
  - version, Windows build, DPI and monitor layout
  - your `config.json`
  - the taskbar's UI Automation structure (element types, class names, IDs, rectangles)
  - the last 400 log lines

  **Removed before saving:** element names (window titles, app names, tray tooltips such as Wi-Fi network names), the app IDs of taskbar buttons, your user name, computer name and profile path. The file is revealed in Explorer so you can read it first, and IslandBar offers to open a new GitHub issue where you can attach it yourself.

## Troubleshooting

| Problem | Fix |
|---|---|
| Taskbar is stuck clipped after IslandBar was killed | Run `islandbar.exe --reset` |
| Taskbar is full width and Status says "unclipped" | Detection failed (often after a Windows update). Please create a debug report and open an issue. |
| Custom background doesn't show | TranslucentTB must be running with the taskbar set to Clear |
| Tray icon is gone | The tray island is hidden; run `islandbar.exe` again to open Settings |
| Running-app indicator dots are cut off | Lower the bottom gap (about 6 or less) |

`uia-dump.exe` prints what IslandBar sees:

```powershell
uia-dump.exe --islands                      # computed island rectangles per taskbar
uia-dump.exe --out dump.txt                 # full UI Automation tree
uia-dump.exe --no-names --out dump.txt      # same, with names removed for sharing
```

## Known limitations

- The clip itself isn't antialiased (a `SetWindowRgn` limit). Use a [custom background](#custom-colours-gradients-and-smooth-corners) for smooth corners.
- Windows' own *Automatically hide the taskbar* isn't supported; use IslandBar's Auto-hide instead.
- The taskbar can't move to the top or sides, and buttons can't be repositioned, because IslandBar only shapes what Windows draws. With left alignment, the app island touches the screen edge.
- Tooltips, thumbnails, Start, search and flyouts are separate windows and keep their normal look.
- Tools that restyle the taskbar in other ways may conflict. TranslucentTB is supported as described above.
- Windows 11 only (tested on 25H2, build 26200).

## Building from source

Requirements: Windows 11, Visual Studio 2022 or Build Tools (Desktop C++ workload and Windows 11 SDK), CMake 3.21 or newer.

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100"
winget install --id Kitware.CMake

cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# → build\Release\islandbar.exe, build\Release\uia-dump.exe
```

C++20, Win32 and UI Automation only. The single third-party dependency, [nlohmann/json](https://github.com/nlohmann/json) (MIT), is vendored in `third_party/`.

**Releasing:** push a tag such as `v1.2.0`. The workflow builds, hashes, attests and publishes the release.

## Project layout

| Path | Purpose |
|---|---|
| `src/core/match_rules.h` | every UIA identifier and window class IslandBar depends on |
| `src/core/bounds.*` | UI Automation → island rectangles, clustering, sanity checks |
| `src/core/region.*` | rounded regions, `SetWindowRgn`, taskbar discovery, reset |
| `src/core/config.*` | `config.json` load/save |
| `src/core/log.*` | rolling log, opt-in verbose lines |
| `src/core/tree_dump.*` | UIA tree dump (used by `uia-dump` and debug reports) |
| `src/islandbar/engine.*` | hooks, state decisions (islands / full / hidden), applying regions |
| `src/islandbar/backdrop.*` | antialiased custom background window |
| `src/islandbar/settings_window.*` | the Settings window |
| `src/islandbar/debug_report.*` | consent-gated, redacted debug report |
| `src/islandbar/main.cpp` | tray icon, menu, hotkey, timers, message loop, safety nets |
| `src/uia-dump/` | diagnostic console tool |
| `.github/workflows/release.yml` | CI build, checksums, provenance, releases |

## License

© 2026 Zyneq. IslandBar/FloatBar is licensed under the **[Creative Commons Attribution 4.0 International License (CC BY 4.0)](LICENSE)**.

You may use, share, modify and redistribute it, including commercially, as long as you **give appropriate credit**: name the author and link to this repository and the license, and indicate whether you made changes. For example:

> Based on [FloatBar / IslandBar](https://github.com/Zyneq/FloatBar) by Zyneq, licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

Third-party: [nlohmann/json](https://github.com/nlohmann/json) © Niels Lohmann, MIT License (see `third_party/nlohmann/LICENSE.MIT`).

Inspired by [RoundedTB](https://github.com/RoundedTB/RoundedTB); no code is shared with it.
