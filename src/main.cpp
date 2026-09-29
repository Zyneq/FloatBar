// FloatBar: splits the Windows 11 taskbar into floating rounded islands.
//
//   floatbar.exe                run (opens the settings window)
//   floatbar.exe --background   run without opening settings (used by "Start with Windows")
//   floatbar.exe --reset        remove any clip from all taskbars and exit
//   floatbar.exe --dump <file>  write the taskbar's UI Automation tree to <file> and exit
//   floatbar.exe --config-dir <dir>  use <dir> instead of %APPDATA%\FloatBar (automated tests)

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <fstream>
#include <string>

#include "app.h"
#include "config.h"
#include "debug_report.h"
#include "engine.h"
#include "log.h"
#include "region.h"
#include "settings_window.h"
#include "tree_dump.h"
#include "uia_util.h"
#include "version.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kMainClass[] = L"FloatBarMain";
constexpr wchar_t kMutexName[] = L"Local\\FloatBar.SingleInstance";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"FloatBar";

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_SHOW_SETTINGS = WM_APP + 2;
constexpr UINT WM_APP_BOUNDS = WM_APP + 3;  // a BoundsWorker::Reply* in lParam

enum TimerId : UINT_PTR { kTimerDebounce = 1, kTimerPoll, kTimerSave, kTimerSettle, kTimerHover, kTimerRetry, kTimerRecheck, kTimerRefresh };
constexpr UINT kDebounceMs = 16;       // taskbar content changed (reads run off-thread, so react fast)
constexpr UINT kWindowEventMs = 30;    // other windows changed (maximise, foreground)
constexpr UINT kRetryMs = 150;
constexpr UINT kRecheckMs = 250;     // one more read after buttons stopped moving
constexpr UINT kHoverPollMs = 50;
constexpr UINT kPollMs = 1000;         // safety net in case an event was missed

constexpr int kHotkeyToggleTray = 1;   // Win+F2, as in RoundedTB

enum MenuId : UINT { kMenuSettings = 1, kMenuEnabled, kMenuDebugReport, kMenuExit };

HINSTANCE g_instance = nullptr;
HWND g_mainWnd = nullptr;
UINT g_taskbarCreatedMsg = 0;
HICON g_smallIcon = nullptr;
HICON g_largeIcon = nullptr;
bool g_debouncePending = false;
bool g_pendingBounds = false;  // a pending update must re-read button bounds
bool g_refreshTimer = false;   // kTimerRefresh is running
bool g_hotkeyRegistered = false;
fb::Config g_config;
fb::Engine* g_engine = nullptr;  // lives in wWinMain so it is released before CoUninitialize

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

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
        fb::log::Write(L"taskbar set changed, reattaching");
        g_engine->Attach();
        force = readBounds = true;
        fb::settings::RefreshControls();  // the monitor list may have changed
    }
    g_engine->Update(force, readBounds);  // fresh bounds arrive later as WM_APP_BOUNDS
    fb::settings::RefreshStatus();
}

void OnBounds(fb::BoundsWorker::Reply* reply) {
    const fb::Engine::UpdateResult result = g_engine->OnBounds(reply);
    if (result.retry) SetTimer(g_mainWnd, kTimerRetry, kRetryMs, nullptr);
    if (result.recheck) SetTimer(g_mainWnd, kTimerRecheck, kRecheckMs, nullptr);
    fb::settings::RefreshStatus();
}

void ApplyConfig(const fb::Config& config, bool save) {
    const bool wasEnabled = g_config.enabled;
    if (config.debugLogging != g_config.debugLogging) fb::log::Write(L"verbose debug logging %s", config.debugLogging ? L"on" : L"off");
    g_config = config;
    fb::log::SetVerbose(g_config.debugLogging);
    g_engine->SetConfig(g_config);
    if (wasEnabled && !g_config.enabled) g_engine->ClearAll();
    if (g_engine->NeedsHoverPolling()) SetTimer(g_mainWnd, kTimerHover, kHoverPollMs, nullptr);
    else KillTimer(g_mainWnd, kTimerHover);
    RunUpdate(true);
    if (save) SetTimer(g_mainWnd, kTimerSave, 400, nullptr);
}

