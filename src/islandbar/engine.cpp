#include "islandbar/engine.h"

#include <dwmapi.h>

#include <algorithm>

#include "common/uia_util.h"
#include "core/log.h"
#include "core/match_rules.h"
#include "core/region.h"

namespace ib {
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

std::wstring Span(const RECT& r) {
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

}  // namespace

Engine* Engine::s_instance = nullptr;

Engine::~Engine() {
    Detach();
    RemoveGlobalHooks();
}

HRESULT Engine::Init(Callbacks callbacks) {
    s_instance = this;
    callbacks_ = std::move(callbacks);
    return reader_.Init();
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
        taskbars_.push_back(tb);
    }

    explorerPid_ = 0;
    if (!hwnds.empty()) GetWindowThreadProcessId(hwnds.front(), &explorerPid_);
    if (explorerPid_) {
        const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        // EVENT_OBJECT_CREATE..EVENT_OBJECT_HIDE covers CREATE, DESTROY, SHOW and HIDE.
        if (HWINEVENTHOOK h = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr, ExplorerEventProc, explorerPid_, 0, flags))
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
    return config_.enabled && (config_.fillOnMaximise || config_.fillOnTaskSwitch || config_.autoHide ||
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
            if (event == EVENT_OBJECT_LOCATIONCHANGE && !self->config_.fillOnMaximise) return;
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
        DWORD pid = 0;
        GetWindowThreadProcessId(root, &pid);
        ctx.taskSwitch = switching_ || rules::InList(cls, rules::kTaskSwitcherClasses);
        if (IsTaskbar(root)) ctx.shellUi = true;
        else if (pid == explorerPid_) ctx.shellUi = !rules::InList(cls, rules::kExplorerNonShellClasses);
        else if (config_.autoHide || config_.trayMode == TrayMode::Hover) ctx.shellUi = rules::InList(ProcessName(pid), rules::kShellProcesses);
    }
    if (config_.fillOnMaximise) EnumWindows(CollectMaximised, reinterpret_cast<LPARAM>(&ctx.maximised));
    return ctx;
}

bool Engine::Update(bool force, bool readBounds) {
    if (!config_.enabled) return false;
    const Context ctx = BuildContext();
    bool retry = false;
    for (Taskbar& tb : taskbars_) retry |= UpdateOne(tb, force, readBounds, ctx);
    return retry;
}

bool Engine::UpdateOne(Taskbar& tb, bool force, bool readBounds, const Context& ctx) {
    const std::wstring label = Label(tb.hwnd, tb.primary);
    if (!IsWindow(tb.hwnd)) {
        tb.status = L"window gone";
        return false;
    }

    if (readBounds || !tb.islands) {
        const BoundsResult result = reader_.Compute(tb.hwnd);
        if (!result.islands) {
            ++tb.failures;
            if (tb.lastError != result.error) {
                log::Write(L"%s: bounds unavailable: %s", label.c_str(), result.error.c_str());
                tb.lastError = result.error;
            }
            if (tb.applied && tb.failures < kFailuresBeforeUnclip) return true;
            tb.islands.reset();
            if (tb.applied || force) {
                ClearRegion(tb.hwnd);
                if (tb.applied) log::Write(L"%s: falling back to unclipped taskbar", label.c_str());
                tb.applied.reset();
            }
            tb.status = L"unclipped – " + result.error;
            return false;
        }
        tb.failures = 0;
        tb.lastError.clear();
        tb.islands = *result.islands;
    }
    const Islands& is = *tb.islands;

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
    State state = kShapes;
    std::wstring why;
    if (config_.autoHide && !revealed) {
        state = kHidden;
    } else if (config_.fillOnTaskSwitch && ctx.taskSwitch) {
        state = kFilled;
        why = L"task switcher open";
    } else if (config_.fillOnMaximise && std::find(ctx.maximised.begin(), ctx.maximised.end(), monitor) != ctx.maximised.end()) {
        state = kFilled;
        why = L"window maximised";
    }

    std::vector<RECT> spans;
    bool trayShown = false;
    if (state == kShapes) {
        if (config_.mode == LayoutMode::Bar) {
            spans.push_back({wr.left + padding, wr.top, wr.right - padding, wr.bottom});
        } else {
            spans.push_back(Padded(is.app, padding));
            if (config_.showWidgets) {
                for (const RECT& extra : is.extras) spans.push_back(Padded(extra, padding));
            }
            trayShown = is.hasTray && (config_.trayMode == TrayMode::Show || (config_.trayMode == TrayMode::Hover && revealed));
            if (trayShown) spans.push_back(Padded(is.tray, padding));
        }
    }

    Key key = {state, wr.left, wr.top, wr.right, wr.bottom, style.marginTop, style.marginBottom, style.radius};
    for (const RECT& s : spans) {
        key.push_back(s.left);
        key.push_back(s.right);
    }

    switch (state) {
        case kHidden: tb.status = L"hidden (auto-hide)"; break;
        case kFilled: tb.status = L"full width – " + why; break;
        default:
            if (config_.mode == LayoutMode::Bar) {
                tb.status = L"single bar";
            } else {
                tb.status = std::to_wstring(is.appCount) + L" app buttons [" + Span(is.app) + L"]";
                if (!is.extras.empty()) tb.status += L", +" + std::to_wstring(is.extras.size()) + (config_.showWidgets ? L" extra" : L" extra hidden");
                if (is.hasTray) tb.status += L", tray [" + Span(is.tray) + L"]" + (trayShown ? L"" : L" hidden");
            }
    }

    const bool regionLost = state != kFilled && tb.applied && !HasRegion(tb.hwnd);
    if (!force && tb.applied == key && !regionLost) return false;

    bool ok = true;
    switch (state) {
        case kFilled: ClearRegion(tb.hwnd); break;
        case kHidden: ok = HideTaskbar(tb.hwnd); break;
        default: ok = ApplySpans(tb.hwnd, wr, spans, style); break;
    }
    if (!ok) {
        log::Write(L"%s: SetWindowRgn failed (%lu)", label.c_str(), GetLastError());
        tb.status = L"SetWindowRgn failed";
        return false;
    }

    const bool stateChanged = !tb.applied || (*tb.applied)[0] != state;
    tb.applied = key;
    if (state == kShapes) {
        log::Write(L"%s: app=%s (%d) extras=%zu tray=%s dpi=%u%s", label.c_str(), FormatRect(is.app).c_str(), is.appCount,
                   is.extras.size(), trayShown ? FormatRect(is.tray).c_str() : L"-", dpi, regionLost ? L" [region was reset]" : L"");
    } else if (stateChanged) {
        log::Write(L"%s: %s", label.c_str(), tb.status.c_str());
    }
    return false;
}

void Engine::ClearAll() {
    for (Taskbar& tb : taskbars_) {
        if (IsWindow(tb.hwnd)) ClearRegion(tb.hwnd);
        tb.applied.reset();
    }
    ClearAllTaskbars();
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
    return s;
}

}  // namespace ib
