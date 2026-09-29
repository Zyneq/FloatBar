#include "engine.h"

#include <dwmapi.h>
#include <tlhelp32.h>

#include <algorithm>

#include "log.h"
#include "match_rules.h"
#include "region.h"
#include "uia_util.h"

namespace fb {
namespace {

// Consecutive failed reads tolerated before falling back to the unclipped
// taskbar; explorer briefly reports nothing while it relayouts.
constexpr int kFailuresBeforeUnclip = 3;
// How long the taskbar stays revealed after the mouse leaves it.
constexpr DWORD kHoverLingerMs = 600;

// How long after a region change it keeps being re-sent (see Engine::Refresh);
// the dropped regions measured came within a few ms of a change.
constexpr double kRefreshAfterChangeMs = 600;

enum State : LONG { kShapes = 0, kFilled = 1, kHidden = 2 };

// Island ids (see Span).
constexpr int kIdApp = 0;
constexpr int kIdTray = 1;
constexpr int kIdBar = 2;
constexpr int kIdExtra = 100;  // + index

std::wstring Label(HWND hwnd, bool primary) {
    return (primary ? L"primary " : L"secondary ") + FormatHwnd(hwnd);
}

std::wstring SpanText(const RECT& r) {
    return std::to_wstring(r.left) + L"–" + std::to_wstring(r.right);
}

std::wstring ProcessName(DWORD pid) {
    std::wstring name;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            name = path;
            name = name.substr(name.find_last_of(L'\\') + 1);
        }
        CloseHandle(process);
    }
    return name;
}

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

BOOL CALLBACK CollectMaximised(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || !IsZoomed(hwnd) || IsCloaked(hwnd)) return TRUE;
    auto& monitors = *reinterpret_cast<std::vector<HMONITOR>*>(lParam);
    HMONITOR m = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (m && std::find(monitors.begin(), monitors.end(), m) == monitors.end()) monitors.push_back(m);
    return TRUE;
}

bool IsProcessRunning(const wchar_t* exeName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry = {sizeof(entry)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry)) {
        found = _wcsicmp(entry.szExeFile, exeName) == 0;
    }
    CloseHandle(snapshot);
    return found;
}

RECT Padded(const RECT& r, int padding) { return {r.left - padding, r.top, r.right + padding, r.bottom}; }

bool SameSpans(const std::vector<Span>& a, const std::vector<Span>& b) {
    return std::ranges::equal(a, b, [](const Span& x, const Span& y) { return x.id == y.id && x.rect.left == y.rect.left && x.rect.right == y.rect.right; });
}

// The islands stretched until they tile `full` edge to edge: the neighbours meet
// halfway across each gap. With zero margins and radius this looks exactly like
// the full-width taskbar, so morphing between the two is seamless.
std::vector<Span> Expanded(std::vector<Span> spans, const RECT& full) {
    if (spans.empty()) return spans;
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.rect.left < b.rect.left; });
    spans.front().rect.left = full.left;
    spans.back().rect.right = full.right;
    for (size_t i = 0; i + 1 < spans.size(); ++i) {
        const LONG meet = (spans[i].rect.right + spans[i + 1].rect.left) / 2;
        spans[i].rect.right = meet;
        spans[i + 1].rect.left = meet;
    }
    return spans;
}

// Cuts the Show Desktop sliver off whichever end of `span` it sits at.
RECT WithoutShowDesktop(RECT span, const Islands& is) {
    if (!is.hasShowDesktop) return span;
    if (is.showDesktop.left >= (span.left + span.right) / 2) span.right = std::min(span.right, is.showDesktop.left);
    else span.left = std::max(span.left, is.showDesktop.right);
    return span;
}