void ToggleTray() {
    fb::Config c = g_config;
    c.trayMode = c.trayMode == fb::TrayMode::Hide ? fb::TrayMode::Show : fb::TrayMode::Hide;
    ApplyConfig(c, true);
    fb::settings::RefreshControls();
}

void ShowSettings() { fb::settings::Show(g_instance, g_smallIcon, g_largeIcon); }

void AddTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_mainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_smallIcon;
    wcscpy_s(nid.szTip, L"FloatBar");
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

// Everything else lives in the Settings window.
void ShowTrayMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings…");
    SetMenuDefaultItem(menu, kMenuSettings, FALSE);
    AppendMenuW(menu, MF_STRING | (g_config.enabled ? MF_CHECKED : MF_UNCHECKED), kMenuEnabled, L"Enabled");
    AppendMenuW(menu, MF_STRING, kMenuDebugReport, L"Create debug report…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    SetForegroundWindow(g_mainWnd);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, g_mainWnd, nullptr);
    PostMessageW(g_mainWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void OnMenu(UINT id) {
    switch (id) {
        case kMenuSettings: ShowSettings(); break;
        case kMenuEnabled: {
            fb::Config c = g_config;
            c.enabled = !c.enabled;
            ApplyConfig(c, true);
            fb::settings::RefreshControls();
            break;
        }
        case kMenuDebugReport: fb::app::CreateDebugReport(); break;
        case kMenuExit: fb::app::Exit(); break;
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {
        // Explorer restarted: the tray icon is gone and the taskbar XAML needs a moment to build.
        fb::log::Write(L"TaskbarCreated received");
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
                case kTimerRecheck:
                case kTimerRetry:
                    KillTimer(hwnd, wParam);
                    g_engine->MarkAllDirty();
                    g_engine->Update(false, true);
                    break;
                case kTimerRefresh:
                    g_engine->RefreshRegions();
                    if (!g_engine->NeedsRegionRefresh()) {
                        KillTimer(hwnd, kTimerRefresh);
                        g_refreshTimer = false;
                    }
                    break;
                case kTimerPoll:
                    g_engine->MarkAllDirty();  // safety net: read everything once a second
                    RunUpdate(false);
                    break;
                case kTimerSave:
                    KillTimer(hwnd, kTimerSave);
                    fb::SaveConfig(g_config);
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

        case WM_APP_BOUNDS:
            OnBounds(reinterpret_cast<fb::BoundsWorker::Reply*>(lParam));
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
            g_engine->MarkAllDirty();
            ScheduleUpdate(true);
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            g_engine->MarkAllDirty();
            ScheduleUpdate(true);
            SetTimer(hwnd, kTimerSettle, 700, nullptr);
            return 0;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC) SetTimer(hwnd, kTimerSettle, 1500, nullptr);
            return TRUE;

        case WM_ENDSESSION:
            if (wParam) {
                g_engine->ClearAll();
                fb::log::Write(L"session ending, regions cleared");
            }
            return 0;

        case WM_DESTROY:
            UnregisterHotKey(hwnd, kHotkeyToggleTray);
            if (HWND settings = fb::settings::Window()) DestroyWindow(settings);
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    fb::ClearAllTaskbars();
    fb::log::Write(L"unhandled exception, regions cleared");
    return EXCEPTION_CONTINUE_SEARCH;
}

void Dispatch(MSG& msg) {
    HWND settings = fb::settings::Window();
    if (settings && IsDialogMessageW(settings, &msg)) return;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
}

// Idle: block in GetMessage. While islands move: drain messages, draw the frame
// for "now", then wait for the next display refresh (DwmFlush), so motion is
// updated exactly once per refresh at any refresh rate.
void RunMessageLoop() {
    MSG msg;
    for (;;) {
        if (!g_refreshTimer && g_engine->NeedsRegionRefresh()) {
            SetTimer(g_mainWnd, kTimerRefresh, fb::Engine::kRegionRefreshMs, nullptr);
            g_refreshTimer = true;
        }
        if (!g_engine->Animating()) {
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) return;
            Dispatch(msg);
            continue;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            Dispatch(msg);
        }
        g_engine->Animate();
        if (FAILED(DwmFlush())) Sleep(1);
    }
}

// Writes the full (unredacted) tree for local diagnosis.
int Dump(const wchar_t* path) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::string text = "\xEF\xBB\xBF" + fb::Utf8(fb::DumpTaskbarTrees(true));
    std::ofstream(path, std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
    CoUninitialize();
    return 0;
}

}  // namespace

// ---------------------------------------------------------------- app.h

namespace fb::app {

Config GetConfig() { return g_config; }
void SetConfig(const Config& config) { ApplyConfig(config, true); }

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
    log::Write(L"start with Windows: %s", enable ? L"on" : L"off");
}

void OpenConfigFolder() { ShellExecuteW(nullptr, L"open", ConfigDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void CreateDebugReport() { fb::CreateDebugReport(settings::Window(), Status()); }

std::wstring Status() {
    std::wstring status = g_engine->Status();
    if (!g_hotkeyRegistered) status += L"\r\nWin+F2 is already used by another program, so the hotkey is unavailable.";
    return status;
}

std::vector<MonitorEntry> Monitors() { return g_engine->Monitors(); }
void Exit() { DestroyWindow(g_mainWnd); }

}  // namespace fb::app

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool background = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--reset") == 0) {
            fb::ClearAllTaskbars();
            return 0;
        }
        if (_wcsicmp(argv[i], L"--dump") == 0 && i + 1 < argc) return Dump(argv[i + 1]);
        if (_wcsicmp(argv[i], L"--background") == 0) background = true;
        if (_wcsicmp(argv[i], L"--config-dir") == 0 && i + 1 < argc) fb::SetConfigDir(argv[++i]);
    }
    LocalFree(argv);

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

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;

    fb::log::Init(fb::ConfigDir());
    fb::log::Write(L"---- FloatBar %s starting (%s)", fb::kVersion, ExePath().c_str());

    SetUnhandledExceptionFilter(CrashFilter);

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    // The exe's icon (res/version.rc.in), at the sizes the tray and title bars use.
    LoadIconMetric(instance, MAKEINTRESOURCEW(1), LIM_SMALL, &g_smallIcon);
    LoadIconMetric(instance, MAKEINTRESOURCEW(1), LIM_LARGE, &g_largeIcon);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    // Hidden top-level window (not HWND_MESSAGE) so it receives broadcasts such as
    // TaskbarCreated, WM_SETTINGCHANGE and WM_DISPLAYCHANGE.
    g_mainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"FloatBar", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_mainWnd) return 1;
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    int exitCode = 0;
    {
        fb::Engine engine;
        g_engine = &engine;
        fb::Engine::Callbacks callbacks;
        callbacks.taskbarChanged = [] { ScheduleUpdate(true); };
        callbacks.windowsChanged = [] { ScheduleUpdate(false, kWindowEventMs); };
        if (FAILED(engine.Init(std::move(callbacks), g_mainWnd, WM_APP_BOUNDS))) {
            MessageBoxW(nullptr, L"Could not initialize UI Automation.", L"FloatBar", MB_ICONERROR);
            exitCode = 1;
        } else {
            g_hotkeyRegistered = RegisterHotKey(g_mainWnd, kHotkeyToggleTray, MOD_WIN | MOD_NOREPEAT, VK_F2) != FALSE;
            if (!g_hotkeyRegistered) fb::log::Write(L"Win+F2 hotkey unavailable (%lu)", GetLastError());

            // A crashed or killed previous run may have left a clip behind.
            fb::ClearAllTaskbars();
            engine.Attach();
            ApplyConfig(fb::LoadConfig(), false);
            SetTimer(g_mainWnd, kTimerPoll, kPollMs, nullptr);
            AddTrayIcon();
            if (!background) ShowSettings();

            RunMessageLoop();
            engine.Detach();
            engine.ClearAll();
            fb::log::Write(L"---- FloatBar exiting, regions cleared");
        }
        g_engine = nullptr;
    }
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return exitCode;
}
