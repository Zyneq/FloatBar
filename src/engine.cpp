#include "engine.h"

#include <dwmapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>

#include "uia_util.h"
#include "log.h"
#include "match_rules.h"
#include "region.h"

namespace fb {
namespace {

// Consecutive failed reads tolerated before falling back to the unclipped
// taskbar; explorer briefly reports nothing while it relayouts.
constexpr int kFailuresBeforeUnclip = 3;
// How long the taskbar stays revealed after the mouse leaves it.
constexpr DWORD kHoverLingerMs = 600;

enum State : LONG { kShapes = 0, kFilled = 1, kHidden = 2 };

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

RECT Padded(const RECT& r, int padding) { return {r.left - padding, r.top, r.right + padding, r.bottom}; }

// Island ids (see Span).
constexpr int kIdApp = 0;
constexpr int kIdTray = 1;
constexpr int kIdBar = 2;
constexpr int kIdStart = 3;
// Gap between the Start island and the app island ("separate Start").
constexpr int kStartGapLogicalPx = 8;
// Overshoot ignored while explorer slides buttons (see GrowTo); icons sit >= 10 px inside their button.
constexpr int kSlideToleranceLogicalPx = 12;
constexpr int kIdExtra = 100;  // + index

const Span* FindSpan(const std::vector<Span>& spans, int id) {
    for (const Span& s : spans) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

// Same islands at the same horizontal positions (height comes from the style).
bool SameSpans(const std::vector<Span>& a, const std::vector<Span>& b) {
    if (a.size() != b.size()) return false;
    for (const Span& s : a) {
        const Span* t = FindSpan(b, s.id);
        if (!t || t->rect.left != s.rect.left || t->rect.right != s.rect.right) return false;
    }
    return true;
}

bool SameStyle(const SpanStyle& a, const SpanStyle& b) {
    return a.marginTop == b.marginTop && a.marginBottom == b.marginBottom && a.radius == b.radius;
}

// Island motion is linear in time (constant speed, no ease ramps), sampled
// once per display frame. The duration follows the distance, within limits.
constexpr double kTweenMsPerPixel = 0.3;
constexpr double kMinTweenMs = 120;
constexpr double kMaxTweenMs = 280;

double NowMs() {
    static const double ticksPerMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / ticksPerMs;
}

LONG Lerp(LONG a, LONG b, double t) { return a + static_cast<LONG>(std::lround((b - a) * t)); }
int Lerp(int a, int b, double t) { return a + static_cast<int>(std::lround((b - a) * t)); }

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

bool SameIslands(const Islands& a, const Islands& b) {
    if (!EqualRect(&a.app, &b.app) || a.hasTray != b.hasTray || (a.hasTray && !EqualRect(&a.tray, &b.tray))) return false;
    if (a.hasSplit != b.hasSplit || a.split != b.split) return false;
    if (a.extras.size() != b.extras.size()) return false;
    for (size_t i = 0; i < a.extras.size(); ++i) {
        if (!EqualRect(&a.extras[i], &b.extras[i])) return false;
    }
    return true;
}

// Grows `held` to cover `fresh`, ignoring overshoots of up to `tolerance` px:
// explorer's slide animation nudges buttons by a few pixels, which only touches
// the empty margin around their icons and isn't worth an out-and-back wobble.
void GrowTo(RECT& held, const RECT& fresh, LONG tolerance) {
    if (fresh.left < held.left - tolerance) held.left = fresh.left;
    if (fresh.right > held.right + tolerance) held.right = fresh.right;
}

// `fresh`, but with the app and tray islands never smaller than `held`, so a
// mid-relayout reading doesn't make an island shrink and grow again.
Islands Merged(const Islands& held, const Islands& fresh, LONG tolerance) {
    Islands out = fresh;
    out.app = held.app;
    GrowTo(out.app, fresh.app, tolerance);
    if (held.hasTray && fresh.hasTray) {
        out.tray = held.tray;
        GrowTo(out.tray, fresh.tray, tolerance);
    }
    return out;
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

namespace {

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

}  // namespace

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

void CALLBACK Engine::ExplorerEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (!s_instance || !hwnd || idObject == OBJID_CURSOR || idObject == OBJID_CARET) return;
    // explorer.exe also hosts File Explorer windows; only react to the taskbars.
    if (!s_instance->IsTaskbar(GetAncestor(hwnd, GA_ROOT))) return;
    if (s_instance->callbacks_.taskbarChanged) s_instance->callbacks_.taskbarChanged();
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
        if (readBounds) {
            worker_.Request(tb.hwnd);
            if (force) tb.forcePending = true;
        }
        if (tb.islands && IsWindow(tb.hwnd)) Layout(tb, force, ctx);
    }
}

void Engine::RequestSettlingReads() {
    for (const Taskbar& tb : taskbars_) {
        if (tb.settling) worker_.Request(tb.hwnd);
    }
}

Engine::UpdateResult Engine::OnBounds(BoundsWorker::Reply* raw) {
    const std::unique_ptr<BoundsWorker::Reply> reply(raw);
    UpdateResult result;
    Taskbar* tb = Find(reply->taskbar);
    if (!tb || !config_.enabled || !IsWindow(tb->hwnd)) return result;  // e.g. from before a re-attach

    const std::wstring label = Label(tb->hwnd, tb->primary);
    const BoundsResult& read = reply->result;
    log::Debug(L"%s: bounds read in %lu ms: %s", label.c_str(), reply->ms,
               read.islands ? (L"app " + FormatRect(read.islands->app) + L", " + std::to_wstring(read.islands->extras.size()) +
                               L" extra, tray " + (read.islands->hasTray ? FormatRect(read.islands->tray) : L"-")).c_str()
                            : read.error.c_str());
    const bool force = tb->forcePending;
    tb->forcePending = false;

    if (!read.islands) {
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
        tb->lastRead.reset();
        tb->settling = false;
        if (tb->applied || force) {
            ClearRegion(tb->hwnd);
            if (tb->applied) log::Write(L"%s: falling back to unclipped taskbar", label.c_str());
            tb->applied.reset();
        }
        // The custom background (if any) follows the now full-width taskbar.
        GetWindowRect(tb->hwnd, &tb->wr);
        tb->dpi = GetDpiForWindow(tb->hwnd) ? GetDpiForWindow(tb->hwnd) : 96;
        tb->state = tb->logical = kFilled;
        tb->fillWhenSettled = tb->tweening = false;
        tb->trimShowDesktop = false;
        tb->fillSpan = tb->wr;
        tb->shown.clear();
        tb->target.clear();
        SyncLayers(*tb);
        tb->status = L"unclipped – " + read.error;
        return result;
    }

    tb->failures = 0;
    tb->lastError.clear();
    tb->islands = *read.islands;
    result.reread = Settle(*tb, *read.islands);
    Layout(*tb, force, BuildContext());
    return result;
}

bool Engine::Settle(Taskbar& tb, const Islands& fresh) {
    constexpr int kStableReadsToSettle = 2;
    constexpr DWORD kMaxSettleMs = 1500;  // never wait longer than this for explorer
    const LONG tolerance = MulDiv(kSlideToleranceLogicalPx, static_cast<int>(tb.dpi ? tb.dpi : 96), 96);
    const DWORD now = GetTickCount();
    const bool first = !tb.lastRead;
    const bool changed = first || !SameIslands(fresh, *tb.lastRead);
    // A button appeared or disappeared: the reading right after it is already
    // close to the final layout (explorer's slide animation follows it), so it
    // becomes the new base instead of being merged with the old, wider layout.
    // Without this a closing app kept its space until the slide had finished.
    const bool countChanged = !first && (fresh.appCount != tb.lastRead->appCount || fresh.trayCount != tb.lastRead->trayCount ||
                                         fresh.extras.size() != tb.lastRead->extras.size());
    tb.lastRead = fresh;

    if (first || !config_.animate) {
        tb.settling = false;
        tb.display = fresh;
    } else if (changed) {
        if (!tb.settling) {
            tb.settling = true;
            tb.settleStart = now;
        }
        tb.stableReads = 0;
        tb.display = countChanged ? fresh : Merged(tb.display, fresh, tolerance);
    } else if (tb.settling) {
        if (++tb.stableReads >= kStableReadsToSettle) tb.settling = false;
        tb.display = tb.settling ? Merged(tb.display, fresh, tolerance) : fresh;
    } else {
        tb.display = fresh;
    }
    if (tb.settling && now - tb.settleStart > kMaxSettleMs) {
        tb.settling = false;
        tb.display = fresh;
    }
    return tb.settling;
}

void Engine::Layout(Taskbar& tb, bool force, const Context& ctx) {
    const std::wstring label = Label(tb.hwnd, tb.primary);
    // Shape decisions use the settled view; the sliver comes from the latest reading.
    Islands is = tb.lastRead ? tb.display : *tb.islands;
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
    const RECT fillSpan = trimShowDesktop ? WithoutShowDesktop(wr, is) : wr;
    std::vector<Span> islands;
    bool trayShown = false;
    if (config_.mode == LayoutMode::Bar) {
        const RECT bar = {wr.left + padding, wr.top, wr.right - padding, wr.bottom};
        islands.push_back({trimShowDesktop ? WithoutShowDesktop(bar, is) : bar, kIdBar});
    } else {
        if (config_.separateStart && is.hasSplit) {
            // Cut a gap where the app buttons begin. Both neighbouring buttons have
            // empty space beside their icons, so the gap never cuts an icon.
            const LONG half = scale(kStartGapLogicalPx) / 2;
            islands.push_back({{is.app.left - padding, is.app.top, is.split - half, is.app.bottom}, kIdStart});
            islands.push_back({{is.split + half, is.app.top, is.app.right + padding, is.app.bottom}, kIdApp});
        } else {
            islands.push_back({Padded(is.app, padding), kIdApp});
        }
        if (config_.showWidgets) {
            for (size_t i = 0; i < is.extras.size(); ++i) islands.push_back({Padded(is.extras[i], padding), kIdExtra + static_cast<int>(i)});
        }
        trayShown = is.hasTray && (config_.trayMode == TrayMode::Show || (config_.trayMode == TrayMode::Hover && revealed));
        if (trayShown) islands.push_back({Padded(is.tray, padding), kIdTray});
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
    const std::vector<Span> previousTarget = tb.target;
    tb.wr = wr;
    tb.dpi = dpi;
    tb.trimShowDesktop = trimShowDesktop;
    tb.showDesktop = is.showDesktop;
    tb.fillSpan = fillSpan;
    constexpr SpanStyle kFlat{0, 0, 0};

    if (state == kHidden) {
        tb.state = kHidden;
        tb.fillWhenSettled = tb.tweening = false;
        tb.shown.clear();
        tb.target.clear();
    } else if (state == kFilled) {
        tb.target = Expanded(islands, fillSpan);
        tb.targetStyle = kFlat;
        if (canAnimate && tb.state == kShapes) {
            // Morph out: stretch the islands edge to edge and flatten them, then
            // hand over to the real full-width taskbar when the tween ends.
            tb.fillWhenSettled = true;
            Retarget(tb, false);
        } else {
            tb.state = kFilled;
            tb.fillWhenSettled = false;
            Retarget(tb, true);
        }
    } else {
        const bool snap = !canAnimate || tb.state == kHidden;
        if (!snap && tb.state == kFilled) {
            // Morph in: start from the stretched, flat islands.
            tb.shown = Expanded(islands, fillSpan);
            tb.style = kFlat;
            tb.tweening = false;
        }
        tb.state = kShapes;
        tb.fillWhenSettled = false;
        tb.target = islands;
        tb.targetStyle = style;
        Retarget(tb, snap);
    }
    tb.logical = state;
    Present(tb, force);

    if (state == kShapes && (logicalChanged || !SameSpans(previousTarget, tb.target))) {
        log::Write(L"%s: app=%s (%d) extras=%zu tray=%s dpi=%u%s", label.c_str(), FormatRect(is.app).c_str(), is.appCount,
                   is.extras.size(), trayShown ? FormatRect(is.tray).c_str() : L"-", dpi, tb.settling ? L" (settling)" : L"");
    } else if (logicalChanged) {
        log::Write(L"%s: %s", label.c_str(), tb.status.c_str());
    }
}

void Engine::Retarget(Taskbar& tb, bool snap) {
    if (snap) {
        tb.shown = tb.target;
        tb.style = tb.targetStyle;
        tb.tweening = false;
        return;
    }
    // Already on the way there (or there)? Then keep going undisturbed.
    std::vector<Span> heading;
    for (const Span& s : tb.to) {
        if (FindSpan(tb.target, s.id)) heading.push_back(s);
    }
    if (tb.tweening && SameSpans(heading, tb.target) && SameStyle(tb.toStyle, tb.targetStyle)) return;
    if (!tb.tweening && SameSpans(tb.shown, tb.target) && SameStyle(tb.style, tb.targetStyle)) return;

    // New tween from exactly what is on screen now. Islands that appear grow out
    // of their centre; islands that go away shrink into theirs.
    tb.from = tb.shown;
    tb.fromStyle = tb.style;
    tb.to = tb.target;
    tb.toStyle = tb.targetStyle;
    for (const Span& t : tb.target) {
        if (FindSpan(tb.from, t.id)) continue;
        const LONG c = (t.rect.left + t.rect.right) / 2;
        tb.from.push_back({{c, t.rect.top, c, t.rect.bottom}, t.id});
    }
    for (const Span& f : tb.from) {
        if (FindSpan(tb.to, f.id)) continue;
        const LONG c = (f.rect.left + f.rect.right) / 2;
        tb.to.push_back({{c, f.rect.top, c, f.rect.bottom}, f.id});
    }

    // Constant speed: the duration follows the longest edge move.
    LONG distance = 0;
    for (const Span& f : tb.from) {
        const Span* t = FindSpan(tb.to, f.id);
        distance = std::max({distance, std::abs(t->rect.left - f.rect.left), std::abs(t->rect.right - f.rect.right)});
    }
    distance = std::max({distance, static_cast<LONG>(std::abs(tb.toStyle.marginTop - tb.fromStyle.marginTop)),
                         static_cast<LONG>(std::abs(tb.toStyle.marginBottom - tb.fromStyle.marginBottom)),
                         static_cast<LONG>(std::abs(tb.toStyle.radius - tb.fromStyle.radius))});
    tb.tweenDuration = std::clamp(distance * kTweenMsPerPixel, kMinTweenMs, kMaxTweenMs) * 100.0 / config_.animationSpeed;
    tb.tweenStart = NowMs();
    tb.tweening = true;
}

void Engine::Present(Taskbar& tb, bool force) {
    SyncLayers(tb);

    Key key = {tb.state, tb.wr.left, tb.wr.top, tb.wr.right, tb.wr.bottom, tb.style.marginTop, tb.style.marginBottom, tb.style.radius,
               tb.trimShowDesktop ? 1 : 0, tb.fillSpan.left, tb.fillSpan.right};
    if (tb.state == kShapes) {
        for (const Span& s : tb.shown) key.insert(key.end(), {s.id, s.rect.left, s.rect.right});
    }

    // Something else removed our region while the state stayed the same.
    const bool regionLost = tb.state != kFilled && tb.applied && (*tb.applied)[0] == tb.state && !HasRegion(tb.hwnd);
    if (!force && tb.applied == key && !regionLost) return;

    bool ok = true;
    switch (tb.state) {
        case kFilled:
            if (tb.trimShowDesktop) ok = ApplyFullExcept(tb.hwnd, tb.wr, tb.showDesktop);
            else ClearRegion(tb.hwnd);
            break;
        case kHidden: ok = HideTaskbar(tb.hwnd); break;
        default: {
            std::vector<RECT> rects;
            for (const Span& s : tb.shown) {
                if (s.rect.right > s.rect.left) rects.push_back(s.rect);
            }
            ok = ApplySpans(tb.hwnd, tb.wr, rects, tb.style);
            break;
        }
    }
    if (!ok) {
        log::Write(L"%s: SetWindowRgn failed (%lu)", Label(tb.hwnd, tb.primary).c_str(), GetLastError());
        tb.status = L"SetWindowRgn failed";
        return;
    }
    if (regionLost) log::Write(L"%s: region was reset by something else, reapplied", Label(tb.hwnd, tb.primary).c_str());
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
    int radius = tb.style.radius;
    if (tb.state == kFilled) {
        shapes.push_back({tb.fillSpan.left - tb.wr.left, 0, tb.fillSpan.right - tb.wr.left, tb.wr.bottom - tb.wr.top});
        radius = 0;
    } else if (tb.state == kShapes) {
        for (const Span& s : tb.shown) {
            if (s.rect.right > s.rect.left) shapes.push_back(SpanToWindowRect(tb.wr, s.rect, tb.style));
        }
    }

    auto sync = [&](std::unique_ptr<Backdrop>& layer, bool wanted, Backdrop::Layer kind) {
        if (!wanted) {
            layer.reset();
            return;
        }
        if (!layer) layer = std::make_unique<Backdrop>(kind);
        if (shapes.empty()) layer->Hide();
        else layer->Show(tb.hwnd, tb.wr, shapes, radius, config_, tb.dpi);
    };
    sync(tb.fill, config_.background != Background::Default && FillVisible(), Backdrop::Layer::Fill);
    // The border stays on at full width too, so the morph ends without a pop.
    sync(tb.border, BorderActive(), Backdrop::Layer::Border);
}

bool Engine::Animating() const {
    for (const Taskbar& tb : taskbars_) {
        if (tb.tweening) return true;
    }
    return false;
}

void Engine::Animate() {
    const double now = NowMs();
    for (Taskbar& tb : taskbars_) {
        if (!tb.tweening) continue;
        const double p = std::clamp((now - tb.tweenStart) / tb.tweenDuration, 0.0, 1.0);
        if (p >= 1.0) {
            tb.tweening = false;
            tb.shown = tb.target;
            tb.style = tb.targetStyle;
            // Stretched edge to edge and flat: now it is the full-width taskbar.
            if (tb.fillWhenSettled) {
                tb.state = kFilled;
                tb.fillWhenSettled = false;
            }
        } else {
            tb.shown.clear();
            for (const Span& f : tb.from) {
                const Span* t = FindSpan(tb.to, f.id);
                RECT r = f.rect;
                r.left = Lerp(f.rect.left, t->rect.left, p);
                r.right = Lerp(f.rect.right, t->rect.right, p);
                tb.shown.push_back({r, f.id});
            }
            tb.style.marginTop = Lerp(tb.fromStyle.marginTop, tb.toStyle.marginTop, p);
            tb.style.marginBottom = Lerp(tb.fromStyle.marginBottom, tb.toStyle.marginBottom, p);
            tb.style.radius = Lerp(tb.fromStyle.radius, tb.toStyle.radius, p);
        }
        Present(tb, false);
    }
}

void Engine::ClearAll() {
    for (Taskbar& tb : taskbars_) {
        if (IsWindow(tb.hwnd)) ClearRegion(tb.hwnd);
        tb.applied.reset();
        tb.fill.reset();
        tb.border.reset();
        tb.shown.clear();
        tb.target.clear();
        tb.lastRead.reset();
        tb.logical = -1;
        tb.state = kShapes;
        tb.fillWhenSettled = tb.tweening = tb.settling = false;
    }
    ClearAllTaskbars();
}

Engine::Taskbar* Engine::Find(HWND hwnd) {
    for (Taskbar& tb : taskbars_) {
        if (tb.hwnd == hwnd) return &tb;
    }
    return nullptr;
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