// The monitor `hwnd` covers completely, if it is a fullscreen or borderless app.
HMONITOR FullscreenMonitor(HWND hwnd) {
    if (!hwnd) return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!IsWindowVisible(root) || IsIconic(root) || IsCloaked(root)) return nullptr;
    // The desktop and Alt+Tab / Task View also cover the monitor but are not apps.
    const std::wstring cls = WindowClass(root);
    if (rules::InList(cls, rules::kDesktopClasses) || rules::InList(cls, rules::kTaskSwitcherClasses)) return nullptr;
    HMONITOR monitor = MonitorFromWindow(root, MONITOR_DEFAULTTONULL);
    MONITORINFO mi = {sizeof(mi)};
    RECT wr;
    if (!monitor || !GetMonitorInfoW(monitor, &mi) || !GetWindowRect(root, &wr)) return nullptr;
    const RECT& m = mi.rcMonitor;
    const bool covers = wr.left <= m.left && wr.top <= m.top && wr.right >= m.right && wr.bottom >= m.bottom;
    return covers ? monitor : nullptr;
}

}  // namespace

std::wstring MonitorKey(HMONITOR monitor) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    return GetMonitorInfoW(monitor, &mi) ? std::wstring(mi.szDevice) : std::wstring();
}

Engine* Engine::s_instance = nullptr;

Engine::~Engine() {
    worker_.Stop();
    Detach();
    RemoveGlobalHooks();
}

HRESULT Engine::Init(Callbacks callbacks, HWND notifyWnd, UINT boundsMessage) {
    s_instance = this;
    callbacks_ = std::move(callbacks);
    return worker_.Start(notifyWnd, boundsMessage);
}

void Engine::SetConfig(const Config& config) {
    config_ = config;
    if (NeedsGlobalHooks()) InstallGlobalHooks();
    else RemoveGlobalHooks();
}

// ------------------------------------------------------------------ hooks

void Engine::Attach() {
    Detach();
    taskbars_.clear();

    const std::vector<HWND> hwnds = FindTaskbars();
    for (HWND hwnd : hwnds) {
        Taskbar tb;
        tb.hwnd = hwnd;
        tb.primary = WindowClass(hwnd) == L"Shell_TrayWnd";
        taskbars_.push_back(std::move(tb));
    }

    explorerPid_ = 0;
    if (!hwnds.empty()) GetWindowThreadProcessId(hwnds.front(), &explorerPid_);
    if (explorerPid_) {
        const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        // EVENT_OBJECT_CREATE..EVENT_OBJECT_REORDER covers CREATE, DESTROY, SHOW, HIDE and
        // REORDER (the taskbar re-raising itself, which must re-stack the border above it).
        if (HWINEVENTHOOK h = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER, nullptr, ExplorerEventProc, explorerPid_, 0, flags))
            explorerHooks_.push_back(h);
        if (HWINEVENTHOOK h = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, ExplorerEventProc, explorerPid_, 0, flags))
            explorerHooks_.push_back(h);
    }
    log::Write(L"attach: %zu taskbar(s), explorer pid %lu, %zu hook(s)", taskbars_.size(), explorerPid_, explorerHooks_.size());
}

void Engine::Detach() {
    for (HWINEVENTHOOK h : explorerHooks_) UnhookWinEvent(h);
    explorerHooks_.clear();
}

bool Engine::NeedsGlobalHooks() const {
    // Background and border layers also need foreground changes to keep their z-order
    // next to the taskbar (and out of the way of fullscreen apps).
    return config_.enabled && (config_.fillOnMaximise || config_.fillOnTaskSwitch || config_.autoHide ||
                               config_.hideOverFullscreen || config_.background != Background::Default || BorderActive() ||
                               (config_.mode == LayoutMode::Islands && config_.trayMode == TrayMode::Hover));
}

void Engine::InstallGlobalHooks() {
    if (!globalHooks_.empty()) return;
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    const DWORD ranges[][2] = {
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND},
        {EVENT_SYSTEM_SWITCHSTART, EVENT_SYSTEM_MINIMIZEEND},  // switch start/end, minimize start/end
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},             // destroy, show, hide
        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    };
    for (const auto& range : ranges) {
        if (HWINEVENTHOOK h = SetWinEventHook(range[0], range[1], nullptr, GlobalEventProc, 0, 0, flags)) globalHooks_.push_back(h);
    }
    log::Write(L"global window hooks installed (%zu)", globalHooks_.size());
}

