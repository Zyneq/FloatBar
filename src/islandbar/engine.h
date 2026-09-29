#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/bounds.h"
#include "core/config.h"
#include "core/region.h"
#include "islandbar/backdrop.h"

namespace ib {

// Stable-enough monitor identity for per-monitor settings, e.g. \\.\DISPLAY2.
std::wstring MonitorKey(HMONITOR monitor);

struct MonitorEntry {
    std::wstring key;    // MonitorKey()
    std::wstring label;  // "Main taskbar (1920×1080)"
};

// Owns the taskbar list, the WinEvent hooks and the applied regions.
// Everything runs on the main (UI) thread.
class Engine {
public:
    struct Callbacks {
        std::function<void()> taskbarChanged;  // taskbar content moved: re-read bounds
        std::function<void()> windowsChanged;  // other windows changed: re-evaluate fill/auto-hide only
    };

    ~Engine();

    HRESULT Init(Callbacks callbacks);
    // Also installs or removes the system-wide hooks the behaviour options need.
    void SetConfig(const Config& config);

    // (Re)discovers taskbars and hooks the explorer process that owns them.
    void Attach();
    void Detach();
    bool TaskbarsChanged() const;

    // Reapplies regions where they changed (or always, if `force`). With
    // `readBounds` false the last known button bounds are reused, which is cheap.
    // Returns true when a transient failure wants a quick retry.
    bool Update(bool force, bool readBounds);

    // Mouse tracking for auto-hide and the hover tray. Returns true if any
    // taskbar's hover state changed.
    bool NeedsHoverPolling() const;
    bool PollHover();

    void ClearAll();
    std::wstring Status() const;
    std::vector<MonitorEntry> Monitors() const;

private:
    using Key = std::vector<LONG>;

    struct Taskbar {
        HWND hwnd = nullptr;
        bool primary = false;
        std::optional<Islands> islands;  // last good bounds
        std::optional<Key> applied;
        int failures = 0;
        std::wstring lastError;
        std::wstring status;
        bool hovered = false;
        DWORD lastInside = 0;
        std::unique_ptr<Backdrop> backdrop;  // only with a custom background
    };

    // Foreground/maximised state shared by all taskbars during one update.
    struct Context {
        bool shellUi = false;     // Start, search, a flyout or the taskbar itself is focused
        bool taskSwitch = false;  // Alt+Tab or Task View is open
        std::vector<HMONITOR> maximised;
        std::vector<HMONITOR> fullscreen;
        std::wstring foregroundClass;  // for verbose logging only
    };

    Context BuildContext() const;
    bool UpdateOne(Taskbar& tb, bool force, bool readBounds, const Context& ctx);
    void SyncBackdrop(Taskbar& tb, LONG state, const RECT& wr, const std::vector<RECT>& spans, const SpanStyle& style, UINT dpi);
    bool IsTaskbar(HWND hwnd) const;
    bool NeedsGlobalHooks() const;
    void InstallGlobalHooks();
    void RemoveGlobalHooks();

    static void CALLBACK ExplorerEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static void CALLBACK GlobalEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static Engine* s_instance;

    BoundsReader reader_;
    Config config_;
    Callbacks callbacks_;
    std::vector<Taskbar> taskbars_;
    std::vector<HWINEVENTHOOK> explorerHooks_;
    std::vector<HWINEVENTHOOK> globalHooks_;
    DWORD explorerPid_ = 0;
    bool switching_ = false;
};

}  // namespace ib
