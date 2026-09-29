#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "backdrop.h"
#include "bounds.h"
#include "bounds_worker.h"
#include "config.h"
#include "region.h"

namespace fb {

// Stable-enough monitor identity for per-monitor settings, e.g. \\.\DISPLAY2.
std::wstring MonitorKey(HMONITOR monitor);

struct MonitorEntry {
    std::wstring key;    // MonitorKey()
    std::wstring label;  // "Main taskbar (1920×1080)"
};

// One rounded island in screen coordinates (only left/right matter; the height
// comes from the style). `id` says which island it is, so animation can match an
// island across frames and grow or shrink islands that appear or disappear.
struct Span {
    RECT rect{};
    int id = 0;
};

// Owns the taskbar list, the WinEvent hooks and the applied regions. Runs on
// the UI thread; UI Automation reads happen on a BoundsWorker thread.
class Engine {
public:
    struct Callbacks {
        std::function<void()> taskbarChanged;  // taskbar content moved: re-read bounds
        std::function<void()> windowsChanged;  // other windows changed: re-evaluate fill/auto-hide only
    };

    struct UpdateResult {
        bool retry = false;   // a read failed transiently: read again soon
        bool reread = false;  // a taskbar is mid-relayout: read it again quickly until it settles
    };

    ~Engine();

    // Bounds results arrive as `boundsMessage` on `notifyWnd`; pass them to OnBounds().
    HRESULT Init(Callbacks callbacks, HWND notifyWnd, UINT boundsMessage);
    // Also installs or removes the system-wide hooks the behaviour options need.
    void SetConfig(const Config& config);

    // (Re)discovers taskbars and hooks the explorer process that owns them.
    void Attach();
    void Detach();
    bool TaskbarsChanged() const;

    // Re-evaluates every taskbar with the last known bounds (cheap) and, with
    // `readBounds`, also asks the worker for fresh bounds. `force` redraws even
    // when nothing changed and skips animation.
    void Update(bool force, bool readBounds);
    // Asks for fresh bounds of the taskbars that are still settling.
    void RequestSettlingReads();
    // A read finished (from the worker's message); takes ownership of `reply`.
    UpdateResult OnBounds(BoundsWorker::Reply* reply);

    // Mouse tracking for auto-hide and the hover tray. Returns true if any
    // taskbar's hover state changed.
    bool NeedsHoverPolling() const;
    bool PollHover();

    // Time-based island animation. While Animating(), call Animate() once per
    // display frame (after DwmFlush); it positions everything for "now".
    bool Animating() const;
    void Animate();

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
        bool forcePending = false;       // a forced update is waiting for its read
        std::wstring lastError;
        std::wstring status;
        bool hovered = false;
        DWORD lastInside = 0;

        // Settling: while explorer relayouts (it passes through intermediate
        // layouts), `display` only grows to cover every reading, and takes the
        // real reading once two reads in a row agree.
        std::optional<Islands> lastRead;
        Islands display;
        bool settling = false;
        int stableReads = 0;
        DWORD settleStart = 0;

        // What is on screen right now, so animation frames can redraw it.
        LONG logical = -1;             // the decided state
        LONG state = 0;                // the drawn state (islands while morphing to/from full width)
        bool fillWhenSettled = false;  // morphing towards full width
        RECT wr{};
        RECT fillSpan{};               // the full-width extent (minus a trimmed Show Desktop sliver)
        UINT dpi = 96;
        bool trimShowDesktop = false;
        RECT showDesktop{};
        std::vector<Span> target;      // where the islands should end up
        SpanStyle targetStyle{};
        std::vector<Span> shown;       // where they are drawn this frame
        SpanStyle style{};             // drawn style

        // Linear tween from `from` to `to` (which also holds shrinking islands).
        bool tweening = false;
        double tweenStart = 0, tweenDuration = 0;  // milliseconds
        std::vector<Span> from, to;
        SpanStyle fromStyle{}, toStyle{};

        std::unique_ptr<Backdrop> fill;    // custom background, below the taskbar
        std::unique_ptr<Backdrop> border;  // outline, above the taskbar
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
    void Layout(Taskbar& tb, bool force, const Context& ctx);
    // Folds a fresh reading into tb.display; returns true while still settling.
    bool Settle(Taskbar& tb, const Islands& fresh);
    // Moves towards tb.target/targetStyle: snaps, or (re)starts a tween from what is shown.
    void Retarget(Taskbar& tb, bool snap);
    // Applies tb.shown (region + layers) in tb.state; skips unchanged output unless `force`.
    void Present(Taskbar& tb, bool force);
    void SyncLayers(Taskbar& tb);
    bool FillVisible();
    bool BorderActive() const { return config_.borderWidth > 0 && config_.borderOpacity > 0; }
    Taskbar* Find(HWND hwnd);
    bool IsTaskbar(HWND hwnd) const;
    bool NeedsGlobalHooks() const;
    void InstallGlobalHooks();
    void RemoveGlobalHooks();

    static void CALLBACK ExplorerEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static void CALLBACK GlobalEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static Engine* s_instance;

    BoundsWorker worker_;
    Config config_;
    Callbacks callbacks_;
    std::vector<Taskbar> taskbars_;
    std::vector<HWINEVENTHOOK> explorerHooks_;
    std::vector<HWINEVENTHOOK> globalHooks_;
    DWORD explorerPid_ = 0;
    bool switching_ = false;
    bool translucentTbRunning_ = false;  // cached; checked every few seconds
    DWORD translucentTbCheckedAt_ = 0;
};

}  // namespace fb