void Engine::RemoveGlobalHooks() {
    if (globalHooks_.empty()) return;
    for (HWINEVENTHOOK h : globalHooks_) UnhookWinEvent(h);
    globalHooks_.clear();
    switching_ = false;
    log::Write(L"global window hooks removed");
}

bool Engine::TaskbarsChanged() const {
    const std::vector<HWND> now = FindTaskbars();
    if (now.size() != taskbars_.size()) return true;
    for (size_t i = 0; i < now.size(); ++i) {
        if (now[i] != taskbars_[i].hwnd) return true;
    }
    return false;
}

bool Engine::IsTaskbar(HWND hwnd) const {
    for (const Taskbar& tb : taskbars_) {
        if (tb.hwnd == hwnd) return true;
    }
    return false;
}

Engine::Taskbar* Engine::Find(HWND hwnd) {
    for (Taskbar& tb : taskbars_) {
        if (tb.hwnd == hwnd) return &tb;
    }
    return nullptr;
}

void CALLBACK Engine::ExplorerEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (!s_instance || !hwnd || idObject == OBJID_CURSOR || idObject == OBJID_CARET) return;
    // explorer.exe also hosts File Explorer windows; only react to the taskbars,
    // and only re-read the one that changed (reads are the scarce resource).
    Taskbar* tb = s_instance->Find(GetAncestor(hwnd, GA_ROOT));
    if (!tb) return;
    tb->dirty = true;
    if (s_instance->callbacks_.taskbarChanged) s_instance->callbacks_.taskbarChanged();
}

void Engine::MarkAllDirty() {
    for (Taskbar& tb : taskbars_) tb.dirty = true;
}

void CALLBACK Engine::GlobalEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    Engine* self = s_instance;
    if (!self) return;
    switch (event) {
        case EVENT_SYSTEM_SWITCHSTART: self->switching_ = true; break;
        case EVENT_SYSTEM_SWITCHEND: self->switching_ = false; break;
        case EVENT_SYSTEM_FOREGROUND:
        case EVENT_SYSTEM_MOVESIZEEND:
        case EVENT_SYSTEM_MINIMIZESTART:
        case EVENT_SYSTEM_MINIMIZEEND:
            break;
        default:
            // Object events: only top-level windows matter.
            if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
            if (GetAncestor(hwnd, GA_ROOT) != hwnd || self->IsTaskbar(hwnd)) return;
            if (event == EVENT_OBJECT_LOCATIONCHANGE && !self->config_.fillOnMaximise && !self->config_.hideOverFullscreen) return;
            break;
    }
    if (self->callbacks_.windowsChanged) self->callbacks_.windowsChanged();
}

// ------------------------------------------------------------------ hover

bool Engine::NeedsHoverPolling() const {
    return config_.enabled && (config_.autoHide || (config_.mode == LayoutMode::Islands && config_.trayMode == TrayMode::Hover));
}

bool Engine::PollHover() {
    POINT pt;
    if (!GetCursorPos(&pt)) return false;
    const DWORD now = GetTickCount();
    bool changed = false;
    for (Taskbar& tb : taskbars_) {
        RECT wr;
        if (!GetWindowRect(tb.hwnd, &wr)) continue;
        if (PtInRect(&wr, pt)) {
            tb.lastInside = now;
            if (!tb.hovered) tb.hovered = changed = true;
        } else if (tb.hovered && now - tb.lastInside > kHoverLingerMs) {
            tb.hovered = false;
            changed = true;
        }
    }
    return changed;
}

// ------------------------------------------------------------------ update

