#pragma once

// Every identifier FloatBar relies on lives in this file.
// If a Windows update changes the taskbar, run `floatbar.exe --dump dump.txt`,
// compare, and update the values below. Nothing else should need to change.
//
// Observed on Windows 11 25H2 (build 26200.9457):
//
//   Pane  cls="Taskbar.TaskbarFrameAutomationPeer" aid="TaskbarFrame"   <- app side root
//     Group aid="TaskbarFrameRepeater"
//       Button aid="StartButton"
//       Button cls="Taskbar.TaskListButtonAutomationPeer" (one per pinned/running app)
//   Button cls="SystemTray.NormalButton"      aid="SystemTrayIcon"  (chevron, input indicator, tray icons)
//   Button cls="SystemTray.AccentButton"      aid="SystemTrayIcon"  (network)
//   Button cls="SystemTray.OmniButtonRight"   aid="SystemTrayIcon"  (volume)
//   Button cls="SystemTray.OmniButton"        aid="SystemTrayIcon"  (clock; also on secondary taskbars)
//   Button cls="SystemTray.ShowDesktopButton" aid="SystemTrayIcon"  (12 px sliver at the far edge)
//
// The legacy Win32 children (ReBarWindow32, MSTaskSwWClass, WorkerW, ...) report
// stale rectangles and are never used.

#include <windows.h>

#include <string>
#include <string_view>

namespace fb::rules {

// ---- App side ------------------------------------------------------------
// Every visible Button under this element belongs to the app side. Buttons are
// grouped into clusters by horizontal gaps: the cluster holding Start is the app
// island, any other cluster (e.g. Widgets at the far left when centered) is an
// extra island. This picks up Search, Task View, Widgets and overflow without
// knowing their ids.
inline constexpr wchar_t kAppSideRootAutomationId[] = L"TaskbarFrame";
inline constexpr wchar_t kStartButtonAutomationId[] = L"StartButton";
inline constexpr int kClusterGapLogicalPx = 16;

// ---- Tray ----------------------------------------------------------------
inline constexpr wchar_t kTrayButtonAutomationId[] = L"SystemTrayIcon";
inline constexpr std::wstring_view kTrayClassPrefix = L"SystemTray.";
inline constexpr wchar_t kShowDesktopClass[] = L"SystemTray.ShowDesktopButton";
inline constexpr std::wstring_view kTrayExcludedClasses[] = {
    kShowDesktopClass,
};

inline bool IsTrayIslandMember(const std::wstring& cls) {
    if (!std::wstring_view(cls).starts_with(kTrayClassPrefix)) return false;
    for (std::wstring_view excluded : kTrayExcludedClasses) {
        if (cls == excluded) return false;
    }
    return true;
}

// ---- Foreground window classification (auto-hide, hover, fill) ----------------
// Windows 11 Alt+Tab and Task View.
inline constexpr std::wstring_view kTaskSwitcherClasses[] = {
    L"XamlExplorerHostIslandWindow",
};

// Windows that cover a whole monitor without being a "fullscreen app".
inline constexpr std::wstring_view kDesktopClasses[] = {
    L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
};

// explorer.exe windows that are *not* shell UI (File Explorer, the desktop).
inline constexpr std::wstring_view kExplorerNonShellClasses[] = {
    L"CabinetWClass", L"Progman", L"WorkerW",
};

// Processes hosting Start, Search, notification center, calendar and quick settings.
inline constexpr std::wstring_view kShellProcesses[] = {
    L"StartMenuExperienceHost.exe", L"SearchHost.exe", L"ShellExperienceHost.exe", L"ShellHost.exe",
};

template <size_t N>
bool InList(const std::wstring& value, const std::wstring_view (&list)[N]) {
    for (std::wstring_view item : list) {
        if (_wcsicmp(value.c_str(), std::wstring(item).c_str()) == 0) return true;
    }
    return false;
}

}  // namespace fb::rules
