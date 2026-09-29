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
#include "motion.h"
#include "region.h"

namespace fb {

// Stable-enough monitor identity for per-monitor settings, e.g. \\.\DISPLAY2.
std::wstring MonitorKey(HMONITOR monitor);

struct MonitorEntry {
    std::wstring key;    // MonitorKey()
    std::wstring label;  // "Main taskbar (1920×1080)"
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
        bool retry = false;    // a read failed transiently: read again soon
        bool recheck = false;  // a relayout just ended: read once more a bit later
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
    // `readBounds`, also asks the worker for fresh bounds of the taskbars that
    // changed (all of them with `force`). `force` also redraws even when nothing
    // changed and skips animation.
    void Update(bool force, bool readBounds);
    // Makes the next Update(readBounds) read every taskbar (poll, display change).
    void MarkAllDirty();
    // A read finished (from the worker's message); takes ownership of `reply`.
    // While that taskbar's buttons still move, it asks for the next read at once.
    UpdateResult OnBounds(BoundsWorker::Reply* reply);

    // Mouse tracking for auto-hide and the hover tray. Returns true if any
    // taskbar's hover state changed.
    bool NeedsHoverPolling() const;
    bool PollHover();

    // Island animation. While Animating(), call Animate() once per composition
    // frame; it positions everything for "now".
    bool Animating() const;
    void Animate();

    // While NeedsRefresh(), call Refresh() every kRefreshMs. It re-sends regions:
    // now and then DWM shows a taskbar unclipped right after its region changed
    // while explorer is busy (the region stays set) until the region is set
    // again; seen lasting up to 460 ms. Re-sending it to an idle taskbar never
    // did that (44,000 calls measured). And it puts the border back above a
    // taskbar that explorer lifted out of its reach (Start or Search open).
    static constexpr UINT kRefreshMs = 50;
    bool NeedsRefresh() const;
    void Refresh();

    void ClearAll();
    std::wstring Status() const;
    std::vector<MonitorEntry> Monitors() const;

private:
    using Key = std::vector<LONG>;

    struct Taskbar {
        HWND hwnd = nullptr;
        bool primary = false;
        std::optional<Islands> islands;  // latest good reading
        Islands display;                 // the same, filtered so it never cuts an icon
        ReadingFilter filter;
        std::optional<Key> applied;
        int failures = 0;
        bool forcePending = false;       // a forced update is waiting for its read
        bool dirty = true;               // explorer reported a change: read it next time
        std::wstring lastError;
        std::wstring status;
        bool hovered = false;
        DWORD lastInside = 0;

        // What is drawn right now.
        LONG logical = -1;             // the decided state
        LONG state = 0;                // the drawn state (islands while morphing to/from full width)
        bool fillWhenSettled = false;  // morphing towards full width
        RECT wr{};
        RECT fillSpan{};               // the full-width extent (minus a trimmed Show Desktop sliver)
        UINT dpi = 96;
        bool trimShowDesktop = false;
        RECT showDesktop{};
        Pursuit motion;                // drawn islands, moving towards their target
        double regionAt = 0;           // when the region was last set (NowMs)
        double refreshUntil = 0;       // re-send it until then (explorer busy, or just changed)

        std::unique_ptr<Backdrop> fill;    // custom background, below the taskbar
        std::unique_ptr<Backdrop> border;  // outline, above the taskbar
        bool misplaced = false;            // a layer could not be stacked next to the taskbar
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
    // `freshReading`: called right after a read (edge tracking hints are current).
    void Layout(Taskbar& tb, bool force, const Context& ctx, bool freshReading);
    // Applies what tb.motion shows (region + layers) in tb.state; skips an unchanged region unless `force`.
    void Present(Taskbar& tb, bool force);
    void ApplyRegion(Taskbar& tb, bool force);
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