Engine::Context Engine::BuildContext() const {
    Context ctx;
    if (HWND fg = GetForegroundWindow()) {
        HWND root = GetAncestor(fg, GA_ROOT);
        const std::wstring cls = WindowClass(root);
        ctx.foregroundClass = cls;
        DWORD pid = 0;
        GetWindowThreadProcessId(root, &pid);
        ctx.taskSwitch = switching_ || rules::InList(cls, rules::kTaskSwitcherClasses);
        if (IsTaskbar(root)) ctx.shellUi = true;
        else if (pid == explorerPid_) ctx.shellUi = !rules::InList(cls, rules::kExplorerNonShellClasses);
        else if (config_.autoHide || config_.trayMode == TrayMode::Hover) ctx.shellUi = rules::InList(ProcessName(pid), rules::kShellProcesses);
    }
    if (config_.fillOnMaximise) EnumWindows(CollectMaximised, reinterpret_cast<LPARAM>(&ctx.maximised));
    if (config_.hideOverFullscreen) {
        if (HMONITOR m = FullscreenMonitor(GetForegroundWindow())) ctx.fullscreen.push_back(m);
    }
    return ctx;
}

void Engine::Update(bool force, bool readBounds) {
    if (!config_.enabled) return;
    const Context ctx = BuildContext();
    log::Debug(L"update force=%d bounds=%d fg=%s shellUi=%d taskSwitch=%d maximisedMonitors=%zu fullscreenMonitors=%zu", force,
               readBounds, ctx.foregroundClass.c_str(), ctx.shellUi, ctx.taskSwitch, ctx.maximised.size(), ctx.fullscreen.size());
    for (Taskbar& tb : taskbars_) {
        if (readBounds && (force || tb.dirty)) {
            worker_.Request(tb.hwnd);
            tb.dirty = false;
            if (force) tb.forcePending = true;
        }
        if (tb.islands && IsWindow(tb.hwnd)) Layout(tb, force, ctx, false);
    }
}

Engine::UpdateResult Engine::OnBounds(BoundsWorker::Reply* raw) {
    const std::unique_ptr<BoundsWorker::Reply> reply(raw);
    UpdateResult result;
    Taskbar* tb = Find(reply->taskbar);
    if (!tb || !config_.enabled || !IsWindow(tb->hwnd)) return result;  // e.g. from before a re-attach

    const std::wstring label = Label(tb->hwnd, tb->primary);
    const BoundsResult& read = reply->result;
    const bool force = tb->forcePending;
    tb->forcePending = false;

    if (!read.islands) {
        log::Debug(L"%s: bounds read in %lu ms: %s", label.c_str(), reply->ms, read.error.c_str());
        ++tb->failures;
        if (tb->lastError != read.error) {
            log::Write(L"%s: bounds unavailable: %s", label.c_str(), read.error.c_str());
            tb->lastError = read.error;
        }
        if (tb->applied && tb->failures < kFailuresBeforeUnclip) {
            result.retry = true;
            return result;
        }
        tb->islands.reset();
        tb->filter.Reset();
        if (tb->applied || force) {
            ClearRegion(tb->hwnd);
            if (tb->applied) log::Write(L"%s: falling back to unclipped taskbar", label.c_str());
            tb->applied.reset();
        }
        // The custom background (if any) follows the now full-width taskbar.
        GetWindowRect(tb->hwnd, &tb->wr);
        tb->dpi = GetDpiForWindow(tb->hwnd) ? GetDpiForWindow(tb->hwnd) : 96;
        tb->state = tb->logical = kFilled;
        tb->fillWhenSettled = false;
        tb->trimShowDesktop = false;
        tb->fillSpan = tb->wr;
        tb->motion.Clear();
        SyncLayers(*tb);
        tb->status = L"unclipped – " + read.error;
        return result;
    }

    tb->failures = 0;
    tb->lastError.clear();
    tb->islands = *read.islands;
    const bool wasMoving = tb->filter.Transitioning();
    RECT wr = {};
    GetWindowRect(tb->hwnd, &wr);
    tb->display = tb->filter.Apply(*read.islands, GetDpiForWindow(tb->hwnd), NowMs(), wr.left + wr.right);
    // UI Automation reports explorer's last slide pixel late; look once more.
    // The same confirms that the buttons stood still before the first clip.
    result.recheck = (wasMoving && !tb->filter.Transitioning()) || (!tb->applied && !tb->filter.Settled(NowMs()));
    const TrackedEdges& tracked = tb->filter.Tracked();
    log::Debug(L"%s: bounds read in %lu ms: app %s #%d -> %s%s%s%s", label.c_str(), reply->ms, FormatRect(read.islands->app).c_str(),
               read.islands->appCount, SpanText(tb->display.app).c_str(), tracked.appLeft ? L" trackL" : L"", tracked.appRight ? L" trackR" : L"",
               tb->filter.Transitioning() ? L" (moving)" : L"");
    if (tb->filter.Transitioning()) {
        worker_.Request(tb->hwnd);  // read back to back while buttons move
        // Explorer moving buttons is when DWM drops regions, even a region that stays put.
        tb->refreshUntil = std::max(tb->refreshUntil, NowMs() + kRefreshAfterChangeMs);
    }
    Layout(*tb, force, BuildContext(), true);
    return result;
}

