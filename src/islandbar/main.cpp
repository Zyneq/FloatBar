// IslandBar: splits the Windows 11 taskbar into floating rounded islands.
//
//   islandbar.exe               run (opens the settings window)
//   islandbar.exe --background  run without opening settings (used by "Start with Windows")
//   islandbar.exe --reset       remove any clip from all taskbars and exit

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <string>

#include "common/uia_util.h"
#include "core/config.h"
#include "core/log.h"
#include "core/region.h"
#include "islandbar/app_icon.h"
#include "islandbar/engine.h"
#include "islandbar/settings_window.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kMainClass[] = L"IslandBarMain";
constexpr wchar_t kMutexName[] = L"Local\\IslandBar.SingleInstance";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"IslandBar";

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_SHOW_SETTINGS = WM_APP + 2;

enum TimerId : UINT_PTR { kTimerDebounce = 1, kTimerPoll, kTimerSave, kTimerSettle, kTimerHover };
constexpr UINT kDebounceMs = 100;      // taskbar content changed
constexpr UINT kWindowEventMs = 30;    // other windows changed (maximise, foreground)
constexpr UINT kRetryMs = 150;
constexpr UINT kHoverPollMs = 50;

constexpr int kHotkeyToggleTray = 1;   // Win+F2, as in RoundedTB

enum MenuId : UINT {
    kMenuSettings = 1,
    kMenuEnabled,
    kMenuShowTray,
    kMenuFillMaximise,
    kMenuAutoHide,
    kMenuReload,
    kMenuOpenFolder,
    kMenuAutostart,
    kMenuExit,
};

HINSTANCE g_instance = nullptr;
HWND g_mainWnd = nullptr;
UINT g_taskbarCreatedMsg = 0;
HICON g_smallIcon = nullptr;
HICON g_largeIcon = nullptr;
bool g_debouncePending = false;
bool g_pendingBounds = false;  // a pending update must re-read button bounds
ib::Config g_config;
ib::Engine* g_engine = nullptr;  // lives in wWinMain so it is released before CoUninitialize

// ---------------------------------------------------------------- autostart

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

bool IsAutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool enable) {
    if (enable) {
        const std::wstring command = L"\"" + ExePath() + L"\" --background";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(),
                        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }
    ib::log::Write(L"start with Windows: %s", enable ? L"on" : L"off");
}

// ---------------------------------------------------------------- updates

// Coalesces bursts of events into one update. `readBounds` asks for a fresh UIA
// read; without it only fill/auto-hide/hover state is re-evaluated (cheap).
void ScheduleUpdate(bool readBounds, UINT delayMs = kDebounceMs) {
    g_pendingBounds |= readBounds;
    if (g_debouncePending) return;
    g_debouncePending = true;
    SetTimer(g_mainWnd, kTimerDebounce, delayMs, nullptr);
}

void RunUpdate(bool force, bool readBounds = true) {
    if (g_engine->TaskbarsChanged()) {
        ib::log::Write(L"taskbar set changed, reattaching");
        g_engine->Attach();
        force = readBounds = true;
    }
    if (g_engine->Update(force, readBounds)) ScheduleUpdate(true, kRetryMs);
    ib::settings::RefreshStatus();
}

void SetConfig(const ib::Config& config, bool save) {
    const bool wasEnabled = g_config.enabled;
    g_config = config;
    g_engine->SetConfig(g_config);
    if (wasEnabled && !g_config.enabled) g_engine->ClearAll();
    SetTimer(g_mainWnd, kTimerPoll, g_config.pollIntervalMs, nullptr);
    if (g_engine->NeedsHoverPolling()) SetTimer(g_mainWnd, kTimerHover, kHoverPollMs, nullptr);
    else KillTimer(g_mainWnd, kTimerHover);
    RunUpdate(true);
    if (save) SetTimer(g_mainWnd, kTimerSave, 400, nullptr);
}

void ToggleTray() {
    ib::Config c = g_config;
    c.trayMode = c.trayMode == ib::TrayMode::Hide ? ib::TrayMode::Show : ib::TrayMode::Hide;
    SetConfig(c, true);
    ib::settings::RefreshControls();
}