void Engine::Layout(Taskbar& tb, bool force, const Context& ctx, bool freshReading) {
    const std::wstring label = Label(tb.hwnd, tb.primary);
    if (!tb.applied && !tb.filter.Settled(NowMs())) {
        tb.status = L"waiting for the taskbar buttons to settle";
        return;
    }
    // Shapes follow the filtered view; the sliver comes from the latest reading.
    Islands is = tb.display;
    is.hasShowDesktop = tb.islands->hasShowDesktop;
    is.showDesktop = tb.islands->showDesktop;

    RECT wr = {};
    GetWindowRect(tb.hwnd, &wr);
    UINT dpi = GetDpiForWindow(tb.hwnd);
    if (!dpi) dpi = 96;
    auto scale = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
    const SpanStyle style{scale(config_.marginTop), scale(config_.marginBottom), scale(config_.cornerRadius)};
    const int padding = scale(config_.islandPadding);

    // Decide what this taskbar should look like right now.
    const bool revealed = tb.hovered || ctx.shellUi;
    const HMONITOR monitor = MonitorFromWindow(tb.hwnd, MONITOR_DEFAULTTONEAREST);
    auto onThisMonitor = [monitor](const std::vector<HMONITOR>& list) {
        return std::find(list.begin(), list.end(), monitor) != list.end();
    };
    const auto modeIt = config_.monitorModes.find(MonitorKey(monitor));
    const MonitorMode monitorMode = modeIt == config_.monitorModes.end() ? MonitorMode::Default : modeIt->second;
    const bool trimShowDesktop = config_.hideShowDesktop && is.hasShowDesktop;

    State state = kShapes;
    std::wstring why;
    if (monitorMode == MonitorMode::Hidden) {
        state = kHidden;
        why = L"hidden on this monitor";
    } else if (config_.hideOverFullscreen && onThisMonitor(ctx.fullscreen)) {
        state = kHidden;
        why = L"fullscreen app";
    } else if (monitorMode == MonitorMode::Normal) {
        state = kFilled;
        why = L"normal taskbar on this monitor";
    } else if (config_.autoHide && !revealed) {
        state = kHidden;
        why = L"auto-hide";
    } else if (config_.fillOnTaskSwitch && ctx.taskSwitch) {
        state = kFilled;
        why = L"task switcher open";
    } else if (config_.fillOnMaximise && onThisMonitor(ctx.maximised)) {
        state = kFilled;
        why = L"window maximised";
    }

    // The islands this taskbar would show in islands mode. They are also the
    // start/end point of the morph to and from full width.
    // Edges that just followed explorer's slide (only meaningful right after a read).
    const TrackedEdges tr = freshReading ? tb.filter.Tracked() : TrackedEdges{};
    const RECT fillSpan = trimShowDesktop ? WithoutShowDesktop(wr, is) : wr;
    std::vector<Span> islands;
    bool trayShown = false;
    if (config_.mode == LayoutMode::Bar) {
        const RECT bar = {wr.left + padding, wr.top, wr.right - padding, wr.bottom};
        islands.push_back({trimShowDesktop ? WithoutShowDesktop(bar, is) : bar, kIdBar});
    } else {
        islands.push_back({Padded(is.app, padding), kIdApp, tr.appLeft, tr.appRight});
        if (config_.showWidgets) {
            for (size_t i = 0; i < is.extras.size(); ++i) islands.push_back({Padded(is.extras[i], padding), kIdExtra + static_cast<int>(i)});
        }
        trayShown = is.hasTray && (config_.trayMode == TrayMode::Show || (config_.trayMode == TrayMode::Hover && revealed));
        if (trayShown) islands.push_back({Padded(is.tray, padding), kIdTray, tr.trayLeft, tr.trayRight});
    }

    switch (state) {
        case kHidden: tb.status = L"hidden – " + why; break;
        case kFilled: tb.status = L"full width – " + why; break;
        default:
            if (config_.mode == LayoutMode::Bar) {
                tb.status = L"single bar";
            } else {
                tb.status = std::to_wstring(is.appCount) + L" app buttons [" + SpanText(is.app) + L"]";
                if (!is.extras.empty()) tb.status += L", +" + std::to_wstring(is.extras.size()) + (config_.showWidgets ? L" extra" : L" extra hidden");
                if (is.hasTray) tb.status += L", tray [" + SpanText(is.tray) + L"]" + (trayShown ? L"" : L" hidden");
            }
    }

    // Animate between islands and full width on an unchanged screen; everything
    // else (first frame, hidden, forced refresh, DPI or resolution change) snaps.
    const bool logicalChanged = tb.logical != state;
    const bool canAnimate = config_.animate && !force && tb.applied && EqualRect(&tb.wr, &wr) && tb.dpi == dpi;
    const std::vector<Span> previousTarget = tb.motion.Target();
    tb.wr = wr;
    tb.dpi = dpi;
    tb.trimShowDesktop = trimShowDesktop;
    tb.showDesktop = is.showDesktop;
    tb.fillSpan = fillSpan;
    constexpr SpanStyle kFlat{0, 0, 0};
    const double glideMs = kGlideMs * 100.0 / config_.animationSpeed;
    const double minSpeed = kTrackSpeedLogicalPxPerMs * dpi / 96.0;  // for tracked edges only
    const double now = NowMs();

    if (state == kHidden) {
        tb.state = kHidden;
        tb.fillWhenSettled = false;
        tb.motion.Clear();
    } else if (state == kFilled) {
        const std::vector<Span> expanded = Expanded(islands, fillSpan);
        if (canAnimate && tb.state == kShapes) {
            // Morph out: stretch the islands edge to edge and flatten them, then
            // hand over to the real full-width taskbar once they arrive.
            tb.fillWhenSettled = true;
            tb.motion.SetTarget(expanded, kFlat, false, glideMs, minSpeed, now);
        } else {
            tb.state = kFilled;
            tb.fillWhenSettled = false;
            tb.motion.SetTarget(expanded, kFlat, true, glideMs, minSpeed, now);
        }
    } else {
        const bool snap = !canAnimate || tb.state == kHidden;
        if (!snap && tb.state == kFilled) tb.motion.Place(Expanded(islands, fillSpan), kFlat);  // morph in from full width
        tb.state = kShapes;
        tb.fillWhenSettled = false;
        tb.motion.SetTarget(islands, style, snap, glideMs, minSpeed, now);
    }
    tb.logical = state;
    Present(tb, force);

    if (state == kShapes && (logicalChanged || (!tb.filter.Transitioning() && !SameSpans(previousTarget, tb.motion.Target())))) {
        log::Write(L"%s: app=%s (%d) extras=%zu tray=%s dpi=%u", label.c_str(), FormatRect(is.app).c_str(), is.appCount, is.extras.size(),
                   trayShown ? FormatRect(is.tray).c_str() : L"-", dpi);
    } else if (logicalChanged) {
        log::Write(L"%s: %s", label.c_str(), tb.status.c_str());
    }
}

void Engine::Present(Taskbar& tb, bool force) {
    ApplyRegion(tb, force);
    // After the region: SetWindowRgn waits for explorer, so layers updated first
    // were shown a frame ahead of the island they outline.
    SyncLayers(tb);
}

void Engine::ApplyRegion(Taskbar& tb, bool force) {
    const std::vector<Span> shown = tb.motion.Shown();
    const SpanStyle style = tb.motion.ShownStyle();
    Key key = {tb.state, tb.wr.left, tb.wr.top, tb.wr.right, tb.wr.bottom, style.marginTop, style.marginBottom, style.radius,
               tb.trimShowDesktop ? 1 : 0, tb.fillSpan.left, tb.fillSpan.right};
    if (tb.state == kShapes) {
        for (const Span& s : shown) key.insert(key.end(), {s.id, s.rect.left, s.rect.right});
    }

    // Something else removed our region while the state stayed the same.
    const bool regionLost = tb.state != kFilled && tb.applied && (*tb.applied)[0] == tb.state && !HasRegion(tb.hwnd);
    if (!force && tb.applied == key && !regionLost) return;

    bool ok = true;
    const double started = NowMs();
    switch (tb.state) {
        case kFilled:
            if (tb.trimShowDesktop) ok = ApplyFullExcept(tb.hwnd, tb.wr, tb.showDesktop);
            else ClearRegion(tb.hwnd);
            break;
        case kHidden: ok = HideTaskbar(tb.hwnd); break;
        default: {
            std::vector<RECT> rects;
            for (const Span& s : shown) rects.push_back(s.rect);
            ok = ApplySpans(tb.hwnd, tb.wr, rects, style);
            break;
        }
    }
    if (!ok) {
        log::Write(L"%s: SetWindowRgn failed (%lu)", Label(tb.hwnd, tb.primary).c_str(), GetLastError());
        tb.status = L"SetWindowRgn failed";
        return;
    }
    if (regionLost) log::Write(L"%s: region was reset by something else, reapplied", Label(tb.hwnd, tb.primary).c_str());
    tb.regionAt = NowMs();
    tb.refreshUntil = std::max(tb.refreshUntil, tb.regionAt + kRefreshAfterChangeMs);
    if (log::Verbose()) {
        // One line per drawn frame, stamped with the high-resolution clock
        // (the same clock tools/probe uses, so the two line up).
        std::wstring spans;
        if (tb.state == kShapes) {
            for (const Span& s : shown) spans += L" " + std::to_wstring(s.id) + L":" + SpanText(s.rect);
        }
        log::Debug(L"%s: frame t=%.1f (%.1f ms) state %ld r=%d%s%s", Label(tb.hwnd, tb.primary).c_str(), started, NowMs() - started, tb.state,
                   style.radius, spans.c_str(), force ? L" forced" : L"");
    }
    tb.applied = key;
}