void ReloadConfig() {
    ib::Config config;
    std::wstring error;
    if (!ib::LoadConfig(config, error)) {
        ib::log::Write(L"%s", error.c_str());
        MessageBoxW(nullptr, (error + L"\n\nUsing default settings.").c_str(), L"IslandBar", MB_ICONWARNING);
    }
    SetConfig(config, false);
    ib::settings::RefreshControls();
}

void OpenConfigFolder() {
    ShellExecuteW(nullptr, L"open", ib::ConfigDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- UI

void ShowSettings() {
    ib::settings::Host host;
    host.getConfig = [] { return g_config; };
    host.setConfig = [](const ib::Config& c) { SetConfig(c, true); };
    host.getAutostart = IsAutostartEnabled;
    host.setAutostart = SetAutostart;
    host.openConfigFolder = OpenConfigFolder;
    host.getStatus = [] { return g_engine->Status(); };
    host.exitApp = [] { DestroyWindow(g_mainWnd); };
    ib::settings::Show(g_instance, host, g_smallIcon, g_largeIcon);
}

void AddTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_mainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_smallIcon;
    wcscpy_s(nid.szTip, L"IslandBar");
    Shell_NotifyIconW(NIM_DELETE, &nid);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_mainWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowTrayMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    auto check = [](bool on) { return on ? MF_CHECKED : MF_UNCHECKED; };
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings…");
    SetMenuDefaultItem(menu, kMenuSettings, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | check(g_config.enabled), kMenuEnabled, L"Enabled");
    AppendMenuW(menu, MF_STRING | check(g_config.trayMode != ib::TrayMode::Hide), kMenuShowTray, L"Show tray island\tWin+F2");
    AppendMenuW(menu, MF_STRING | check(g_config.fillOnMaximise), kMenuFillMaximise, L"Extend when maximised");
    AppendMenuW(menu, MF_STRING | check(g_config.autoHide), kMenuAutoHide, L"Auto-hide");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuReload, L"Reload config");
    AppendMenuW(menu, MF_STRING, kMenuOpenFolder, L"Open config folder");
    AppendMenuW(menu, MF_STRING | check(IsAutostartEnabled()), kMenuAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    SetForegroundWindow(g_mainWnd);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, g_mainWnd, nullptr);
    PostMessageW(g_mainWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void OnMenu(UINT id) {
    ib::Config c = g_config;
    switch (id) {
        case kMenuSettings: ShowSettings(); return;
        case kMenuEnabled: c.enabled = !c.enabled; break;
        case kMenuShowTray: ToggleTray(); return;
        case kMenuFillMaximise: c.fillOnMaximise = !c.fillOnMaximise; break;
        case kMenuAutoHide: c.autoHide = !c.autoHide; break;
        case kMenuReload: ReloadConfig(); return;
        case kMenuOpenFolder: OpenConfigFolder(); return;
        case kMenuAutostart: SetAutostart(!IsAutostartEnabled()); ib::settings::RefreshControls(); return;
        case kMenuExit: DestroyWindow(g_mainWnd); return;
        default: return;
    }
    SetConfig(c, true);
    ib::settings::RefreshControls();
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {
        // Explorer restarted: the tray icon is gone and the taskbar XAML needs a moment to build.
        ib::log::Write(L"TaskbarCreated received");
        AddTrayIcon();
        SetTimer(hwnd, kTimerSettle, 1000, nullptr);
        return 0;
    }

    switch (msg) {
        case WM_TIMER:
            switch (wParam) {
                case kTimerDebounce: {
                    KillTimer(hwnd, kTimerDebounce);
                    const bool readBounds = g_pendingBounds;
                    g_debouncePending = g_pendingBounds = false;
                    RunUpdate(false, readBounds);
                    break;
                }
                case kTimerHover:
                    if (g_engine->PollHover()) RunUpdate(false, false);
                    break;
                case kTimerPoll:
                    RunUpdate(false);
                    break;
                case kTimerSave:
                    KillTimer(hwnd, kTimerSave);
                    ib::SaveConfig(g_config);
                    break;
                case kTimerSettle:
                    KillTimer(hwnd, kTimerSettle);
                    g_engine->Attach();
                    RunUpdate(true);
                    break;
            }
            return 0;

        case WM_APP_TRAY:
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    ShowSettings();
                    break;
                case WM_CONTEXTMENU:
                    ShowTrayMenu(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
                    break;
            }
            return 0;

        case WM_APP_SHOW_SETTINGS:
            ShowSettings();
            return 0;

        case WM_COMMAND:
            OnMenu(LOWORD(wParam));
            return 0;

        case WM_HOTKEY:
            if (wParam == kHotkeyToggleTray) ToggleTray();
            return 0;

        case WM_SETTINGCHANGE:
            ScheduleUpdate(true);
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            ScheduleUpdate(true);
            SetTimer(hwnd, kTimerSettle, 700, nullptr);
            return 0;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC) SetTimer(hwnd, kTimerSettle, 1500, nullptr);
            return TRUE;

        case WM_ENDSESSION:
            if (wParam) {
                g_engine->ClearAll();
                ib::log::Write(L"session ending, regions cleared");
            }
            return 0;

        case WM_DESTROY:
            UnregisterHotKey(hwnd, kHotkeyToggleTray);
            if (HWND settings = ib::settings::Window()) DestroyWindow(settings);
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------- safety nets

LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    ib::ClearAllTaskbars();
    ib::log::Write(L"unhandled exception, regions cleared");
    return EXCEPTION_CONTINUE_SEARCH;
}

bool HasArg(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) return true;
    }
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool reset = HasArg(argc, argv, L"--reset");
    const bool background = HasArg(argc, argv, L"--background");
    LocalFree(argv);

    if (reset) {
        ib::ClearAllTaskbars();
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: bring up its settings window instead.
        if (HWND other = FindWindowW(kMainClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_APP_SHOW_SETTINGS, 0, 0);
        }
        return 0;
    }

    ib::ComInit com(COINIT_MULTITHREADED);
    if (FAILED(com.hr())) return 1;

    const std::wstring dir = ib::ConfigDir();
    ib::log::Init(dir);
    ib::log::Write(L"---- IslandBar starting (%s)", ExePath().c_str());

    SetUnhandledExceptionFilter(CrashFilter);

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    const UINT dpi = GetDpiForSystem();
    g_smallIcon = ib::CreateIslandIcon(GetSystemMetricsForDpi(SM_CXSMICON, dpi));
    g_largeIcon = ib::CreateIslandIcon(GetSystemMetricsForDpi(SM_CXICON, dpi));

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kMainClass;
    wc.hIcon = g_largeIcon;
    wc.hIconSm = g_smallIcon;
    RegisterClassExW(&wc);

    // Hidden top-level window (not HWND_MESSAGE) so it receives broadcasts such as
    // TaskbarCreated, WM_SETTINGCHANGE and WM_DISPLAYCHANGE.
    g_mainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"IslandBar", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_mainWnd) return 1;
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    ib::Engine engine;
    g_engine = &engine;
    ib::Engine::Callbacks callbacks;
    callbacks.taskbarChanged = [] { ScheduleUpdate(true); };
    callbacks.windowsChanged = [] { ScheduleUpdate(false, kWindowEventMs); };
    if (FAILED(g_engine->Init(std::move(callbacks)))) {
        MessageBoxW(nullptr, L"Could not initialize UI Automation.", L"IslandBar", MB_ICONERROR);
        return 1;
    }
    if (!RegisterHotKey(g_mainWnd, kHotkeyToggleTray, MOD_WIN | MOD_NOREPEAT, VK_F2)) {
        ib::log::Write(L"Win+F2 hotkey unavailable (%lu)", GetLastError());
    }

    // A crashed or killed previous run may have left a clip behind.
    ib::ClearAllTaskbars();

    ib::Config config;
    std::wstring error;
    if (!ib::LoadConfig(config, error)) ib::log::Write(L"%s (using defaults)", error.c_str());
    g_engine->Attach();
    SetConfig(config, false);
    AddTrayIcon();

    if (!background) ShowSettings();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND settings = ib::settings::Window();
        if (settings && IsDialogMessageW(settings, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_engine->Detach();
    g_engine->ClearAll();
    g_engine = nullptr;
    ib::log::Write(L"---- IslandBar exiting, regions cleared");
    if (mutex) CloseHandle(mutex);
    return 0;
}