bool Engine::FillVisible() {
    // The fill sits below the taskbar, so it only shows (instead of leaving a
    // jagged fringe around the clip) when TranslucentTB has made the taskbar clear.
    const DWORD now = GetTickCount();
    if (!translucentTbCheckedAt_ || now - translucentTbCheckedAt_ > 3000) {
        translucentTbRunning_ = IsProcessRunning(L"TranslucentTB.exe");
        translucentTbCheckedAt_ = now ? now : 1;
    }
    return translucentTbRunning_;
}

void Engine::SyncLayers(Taskbar& tb) {
    std::vector<RECT> shapes;
    const SpanStyle style = tb.motion.ShownStyle();
    int radius = style.radius;
    if (tb.state == kFilled) {
        shapes.push_back({tb.fillSpan.left - tb.wr.left, 0, tb.fillSpan.right - tb.wr.left, tb.wr.bottom - tb.wr.top});
        radius = 0;
    } else if (tb.state == kShapes) {
        for (const Span& s : tb.motion.Shown()) shapes.push_back(SpanToWindowRect(tb.wr, s.rect, style));
    }

    bool placed = true;
    auto sync = [&](std::unique_ptr<Backdrop>& layer, bool wanted, Backdrop::Layer kind) {
        if (!wanted) {
            layer.reset();
            return;
        }
        if (!layer) layer = std::make_unique<Backdrop>(kind);
        if (shapes.empty()) layer->Hide();
        else placed &= layer->Show(tb.hwnd, tb.wr, shapes, radius, config_, tb.dpi);
    };
    sync(tb.fill, config_.background != Background::Default && FillVisible(), Backdrop::Layer::Fill);
    // The border stays on at full width too, so the morph ends without a pop.
    sync(tb.border, BorderActive(), Backdrop::Layer::Border);
    if (placed == tb.misplaced) {
        log::Debug(L"%s: %s", Label(tb.hwnd, tb.primary).c_str(),
                   placed ? L"layers are next to the taskbar again" : L"explorer lifted the taskbar above its layers; retrying");
    }
    tb.misplaced = !placed;
}

bool Engine::NeedsRefresh() const {
    const double now = NowMs();
    for (const Taskbar& tb : taskbars_) {
        if (tb.misplaced || (tb.applied && tb.state != kFilled && now < tb.refreshUntil)) return true;
    }
    return false;
}

void Engine::Refresh() {
    const double now = NowMs();
    for (Taskbar& tb : taskbars_) {
        if (!IsWindow(tb.hwnd)) continue;
        if (tb.misplaced) SyncLayers(tb);
        if (!tb.applied || tb.state == kFilled || now >= tb.refreshUntil) continue;
        if (now - tb.regionAt < kRefreshMs) continue;  // set just now anyway
        const double until = tb.refreshUntil;
        ApplyRegion(tb, true);
        tb.refreshUntil = until;  // re-sending is not a change
    }
}

bool Engine::Animating() const {
    for (const Taskbar& tb : taskbars_) {
        if (tb.motion.Moving() || tb.fillWhenSettled) return true;
    }
    return false;
}

void Engine::Animate() {
    const double now = NowMs();
    for (Taskbar& tb : taskbars_) {
        const bool moved = tb.motion.Step(now);
        if (!tb.motion.Moving() && tb.fillWhenSettled) {
            // Stretched edge to edge and flat: now it is the full-width taskbar.
            tb.state = kFilled;
            tb.fillWhenSettled = false;
            Present(tb, false);
        } else if (moved) {
            Present(tb, false);
        }
    }
}

void Engine::ClearAll() {
    for (Taskbar& tb : taskbars_) {
        if (IsWindow(tb.hwnd)) ClearRegion(tb.hwnd);
        tb.applied.reset();
        tb.fill.reset();
        tb.border.reset();
        tb.misplaced = false;
        tb.motion.Clear();
        tb.filter.Reset();
        tb.logical = -1;
        tb.state = kShapes;
        tb.fillWhenSettled = false;
    }
    ClearAllTaskbars();
}

std::vector<MonitorEntry> Engine::Monitors() const {
    std::vector<MonitorEntry> result;
    int secondary = 0;
    for (const Taskbar& tb : taskbars_) {
        HMONITOR monitor = MonitorFromWindow(tb.hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(monitor, &mi);
        const std::wstring size = std::to_wstring(mi.rcMonitor.right - mi.rcMonitor.left) + L"×" +
                                  std::to_wstring(mi.rcMonitor.bottom - mi.rcMonitor.top);
        const std::wstring name = tb.primary ? std::wstring(L"Main taskbar") : L"Monitor " + std::to_wstring(++secondary + 1);
        result.push_back({MonitorKey(monitor), name + L" (" + size + L")"});
    }
    return result;
}

std::wstring Engine::Status() const {
    if (!config_.enabled) return L"Disabled – taskbar is unclipped.";
    if (taskbars_.empty()) return L"No taskbar found.";
    std::wstring s;
    int secondary = 0;
    for (const Taskbar& tb : taskbars_) {
        if (!s.empty()) s += L"\r\n";
        s += tb.primary ? std::wstring(L"Main taskbar: ") : L"Monitor " + std::to_wstring(++secondary + 1) + L": ";
        s += tb.status.empty() ? L"pending" : tb.status;
    }
    if (config_.background != Background::Default && !translucentTbRunning_) {
        s += L"\r\n⚠ TranslucentTB is not running, so the custom background is off. Start it with the taskbar set to Clear.";
    }
    return s;
}

}  // namespace fb
