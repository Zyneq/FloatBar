// floatbar-probe suite: end-to-end tests of FloatBar, measured on screen.
//
// Runs an isolated FloatBar (its own --config-dir, so real settings are never
// touched) through one scenario after another on the primary monitor, and
// checks every frame the compositor presents:
//   * no island edge ever cuts an icon,
//   * no frame flashes the unclipped taskbar,
//   * edges move one way only and without jumps, morphs move every frame,
//   * the islands settle exactly where UI Automation says the buttons are,
//   * quitting FloatBar gives every taskbar back unclipped.
// A pure green window behind the taskbar makes whatever the clip removed
// unambiguous in the capture.
//
// Results go to --out: report.txt, and per scenario frames.csv (one line per
// presented frame), events.csv (the actions), and FloatBar's own config.ini and
// verbose log.txt, whose "frame t=" lines use the same clock as the CSV files.
//
// A FloatBar that is already running is closed first and started again, with
// the same command line, at the end. Interactive scenarios move the mouse and
// take the focus for a moment; --no-interactive skips them.

#include "common.h"

#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include "bounds.h"
#include "config.h"
#include "region.h"
#include "uia_util.h"

namespace probe {
namespace {

// ------------------------------------------------------------------ tuning

constexpr int kBackdropTolerance = 100;  // colour distance still counted as the green backdrop
constexpr int kRingTolerance = 150;      // distance from the test border colour that counts as border
constexpr int kEdgeProbe = 2;            // an icon within this many columns of an island edge is cut
constexpr int kFlashColumns = 80;        // a one-frame bulge of this many visible columns is a flash
constexpr BYTE kTestBorderBgra[4] = {255, 0, 255, 255};
constexpr int kStartGapLogicalPx = 8;    // as in engine.cpp

using fb::Utf8;

std::string Format(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

std::string RunsText(const std::vector<Interval>& runs) {
    std::string s;
    for (const Interval& r : runs) s += (s.empty() ? "" : " ") + std::to_string(r.l) + "-" + std::to_string(r.r);
    return s.empty() ? "(none)" : s;
}

bool RunsMatch(const std::vector<Interval>& want, const std::vector<Interval>& got, int tolerance) {
    return std::ranges::equal(want, got, [&](const Interval& a, const Interval& b) {
        return std::abs(a.l - b.l) <= tolerance && std::abs(a.r - b.r) <= tolerance;
    });
}

// ------------------------------------------------------------------ frames

struct Frame {
    double ms = 0;             // present time (NowMs clock)
    UINT accumulated = 1;      // presents merged into this frame
    std::vector<Interval> visible;  // taskbar pixels on the centre row: everything that isn't backdrop
    std::vector<Interval> ring;     // test-border pixels on the centre row
    int visibleCols = 0;
    int iconL = -1, iconR = -1;     // app-side icon pixels (left of the tray)
    int cutAt = -1;                 // screen x of an island edge that cuts an icon
    int top = -1, bottom = -1;      // rows of the first island at its centre column (taskbar-relative)
    RowRegion region;               // FloatBar's region, read right after the frame arrived
};

// Captures the primary taskbar on its own thread and reduces every frame to a Frame.
class Recorder {
public:
    struct Setup {
        HWND taskbar = nullptr;
        RECT wr{};
        int sideSplit = 0;  // screen x: islands starting left of it are the app side
        BYTE bg[4] = {};    // taskbar background
        bool border = false;
    };

    bool Start(const Setup& setup) {
        setup_ = setup;
        frames_.clear();
        lastChange_ = 0;
        stop_ = false;
        failed_ = false;
        ready_ = false;
        thread_ = std::thread([this] { Loop(); });
        while (!ready_) Sleep(5);
        return !failed_;
    }

    void Stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    std::vector<Frame> Between(double from, double to) const {
        std::lock_guard lock(mutex_);
        std::vector<Frame> out;
        for (const Frame& f : frames_) {
            if (f.ms >= from && f.ms <= to) out.push_back(f);
        }
        return out;
    }
    std::vector<Frame> All() const {
        std::lock_guard lock(mutex_);
        return frames_;
    }
    std::optional<Frame> Latest() const {
        std::lock_guard lock(mutex_);
        if (frames_.empty()) return std::nullopt;
        return frames_.back();
    }
    // When the islands on screen last changed.
    double LastChange() const {
        std::lock_guard lock(mutex_);
        return lastChange_;
    }
private:
    void Loop() {
        const RECT box = setup_.wr;
        HMONITOR monitor = MonitorFromWindow(setup_.taskbar, MONITOR_DEFAULTTOPRIMARY);
        auto capture = std::make_unique<Capture>();
        failed_ = !capture->Init(monitor);
        ready_ = true;
        std::vector<BYTE> px;
        while (!stop_ && !failed_) {
            LONGLONG present = 0;
            UINT accumulated = 0;
            if (!capture->Next(20, box, px, present, accumulated)) {
                if (capture->failed()) {
                    // Access lost (display mode change, secure desktop): start over.
                    capture = std::make_unique<Capture>();
                    if (!capture->Init(monitor)) {
                        Sleep(100);
                        failed_ = !capture->Init(monitor);
                    }
                }
                continue;
            }
            Frame f = Analyze(px, QpcToMs(present), accumulated);
            std::lock_guard lock(mutex_);
            if (frames_.empty() || frames_.back().visible != f.visible) lastChange_ = f.ms;
            frames_.push_back(std::move(f));
        }
    }

    Frame Analyze(const std::vector<BYTE>& px, double ms, UINT accumulated) const {
        const RECT& wr = setup_.wr;
        const int w = wr.right - wr.left, h = wr.bottom - wr.top, row = h / 2;
        auto at = [&](int x, int y) { return &px[(static_cast<size_t>(y) * w + x) * 4]; };
        auto backdrop = [&](const BYTE* p) { return Dist(p, kBackdropBgra) < kBackdropTolerance; };
        Frame f;
        f.ms = ms;
        f.accumulated = std::max(1u, accumulated);

        std::vector<char> visible(w), ring(w), nearRing(w), icon(w);
        for (int x = 0; x < w; ++x) {
            const BYTE* p = at(x, row);
            visible[x] = !backdrop(p);
            ring[x] = setup_.border && Dist(p, kTestBorderBgra) < kRingTolerance;
            f.visibleCols += visible[x];
        }
        auto runs = [&](const std::vector<char>& mask) {
            std::vector<Interval> out;
            for (int x = 0; x < w; ++x) {
                if (!mask[x]) continue;
                if (!out.empty() && out.back().r == wr.left + x) out.back().r++;
                else out.push_back({wr.left + x, wr.left + x + 1});
            }
            return out;
        };
        f.visible = runs(visible);
        f.ring = runs(ring);
        // The border and its antialiased fringe are not icons.
        for (const Interval& r : f.ring) {
            const int l = r.l - static_cast<int>(wr.left), rr = r.r - static_cast<int>(wr.left);
            for (int x = std::max(0, l - 3); x < std::min(w, rr + 3); ++x) nearRing[x] = 1;
        }
        for (int x = 0; x < w; ++x) {
            if (!visible[x] || nearRing[x]) continue;
            for (int y = std::max(0, row - kIconBandHalf); y <= std::min(h - 1, row + kIconBandHalf) && !icon[x]; ++y) {
                const BYTE* p = at(x, y);
                icon[x] = !backdrop(p) && Dist(p, setup_.bg) > kIconTolerance;
            }
            if (icon[x] && wr.left + x < setup_.sideSplit) {
                if (f.iconL < 0) f.iconL = wr.left + x;
                f.iconR = wr.left + x + 1;
            }
        }
        f.region = ReadRegionRow(setup_.taskbar, wr, wr.top + row);
        // An icon right at an island edge is being cut by it - if that edge is
        // FloatBar's clip. Explorer sometimes leaves part of the taskbar
        // transparent for a frame while it inserts a button; that edge is not
        // on the region, and not FloatBar's doing.
        auto onRegionEdge = [&](int x) {
            if (!f.region.clipped) return false;
            for (const Interval& g : f.region.runs) {
                if (std::abs(g.l - x) <= 4 || std::abs(g.r - x) <= 4) return true;
            }
            return false;
        };
        for (const Interval& r : f.visible) {
            if (r.r - r.l < 3 * kEdgeProbe || f.cutAt >= 0) continue;
            for (int i = 0; i < kEdgeProbe && f.cutAt < 0; ++i) {
                if (icon[r.l - wr.left + i] && onRegionEdge(r.l)) f.cutAt = r.l;
                else if (icon[r.r - wr.left - 1 - i] && onRegionEdge(r.r)) f.cutAt = r.r;
            }
        }
        // Vertical extent of the first island.
        if (!f.visible.empty()) {
            const int cx = (f.visible.front().l + f.visible.front().r) / 2 - wr.left;
            for (int y = 0; y < h; ++y) {
                if (backdrop(at(cx, y))) continue;
                if (f.top < 0) f.top = y;
                f.bottom = y + 1;
            }
        }
        return f;
    }

    Setup setup_;
    std::thread thread_;
    std::atomic<bool> stop_ = false, failed_ = false, ready_ = false;
    mutable std::mutex mutex_;
    std::vector<Frame> frames_;
    double lastChange_ = 0;
};

// A pure green window behind the taskbar (never activated, never topmost): where
// the clip removes the taskbar, the capture shows exactly this colour.
class Backdrop {
public:
    bool Show(const RECT& r) {
        thread_ = std::thread([this, r] {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"FloatBarProbeBackdrop";
            wc.hbrBackground = CreateSolidBrush(RGB(kBackdropBgra[2], kBackdropBgra[1], kBackdropBgra[0]));
            RegisterClassW(&wc);
            threadId_ = GetCurrentThreadId();
            hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, r.left, r.top, r.right - r.left,
                                    r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
            if (hwnd_) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            ready_ = true;
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
            if (hwnd_) DestroyWindow(hwnd_);
        });
        while (!ready_) Sleep(5);
        return hwnd_ != nullptr;
    }
    void Hide() {
        if (!thread_.joinable()) return;
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        thread_.join();
    }

private:
    std::thread thread_;
    std::atomic<bool> ready_ = false;
    DWORD threadId_ = 0;
    HWND hwnd_ = nullptr;
};

// ------------------------------------------------------------------ processes

struct Process {
    HANDLE handle = nullptr;
    DWORD pid = 0;
};

Process Launch(std::wstring cmd) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return {};
    CloseHandle(pi.hThread);
    return {pi.hProcess, pi.dwProcessId};
}

HWND WindowOf(const wchar_t* cls, DWORD pid) {
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, cls, nullptr)) != nullptr) {
        DWORD owner = 0;
        GetWindowThreadProcessId(h, &owner);
        if (!pid || owner == pid) return h;
    }
    return nullptr;
}

HWND WaitWindow(const wchar_t* cls, DWORD pid, int timeoutMs) {
    for (int waited = 0; waited <= timeoutMs; waited += 20) {
        if (HWND h = WindowOf(cls, pid)) return h;
        Sleep(20);
    }
    return nullptr;
}

std::wstring CommandLineOf(DWORD pid) {
    using Query = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    std::wstring out;
    if (process && query) {
        constexpr auto kCommandLine = static_cast<PROCESSINFOCLASS>(60);  // ProcessCommandLineInformation
        ULONG size = 0;
        query(process, kCommandLine, nullptr, 0, &size);
        std::vector<BYTE> buf(size + sizeof(UNICODE_STRING));
        if (size && query(process, kCommandLine, buf.data(), static_cast<ULONG>(buf.size()), &size) >= 0) {
            const auto* s = reinterpret_cast<const UNICODE_STRING*>(buf.data());
            out.assign(s->Buffer, s->Length / sizeof(wchar_t));
        }
    }
    if (process) CloseHandle(process);
    return out;
}

bool AnyTaskbarClipped() {
    for (HWND h : fb::FindTaskbars()) {
        if (fb::HasRegion(h)) return true;
    }
    return false;
}

// ------------------------------------------------------------------ suite

struct Options {
    std::wstring floatbar;  // floatbar.exe
    std::wstring out;       // results folder
    std::vector<std::string> only;
    bool interactive = true;
    bool floatbarLog = true;  // FloatBar's verbose log, one line per drawn frame (--no-floatbar-log: as users run it)
};

class Suite;

struct Scenario {
    const char* name;
    const char* what;
    bool interactive;  // moves the mouse or takes the focus
    bool autostart;    // start FloatBar (and wait for it to settle) before the body
    void (*config)(fb::Config&);
    void (*body)(Suite&);
    bool manual = false;  // a diagnostic: runs only when named with --only
};

struct Check {
    std::string text;
    bool ok;
    bool note;
    bool warn = false;  // a known Windows limitation showed up: reported, not a failure
};

struct MotionOptions {
    bool monotonic = true;  // every edge moves one way only
    bool smooth = true;     // no single step far above the typical step
    double lagMs = 350;     // islands may settle at most this long after the icons (0: don't check)
    bool cutsWarn = false;  // cuts are a known limitation here: warn instead of failing
    bool checkCuts = true;  // false: an island appearing or disappearing, which reveals or hides icons on purpose
};

class Suite {
public:
    explicit Suite(Options options) : options_(std::move(options)) {}

    int Run(const std::vector<Scenario>& scenarios);

    // ---- building blocks for scenarios

    double Mark(const std::string& what) {
        const double now = NowMs();
        events_.push_back({now, what});
        std::printf("   . %s\n", what.c_str());
        return now;
    }

    void Expect(const std::string& what, bool ok, const std::string& detail = {}) {
        const std::string text = what + (detail.empty() ? "" : " (" + detail + ")");
        checks_.push_back({text, ok, false});
        std::printf("   %s %s\n", ok ? "ok  " : "FAIL", text.c_str());
    }

    void Note(const std::string& text) {
        checks_.push_back({text, true, true});
        std::printf("        %s\n", text.c_str());
    }

    void Warn(const std::string& text) {
        checks_.push_back({text, true, false, true});
        std::printf("   warn %s\n", text.c_str());
    }

    // Waits until the islands on screen have not changed for `quietMs` (and at
    // least `minMs` passed since `from`); returns the time it stopped waiting.
    double Settle(double from, double quietMs = 500, double minMs = 800, double maxMs = 8000) {
        for (;;) {
            Sleep(20);
            const double now = NowMs();
            const double last = std::max(from, recorder_.LastChange());
            if ((now - last >= quietMs && now - from >= minMs) || now - from >= maxMs) {
                if (now - from >= maxMs) Note(Format("still changing after %.0f ms", maxMs));
                return now;
            }
        }
    }

    // Polls the latest frame until `pred` holds; returns the time it did, or -1.
    template <typename Pred>
    double WaitFor(Pred pred, double timeoutMs) {
        const double start = NowMs();
        while (NowMs() - start < timeoutMs) {
            if (const auto f = recorder_.Latest(); f && pred(*f)) return f->ms;
            Sleep(10);
        }
        return -1;
    }

    HWND Open() {
        HWND h = SpawnDummy(nextDummy_++);
        if (h) windows_.push_back(h);
        else Expect("a test window opens", false);
        return h;
    }
    void Close(size_t index) {
        if (index >= windows_.size()) return;
        PostMessageW(windows_[index], WM_CLOSE, 0, 0);
        windows_.erase(windows_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    void CloseAll(int spacingMs) {
        while (!windows_.empty()) {
            Close(0);
            if (!windows_.empty()) Sleep(spacingMs);
        }
    }
    HWND Window(size_t index) const { return index < windows_.size() ? windows_[index] : nullptr; }

    bool Start(bool background = true) {
        const double t = Mark(background ? "start FloatBar" : "start FloatBar with its Settings window");
        std::wstring cmd = L"\"" + options_.floatbar + L"\"" + (background ? L" --background" : L"") + L" --config-dir \"" + dir_ + L"\"";
        floatbar_ = Launch(cmd);
        main_ = floatbar_.handle ? WaitWindow(L"FloatBarMain", floatbar_.pid, 5000) : nullptr;
        Expect("FloatBar starts", main_ != nullptr);
        if (!main_) return false;
        Settle(t, 400, 600, 5000);
        return true;
    }

    // Quits FloatBar the way its menu does and checks it gave the taskbars back.
    void Stop() {
        if (!floatbar_.handle) return;
        Mark("quit FloatBar");
        if (main_) PostMessageW(main_, WM_CLOSE, 0, 0);
        const bool exited = WaitForSingleObject(floatbar_.handle, 5000) == WAIT_OBJECT_0;
        if (!exited) TerminateProcess(floatbar_.handle, 1);
        Expect("FloatBar exits when asked", exited);
        Expect("every taskbar is unclipped again after exit", !AnyTaskbarClipped());
        Forget();
    }
    // FloatBar went away by itself (Exit button, kill): drop the handles.
    void Forget() {
        if (floatbar_.handle) CloseHandle(floatbar_.handle);
        floatbar_ = {};
        main_ = nullptr;
    }
    const Process& FloatBar() const { return floatbar_; }
    const std::wstring& FloatBarExe() const { return options_.floatbar; }
    const std::wstring& Dir() const { return dir_; }

    fb::Config& Config() { return config_; }
    const RECT& TaskbarRect() const { return wr_; }
    int RowY() const { return (wr_.top + wr_.bottom) / 2; }
    HWND Taskbar() const { return taskbar_; }
    int Scale(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }

    std::optional<Frame> Latest() const { return recorder_.Latest(); }
    std::vector<Frame> Between(double from, double to) const { return recorder_.Between(from, to); }

    std::optional<fb::Islands> ReadIslands() {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const fb::BoundsResult r = reader_.Compute(taskbar_);
            if (r.islands) return r.islands;
            Sleep(100);
        }
        return std::nullopt;
    }

    // Waits until explorer has stopped moving buttons (the previous scenario's
    // windows can still be sliding out).
    void WaitTaskbarIdle() {
        const double start = NowMs();
        double since = start;
        std::optional<fb::Islands> last;
        while (NowMs() - start < 4000) {
            const auto is = reader_.Compute(taskbar_).islands;
            const bool same = is && last && is->appCount == last->appCount && EqualRect(&is->app, &last->app) && EqualRect(&is->tray, &last->tray);
            if (!same) since = NowMs();
            else if (NowMs() - since >= 400) return;
            last = is;
            Sleep(30);
        }
    }

    // What the centre row should show for `is` with the current config.
    std::vector<Interval> ExpectedRuns(const fb::Islands& is) const;

    void ExpectLayout(const std::string& what) {
        const auto is = ReadIslands();
        const auto f = recorder_.Latest();
        if (!is || !f) {
            Expect(what + ": islands match the buttons", false, !is ? "UI Automation read failed" : "no frame captured");
            return;
        }
        const std::vector<Interval> want = ExpectedRuns(*is);
        Expect(what + ": islands match the buttons", RunsMatch(want, f->visible, 1),
               "expected " + RunsText(want) + ", on screen " + RunsText(f->visible));
    }

    bool FullWidth(const Frame& f) const {
        return f.visible.size() == 1 && f.visible[0].l <= wr_.left + 1 && f.visible[0].r >= wr_.right - 1;
    }

    void CheckMotion(const std::string& what, double from, double to, MotionOptions o = {});
    void CheckMorph(const std::string& what, double from, double to, bool toFull, double expectMs);
    void CheckRing(const std::string& what, double from, double to);
    int RemoveFlashes(std::vector<Frame>& frames, double& first) const;

private:
    struct Event {
        double ms;
        std::string what;
    };

    bool RunScenario(const Scenario& s);
    void WriteFrames(double origin) const;
    int SideSplit() const;
    void CheckFlashes(const std::string& what, std::vector<Frame>& frames, double from);

    Options options_;
    fb::BoundsReader reader_;
    HWND taskbar_ = nullptr;
    RECT wr_{};
    UINT dpi_ = 96;
    BYTE bg_[4] = {};
    fb::Islands initial_;  // the layout before any scenario, for the app/tray split

    Recorder recorder_;
    Backdrop backdrop_;
    Process floatbar_;
    HWND main_ = nullptr;
    std::vector<HWND> windows_;
    int nextDummy_ = 1;
    fb::Config config_;
    std::wstring dir_;
    std::vector<Event> events_;
    std::vector<Check> checks_;
    int warnings_ = 0;
};

std::vector<Interval> Suite::ExpectedRuns(const fb::Islands& is) const {
    const fb::Config& c = config_;
    if (!c.enabled) return {{wr_.left, wr_.right}};
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(taskbar_, MONITOR_DEFAULTTOPRIMARY), &mi);
    const auto own = c.monitorModes.find(mi.szDevice);
    if (own != c.monitorModes.end() && own->second == fb::MonitorMode::Hidden) return {};
    if (own != c.monitorModes.end() && own->second == fb::MonitorMode::Normal) return {{wr_.left, wr_.right}};

    const int pad = Scale(c.islandPadding);
    std::vector<Interval> runs;
    auto add = [&](LONG l, LONG r) {
        l = std::max(l, wr_.left);
        r = std::min(r, wr_.right);
        if (r > l) runs.push_back({static_cast<int>(l), static_cast<int>(r)});
    };
    if (c.mode == fb::LayoutMode::Bar) {
        LONG l = wr_.left + pad, r = wr_.right - pad;
        if (c.hideShowDesktop && is.hasShowDesktop) {
            if (is.showDesktop.left >= (l + r) / 2) r = std::min(r, is.showDesktop.left);
            else l = std::max(l, is.showDesktop.right);
        }
        add(l, r);
        return runs;
    }
    if (c.separateStart && is.hasSplit) {
        const LONG half = Scale(kStartGapLogicalPx) / 2;
        add(is.app.left - pad, is.split - half);
        add(is.split + half, is.app.right + pad);
    } else {
        add(is.app.left - pad, is.app.right + pad);
    }
    if (c.showWidgets) {
        for (const RECT& e : is.extras) add(e.left - pad, e.right + pad);
    }
    if (is.hasTray && c.trayMode == fb::TrayMode::Show) add(is.tray.left - pad, is.tray.right + pad);
    std::sort(runs.begin(), runs.end(), [](const Interval& a, const Interval& b) { return a.l < b.l; });
    std::vector<Interval> merged;
    for (const Interval& r : runs) {
        if (!merged.empty() && r.l <= merged.back().r) merged.back().r = std::max(merged.back().r, r.r);
        else merged.push_back(r);
    }
    return merged;
}

// Island runs starting left of this are the app side, the rest the tray.
int Suite::SideSplit() const {
    return initial_.hasTray ? static_cast<int>(initial_.tray.left) - Scale(fb::kMaxIslandPadding) - 1 : wr_.right;
}

// A flash: for up to kFlashMs, more of the taskbar shows than just before and
// just after (the unclipped taskbar, or part of it, for a few frames). DWM
// occasionally drops a window's region on its own for a frame or two after
// SetWindowRgn (the region is still set); the next region update restores it.
// That is a Windows limitation, so a flash is a warning, and its frames are
// left out of the motion checks. Removes them from `frames`; returns how many.
int Suite::RemoveFlashes(std::vector<Frame>& frames, double& first) const {
    constexpr double kFlashMs = 60;
    int flashes = 0;
    first = -1;
    std::vector<char> flash(frames.size());
    auto mark = [&](size_t from, size_t to) {
        if (!flashes) first = frames[from].ms;
        ++flashes;
        std::fill(flash.begin() + static_cast<std::ptrdiff_t>(from), flash.begin() + static_cast<std::ptrdiff_t>(to), char{1});
    };
    auto covered = [&](const Frame& f) {
        int n = 0;
        for (const Interval& g : f.region.runs) n += std::max(0, std::min(g.r, static_cast<int>(wr_.right)) - std::max(g.l, static_cast<int>(wr_.left)));
        return f.region.clipped ? n : wr_.right - wr_.left;
    };
    // Much more of the taskbar shows than its region covers: DWM ignored it. The
    // region is read just after the frame arrived, so compare with the previous
    // frame's region too (the one on screen while islands move fast).
    auto beyondRegion = [&](size_t k) { return k > 0 && frames[k].visibleCols > std::max(covered(frames[k]), covered(frames[k - 1])) + 40; };
    for (size_t i = 1; i + 1 < frames.size(); ++i) {
        if (beyondRegion(i)) {
            size_t j = i;
            while (j < frames.size() && beyondRegion(j)) ++j;
            mark(i, j);
            i = j;
            continue;
        }
        const int before = frames[i - 1].visibleCols;
        if (frames[i].visibleCols <= before + kFlashColumns) continue;
        int peak = 0;
        size_t j = i;
        for (; j < frames.size() && frames[j].ms - frames[i].ms <= kFlashMs && frames[j].visibleCols > before + kFlashColumns; ++j)
            peak = std::max(peak, frames[j].visibleCols);
        if (j == frames.size() || frames[j].ms - frames[i].ms > kFlashMs || peak <= frames[j].visibleCols + kFlashColumns) continue;
        mark(i, j);
        i = j;
    }
    size_t kept = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!flash[i]) frames[kept++] = frames[i];
    }
    frames.resize(kept);
    return flashes;
}

struct EdgeStats {
    int from = INT_MIN, to = INT_MIN, positions = 0, reversals = 0;
    double first = -1, last = -1, medianStep = 0, maxStep = 0, longestPause = 0, firstReversal = -1;
    double spike = 0, spikeAround = 0;  // the biggest step far above its neighbours, and their average
};

// Edge `which` of a frame: 0 app left, 1 app right, 2 tray left, 3 tray right; INT_MIN if absent.
int EdgeOf(const Frame& f, int which, int split) {
    int appL = INT_MAX, appR = INT_MIN, trayL = INT_MAX, trayR = INT_MIN;
    for (const Interval& r : f.visible) {
        if (r.r - r.l < 4) continue;  // antialiasing specks
        if (r.l < split) appL = std::min(appL, r.l), appR = std::max(appR, r.r);
        else trayL = std::min(trayL, r.l), trayR = std::max(trayR, r.r);
    }
    switch (which) {
        case 0: return appL == INT_MAX ? INT_MIN : appL;
        case 1: return appR;
        case 2: return trayL == INT_MAX ? INT_MIN : trayL;
        default: return trayR;
    }
}

EdgeStats AnalyzeEdge(const std::vector<Frame>& frames, int which, int split) {
    EdgeStats st;
    std::vector<double> steps;
    int prev = INT_MIN, dir = 0;
    for (const Frame& f : frames) {
        const int v = EdgeOf(f, which, split);
        if (v == INT_MIN) continue;
        if (st.from == INT_MIN) st.from = v;
        if (prev != INT_MIN && v != prev) {
            const int d = v - prev;
            steps.push_back(std::abs(d) / static_cast<double>(f.accumulated));
            const int nd = d > 0 ? 1 : -1;
            if (dir && nd != dir) {
                if (!st.reversals) st.firstReversal = f.ms;
                ++st.reversals;
            }
            dir = nd;
            if (st.first < 0) st.first = f.ms;
            else st.longestPause = std::max(st.longestPause, f.ms - st.last);
            st.last = f.ms;
            ++st.positions;
        }
        prev = v;
        st.to = v;
    }
    // A jump is a step far bigger than the steps around it. (Slides ease in and
    // out, so a fast middle naturally takes steps several times the median.)
    for (size_t i = 0; i < steps.size(); ++i) {
        double around = 0;
        int n = 0;
        for (size_t k = i >= 2 ? i - 2 : 0; k <= i + 2 && k < steps.size(); ++k) {
            if (k != i) around += steps[k], ++n;
        }
        if (n && steps[i] > std::max(6.0, 3 * around / n) && steps[i] > st.spike) st.spike = steps[i], st.spikeAround = around / n;
    }
    if (!steps.empty()) {
        std::sort(steps.begin(), steps.end());
        st.medianStep = steps[steps.size() / 2];
        st.maxStep = steps.back();
    }
    return st;
}

void Suite::CheckFlashes(const std::string& what, std::vector<Frame>& frames, double from) {
    double first = -1;
    const int flashes = RemoveFlashes(frames, first);
    if (flashes) Warn(Format("%s: %d flash(es) of the unclipped taskbar, first at +%.0f ms (Windows drops the region for a frame or two)", what.c_str(), flashes, first - from));
}

void Suite::CheckMotion(const std::string& what, double from, double to, MotionOptions o) {
    std::vector<Frame> frames = recorder_.Between(from, to);
    CheckFlashes(what, frames, from);
    int cuts = 0, cutX = -1;
    double firstCut = -1;
    for (const Frame& f : frames) {
        if (f.cutAt < 0) continue;
        if (!cuts) firstCut = f.ms, cutX = f.cutAt;
        ++cuts;
    }
    if (!o.checkCuts) {
        // Nothing to check: the island grows out of its centre or shrinks into it.
    } else if (o.cutsWarn && cuts) {
        Warn(Format("%s: %d frame(s) cut an icon, first at +%.0f ms, x=%d", what.c_str(), cuts, firstCut - from, cutX));
    } else {
        Expect(what + ": no frame cuts an icon", cuts == 0,
               cuts ? Format("%d frame(s), first at +%.0f ms, x=%d", cuts, firstCut - from, cutX) : Format("%zu frames", frames.size()));
    }

    static const char* kNames[] = {"app left", "app right", "tray left", "tray right"};
    const int split = SideSplit();
    std::string reversals, chunks;
    for (int e = 0; e < 4; ++e) {
        const EdgeStats st = AnalyzeEdge(frames, e, split);
        if (!st.positions) continue;
        Note(Format("%s %d -> %d: +%.0f..+%.0f ms, %d positions, step median %.1f / max %.1f px, longest pause %.0f ms%s", kNames[e],
                    st.from, st.to, st.first - from, st.last - from, st.positions, st.medianStep, st.maxStep, st.longestPause,
                    st.reversals ? Format(", %d reversal(s)", st.reversals).c_str() : ""));
        if (st.reversals) reversals += Format("%s%s: %d, first at +%.0f ms", reversals.empty() ? "" : "; ", kNames[e], st.reversals, st.firstReversal - from);
        if (st.spike > 0) chunks += Format("%s%s: %.0f px between steps of %.1f", chunks.empty() ? "" : "; ", kNames[e], st.spike, st.spikeAround);
    }
    if (o.monotonic) Expect(what + ": edges move one way only", reversals.empty(), reversals);
    if (o.smooth) Expect(what + ": no jumps", chunks.empty(), chunks);

    if (o.lagMs > 0) {
        double iconsLast = -1, islandsLast = -1;
        for (size_t i = 1; i < frames.size(); ++i) {
            if (frames[i].iconL != frames[i - 1].iconL || frames[i].iconR != frames[i - 1].iconR) iconsLast = frames[i].ms;
            if (frames[i].visible != frames[i - 1].visible) islandsLast = frames[i].ms;
        }
        if (iconsLast >= 0 && islandsLast >= 0) {
            const double lag = islandsLast - iconsLast;
            Expect(what + ": islands settle with the icons", lag <= o.lagMs,
                   lag > 0 ? Format("%.0f ms after the icons stopped", lag) : Format("%.0f ms before the icons stopped", -lag));
        }
    }
}

void Suite::CheckMorph(const std::string& what, double from, double to, bool toFull, double expectMs) {
    std::vector<Frame> frames = recorder_.Between(from, to);
    CheckFlashes(what, frames, from);
    double first = -1, last = -1, pause = 0, maxStep = 0;
    int prev = -1, changes = 0, reversals = 0;
    const int dir = toFull ? 1 : -1;
    for (const Frame& f : frames) {
        const int v = f.visibleCols;
        if (prev >= 0 && v != prev) {
            if ((v - prev) * dir < 0) ++reversals;
            if (first < 0) first = f.ms;
            else pause = std::max(pause, f.ms - last);
            last = f.ms;
            ++changes;
            maxStep = std::max(maxStep, std::abs(v - prev) / static_cast<double>(f.accumulated));
        }
        prev = v;
    }
    const double duration = last - first;
    Note(Format("visible columns changed in %d frames over %.0f ms, starting +%.0f ms; largest step %.0f px, longest pause %.0f ms", changes,
                duration, first - from, maxStep, pause));
    Expect(what + Format(": takes about %.0f ms", expectMs), first >= 0 && duration >= expectMs * 0.6 - 20 && duration <= expectMs * 1.6 + 40,
           Format("%.0f ms", duration));
    Expect(what + (toFull ? ": only ever grows" : ": only ever shrinks"), reversals == 0, reversals ? Format("%d reversal(s)", reversals) : "");
    Expect(what + ": moves every frame", first >= 0 && pause <= 25, Format("longest pause %.0f ms", pause));
    const auto f = recorder_.Latest();
    if (toFull) {
        Expect(what + ": ends as the plain full-width taskbar", f && FullWidth(*f) && !f->region.clipped,
               f ? "on screen " + RunsText(f->visible) + (f->region.clipped ? ", still clipped" : "") : "no frame");
    } else {
        ExpectLayout(what);
    }
}

// The border is a separate window, so while islands move it can land a frame or
// two before or after the region (no API updates two windows atomically, and the
// region goes through explorer; at 360 Hz a frame lasts 2.8 ms). That is a
// warning; staying off for 4 frames or more is a bug.
void Suite::CheckRing(const std::string& what, double from, double to) {
    int off = 0, slipped = 0, stuck = 0, run = 0;
    double firstStuck = -1;
    std::string sample;
    const std::vector<Frame> frames = recorder_.Between(from, to);
    for (const Frame& f : frames) {
        bool ok = true;
        for (const Interval& v : f.visible) {
            if (v.r - v.l < 10) {
                ok = false;  // a border (or a sliver of taskbar) detached from its island
                continue;
            }
            int rl = INT_MAX, rr = INT_MIN;
            for (const Interval& r : f.ring) {
                if (r.l >= v.l && r.r <= v.r) rl = std::min(rl, r.l), rr = std::max(rr, r.r);
            }
            if (rl == INT_MAX || rl - v.l > 2 || v.r - rr > 2) ok = false;
        }
        run = ok ? 0 : run + 1;
        off += !ok;
        slipped += run == 2;
        if (run == 4) {
            if (!stuck) firstStuck = f.ms, sample = "visible " + RunsText(f.visible) + ", border " + RunsText(f.ring);
            ++stuck;
        }
    }
    if (off) Note(Format("%s: border a frame off its island in %d of %zu frames", what.c_str(), off, frames.size()));
    if (slipped && !stuck) Warn(Format("%s: border 2-3 frames off its island %d time(s) (separate windows)", what.c_str(), slipped));
    Expect(what + ": border never stays off the island edges", stuck == 0,
           stuck ? Format("%d time(s), first at +%.0f ms: ", stuck, firstStuck - from) + sample : Format("%zu frames", frames.size()));
}

void Suite::WriteFrames(double origin) const {
    FILE* file = nullptr;
    if (_wfopen_s(&file, (dir_ + L"\\frames.csv").c_str(), L"w") == 0 && file) {
        std::fprintf(file, "ms,accumulated,visible_cols,visible,border,icon_l,icon_r,cut_at,top,bottom,region_clipped,region\n");
        for (const Frame& f : recorder_.All()) {
            std::fprintf(file, "%.2f,%u,%d,%s,%s,%d,%d,%d,%d,%d,%d,%s\n", f.ms - origin, f.accumulated, f.visibleCols, RunsText(f.visible).c_str(),
                         f.ring.empty() ? "" : RunsText(f.ring).c_str(), f.iconL, f.iconR, f.cutAt, f.top, f.bottom, f.region.clipped ? 1 : 0,
                         RunsText(f.region.runs).c_str());
        }
        std::fclose(file);
    }
    if (_wfopen_s(&file, (dir_ + L"\\events.csv").c_str(), L"w") == 0 && file) {
        // FloatBar's log stamps frames with the absolute clock; subtract `origin` to compare.
        std::fprintf(file, "ms,event\n0.00,origin (absolute %.2f)\n", origin);
        for (const Event& e : events_) std::fprintf(file, "%.2f,%s\n", e.ms - origin, e.what.c_str());
        std::fclose(file);
    }
}

bool Suite::RunScenario(const Scenario& s) {
    std::printf("\n== %s: %s\n", s.name, s.what);
    dir_ = options_.out + L"\\" + std::wstring(s.name, s.name + strlen(s.name));
    CreateDirectoryW(dir_.c_str(), nullptr);
    for (const wchar_t* f : {L"\\config.ini", L"\\log.txt", L"\\log.old.txt"}) DeleteFileW((dir_ + f).c_str());
    config_ = fb::Config{};
    config_.debugLogging = options_.floatbarLog;  // FloatBar logs every drawn frame
    if (s.config) s.config(config_);
    fb::SetConfigDir(dir_);
    fb::SaveConfig(config_);
    events_.clear();
    checks_.clear();
    windows_.clear();
    WaitTaskbarIdle();

    Recorder::Setup setup;
    setup.taskbar = taskbar_;
    setup.wr = wr_;
    setup.sideSplit = SideSplit();
    memcpy(setup.bg, bg_, 4);
    setup.border = config_.borderWidth > 0;
    if (!recorder_.Start(setup)) {
        Expect("screen capture works", false);
        return false;
    }
    const double origin = Mark("scenario start");
    const double started = NowMs();
    if (!s.autostart || Start()) s.body(*this);
    CloseAll(0);
    CloseDummies();
    Stop();
    recorder_.Stop();
    WriteFrames(origin);

    bool pass = true;
    for (const Check& c : checks_) {
        pass &= c.ok;
        warnings_ += c.warn;
    }
    FILE* report = nullptr;
    if (_wfopen_s(&report, (options_.out + L"\\report.txt").c_str(), L"a") == 0 && report) {
        std::fprintf(report, "\n[%s] %s: %s (%.1f s)\n", pass ? "PASS" : "FAIL", s.name, s.what, (NowMs() - started) / 1000);
        for (const Check& c : checks_) std::fprintf(report, "    %s %s\n", c.note ? "    " : c.warn ? "warn" : c.ok ? "ok  " : "FAIL", c.text.c_str());
        std::fclose(report);
    }
    std::printf("   => %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int Suite::Run(const std::vector<Scenario>& scenarios) {
    if (GetFileAttributesW(options_.floatbar.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("floatbar.exe not found: %s\n", Utf8(options_.floatbar).c_str());
        return 2;
    }
    CreateDirectoryW(options_.out.c_str(), nullptr);
    DeleteFileW((options_.out + L"\\report.txt").c_str());
    if (FAILED(reader_.Init())) {
        std::printf("UI Automation unavailable\n");
        return 2;
    }

    // Close a running FloatBar (its settings stay as they are) and remember how it was started.
    std::wstring restore;
    if (HWND running = WindowOf(L"FloatBarMain", 0)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(running, &pid);
        restore = CommandLineOf(pid);
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(running, WM_CLOSE, 0, 0);
        if (process) {
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
        }
        std::printf("closed the running FloatBar; it is started again at the end:\n  %s\n", Utf8(restore).c_str());
    }
    CloseDummies();

    taskbar_ = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar_ || fb::HasRegion(taskbar_)) fb::ClearAllTaskbars();
    GetWindowRect(taskbar_, &wr_);
    dpi_ = GetDpiForWindow(taskbar_) ? GetDpiForWindow(taskbar_) : 96;
    backdrop_.Show(wr_);
    Sleep(300);

    // The taskbar background, from the unclipped taskbar: the commonest colour on its centre row.
    {
        Capture capture;
        std::vector<BYTE> px;
        LONGLONG present;
        UINT accumulated;
        bool got = capture.Init(MonitorFromWindow(taskbar_, MONITOR_DEFAULTTOPRIMARY));
        // Desktop duplication delivers the whole screen first, then changes only.
        for (int i = 0; got && i < 50; ++i) {
            if (capture.Next(100, wr_, px, present, accumulated)) break;
        }
        got = got && !px.empty();
        if (!got) {
            std::printf("screen capture unavailable\n");
            backdrop_.Hide();
            return 2;
        }
        const int w = wr_.right - wr_.left, row = (wr_.bottom - wr_.top) / 2;
        std::vector<int> counts(1 << 15);
        for (int x = 0; x < w; ++x) {
            const BYTE* p = &px[(static_cast<size_t>(row) * w + x) * 4];
            ++counts[(p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10)];
        }
        const int bin = static_cast<int>(std::max_element(counts.begin(), counts.end()) - counts.begin());
        bg_[0] = static_cast<BYTE>(((bin & 31) << 3) + 4);
        bg_[1] = static_cast<BYTE>((((bin >> 5) & 31) << 3) + 4);
        bg_[2] = static_cast<BYTE>((((bin >> 10) & 31) << 3) + 4);
    }
    const auto initial = ReadIslands();
    if (!initial) {
        std::printf("the taskbar's buttons can't be read through UI Automation\n");
        backdrop_.Hide();
        return 2;
    }
    initial_ = *initial;
    std::printf("taskbar %ld,%ld-%ld,%ld at %u dpi; background %u,%u,%u; app buttons %ld-%ld (%d), tray %ld-%ld\n", wr_.left, wr_.top, wr_.right,
                wr_.bottom, dpi_, bg_[2], bg_[1], bg_[0], initial_.app.left, initial_.app.right, initial_.appCount, initial_.tray.left,
                initial_.tray.right);

    int passed = 0, failed = 0;
    std::vector<std::string> failures;
    for (const Scenario& s : scenarios) {
        const bool picked = options_.only.empty() || std::find(options_.only.begin(), options_.only.end(), s.name) != options_.only.end();
        if (!picked || (s.interactive && !options_.interactive) || (s.manual && options_.only.empty())) continue;
        if (RunScenario(s)) ++passed;
        else ++failed, failures.push_back(s.name);
    }

    backdrop_.Hide();
    if (!restore.empty()) {
        Process p = Launch(restore);
        if (p.handle) CloseHandle(p.handle);
    }
    std::string summary = Format("\n%d scenario(s) passed, %d failed", passed, failed);
    for (size_t i = 0; i < failures.size(); ++i) summary += (i ? ", " : ": ") + failures[i];
    if (warnings_) summary += Format("\n%d warning(s) about known Windows limitations (see report.txt)", warnings_);
    std::printf("%s\nresults: %s\n", summary.c_str(), Utf8(options_.out).c_str());
    FILE* report = nullptr;
    if (_wfopen_s(&report, (options_.out + L"\\report.txt").c_str(), L"a") == 0 && report) {
        std::fprintf(report, "%s\n", summary.c_str());
        std::fclose(report);
    }
    return failed ? 1 : 0;
}

// ------------------------------------------------------------------ scenarios

void OpenAndClose(Suite& s, const std::string& label) {
    double t = s.Mark("open an app" + label);
    s.Open();
    s.CheckMotion("open" + label, t, s.Settle(t));
    s.ExpectLayout("after open" + label);
    t = s.Mark("close it" + label);
    s.Close(0);
    s.CheckMotion("close" + label, t, s.Settle(t));
    s.ExpectLayout("after close" + label);
}

void MaximiseAndRestore(Suite& s, double expectMs) {
    double t = s.Mark("open an app");
    HWND w = s.Open();
    s.Settle(t);
    t = s.Mark("maximise it");
    ShowWindowAsync(w, SW_MAXIMIZE);
    s.CheckMorph("morph to full width", t, s.Settle(t), true, expectMs);
    t = s.Mark("restore it");
    ShowWindowAsync(w, SW_RESTORE);
    s.CheckMorph("morph back to islands", t, s.Settle(t), false, expectMs);
}

// Frames in [from, to] whose islands differ from the first one.
int ChangedFrames(Suite& s, double from, double to) {
    const std::vector<Frame> frames = s.Between(from, to);
    int changed = 0;
    for (const Frame& f : frames) changed += f.visible != frames.front().visible;
    return changed;
}

bool SecondariesClipped() {
    for (HWND h : fb::FindTaskbars()) {
        if (h != FindWindowW(L"Shell_TrayWnd", nullptr) && !fb::HasRegion(h)) return false;
    }
    return true;
}

HWND FindChild(HWND parent, const wchar_t* text) {
    struct Search {
        const wchar_t* text;
        HWND found;
    } search = {text, nullptr};
    EnumChildWindows(
        parent,
        [](HWND h, LPARAM p) {
            auto* s = reinterpret_cast<Search*>(p);
            wchar_t buf[256] = {};
            GetWindowTextW(h, buf, 256);
            if (wcscmp(buf, s->text) != 0) return TRUE;
            s->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// Clicks a settings checkbox or button the way the user would, minus the mouse.
bool Click(HWND settings, const wchar_t* text, bool check) {
    HWND control = FindChild(settings, text);
    if (!control) return false;
    if (check) SendMessageW(control, BM_SETCHECK, SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(settings, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(control), BN_CLICKED), reinterpret_cast<LPARAM>(control));
    return true;
}

void MoveCursor(Suite& s, bool ontoTaskbar) {
    const RECT& wr = s.TaskbarRect();
    if (ontoTaskbar) SetCursorPos(wr.left + 40, s.RowY());
    else SetCursorPos((wr.left + wr.right) / 2, wr.top - 400);
}

const Scenario kScenarios[] = {
    {"startup", "islands appear where the buttons are; quitting gives the taskbar back", false, true, nullptr,
     [](Suite& s) {
         s.ExpectLayout("after start");
         s.Expect("secondary taskbars are clipped too", SecondariesClipped());
     }},
    {"open-close", "an app opens and closes", false, true, nullptr, [](Suite& s) { OpenAndClose(s, ""); }},
    {"tight", "open and close with no padding (any lag cuts an icon)", false, true, [](fb::Config& c) { c.islandPadding = 0; },
     [](Suite& s) {
         for (int i = 1; i <= 3; ++i) OpenAndClose(s, Format(" #%d", i));
     }},
    {"close-middle", "three apps; the middle one closes", false, true, nullptr,
     [](Suite& s) {
         for (int i = 0; i < 3; ++i) {
             const double t = s.Mark("open an app");
             s.Open();
             s.Settle(t);
         }
         double t = s.Mark("close the middle one");
         s.Close(1);
         s.CheckMotion("close the middle app", t, s.Settle(t));
         s.ExpectLayout("after closing the middle app");
         t = s.Mark("close the other two together");
         s.CloseAll(0);
         s.CheckMotion("close two at once", t, s.Settle(t));
         s.ExpectLayout("after closing both");
     }},
    {"burst", "six apps open 120 ms apart, then close 120 ms apart", false, true, nullptr,
     [](Suite& s) {
         double t = s.Mark("open six apps, 120 ms apart");
         for (int i = 0; i < 6; ++i) {
             s.Open();
             Sleep(120);
         }
         s.CheckMotion("open six", t, s.Settle(t, 600, 1200), {.lagMs = 400});
         s.ExpectLayout("after opening six");
         t = s.Mark("close them, 120 ms apart");
         s.CloseAll(120);
         s.CheckMotion("close six", t, s.Settle(t, 600, 1200), {.lagMs = 400});
         s.ExpectLayout("after closing six");
     }},
    // With 30+ buttons on each taskbar a round of UI Automation reads takes ~40 ms,
    // and the group stops being centred, so while Windows moves buttons into its
    // overflow menu the islands can briefly trail the icons: warnings, not failures.
    {"overflow", "more apps than fit: Windows' overflow button appears; islands fit and never overlap", false, true, nullptr,
     [](Suite& s) {
         const auto before = s.ReadIslands();
         double t = s.Mark("open 40 apps in groups of 5");
         for (int group = 0; group < 8; ++group) {
             const double g = NowMs();
             for (int i = 0; i < 5; ++i) {
                 s.Open();
                 Sleep(60);
             }
             s.Settle(g, 400, 600);
         }
         double end = s.Settle(t);
         s.CheckMotion("filling the taskbar", t, end, {.monotonic = false, .smooth = false, .lagMs = 0, .cutsWarn = true});
         s.ExpectLayout("with the taskbar full");
         const auto full = s.ReadIslands();
         if (before && full) {
             s.Note(Format("app buttons %d -> %d, app island %ld-%ld -> %ld-%ld, tray from %ld", before->appCount, full->appCount, before->app.left,
                           before->app.right, full->app.left, full->app.right, full->tray.left));
             s.Expect("Windows overflows instead of adding buttons forever", full->appCount < before->appCount + 40,
                      Format("%d buttons for %d windows", full->appCount - before->appCount, 40));
         }
         int merged = 0;
         std::vector<Frame> frames = s.Between(t, end);
         double flash = -1;
         s.RemoveFlashes(frames, flash);  // reported by CheckMotion already
         for (const Frame& f : frames) merged += f.visible.size() < 2;
         s.Expect("the app island never runs into the tray island", merged == 0, merged ? Format("%d frames with one island", merged) : "");
         t = s.Mark("close them in groups of 5");
         while (s.Window(0)) {
             const double g = NowMs();
             for (int i = 0; i < 5 && s.Window(0); ++i) {
                 s.Close(0);
                 Sleep(60);
             }
             s.Settle(g, 400, 600);
         }
         end = s.Settle(t);
         s.CheckMotion("emptying the taskbar", t, end, {.monotonic = false, .smooth = false, .lagMs = 0, .cutsWarn = true});
         s.ExpectLayout("after closing all");
     }},
    {"reverse", "an app closes 150 ms after opening, mid-animation", false, true, nullptr,
     [](Suite& s) {
         const auto before = s.Latest();
         const double t = s.Mark("open an app, close it 150 ms later");
         s.Open();
         Sleep(150);
         s.Close(0);
         s.CheckMotion("open then close", t, s.Settle(t, 600, 1200), {.monotonic = false, .lagMs = 0});
         s.ExpectLayout("after open and close");
         const auto after = s.Latest();
         s.Expect("ends exactly where it started", before && after && before->visible == after->visible,
                  before && after ? RunsText(before->visible) + " -> " + RunsText(after->visible) : "");
     }},
    {"cycles", "ten open/close cycles", false, true, nullptr,
     [](Suite& s) {
         const double t = s.Mark("open and close an app ten times");
         for (int i = 0; i < 10; ++i) {
             s.Open();
             Sleep(700);
             s.Close(0);
             Sleep(700);
         }
         s.CheckMotion("ten cycles", t, s.Settle(t), {.monotonic = false, .lagMs = 0});
         s.ExpectLayout("after ten cycles");
     }},
    {"maximise", "a maximised window morphs the islands into the full taskbar and back", false, true,
     [](fb::Config& c) { c.fillOnMaximise = true; }, [](Suite& s) { MaximiseAndRestore(s, 200); }},
    {"maximise-off", "with full width on maximise off, maximising changes nothing", false, true, nullptr,
     [](Suite& s) {
         double t = s.Mark("open an app");
         HWND w = s.Open();
         s.Settle(t);
         t = s.Mark("maximise it");
         ShowWindowAsync(w, SW_MAXIMIZE);
         const double end = s.Settle(t, 500, 1000);
         s.Expect("islands stay as they are", ChangedFrames(s, t, end) == 0);
         ShowWindowAsync(w, SW_RESTORE);
     }},
    {"speed-50", "animation speed 50 %: the morph takes twice as long", false, true,
     [](fb::Config& c) {
         c.fillOnMaximise = true;
         c.animationSpeed = 50;
     },
     [](Suite& s) { MaximiseAndRestore(s, 400); }},
    {"speed-200", "animation speed 200 %: the morph takes half as long", false, true,
     [](fb::Config& c) {
         c.fillOnMaximise = true;
         c.animationSpeed = 200;
     },
     [](Suite& s) { MaximiseAndRestore(s, 100); }},
    {"animate-off", "smooth animation off: islands jump, but never cut an icon", false, true,
     [](fb::Config& c) {
         c.animate = false;
         c.fillOnMaximise = true;
     },
     [](Suite& s) {
         double t = s.Mark("open an app");
         HWND w = s.Open();
         s.CheckMotion("open", t, s.Settle(t), {.smooth = false, .lagMs = 0});
         s.ExpectLayout("after open");
         t = s.Mark("maximise it");
         ShowWindowAsync(w, SW_MAXIMIZE);
         double end = s.Settle(t);
         s.Expect("full width comes at once", ChangedFrames(s, t, end) > 0 && s.Latest() && s.FullWidth(*s.Latest()));
         t = s.Mark("restore it");
         ShowWindowAsync(w, SW_RESTORE);
         s.Settle(t);
         s.ExpectLayout("after restore");
         t = s.Mark("close it");
         s.Close(0);
         s.CheckMotion("close", t, s.Settle(t), {.smooth = false, .lagMs = 0});
         s.ExpectLayout("after close");
     }},
    {"separate-start", "Start in its own island; apps open and close beside it", false, true,
     [](fb::Config& c) { c.separateStart = true; },
     [](Suite& s) {
         s.ExpectLayout("after start");
         const double t = s.Mark("open and close an app");
         OpenAndClose(s, "");
         int merged = 0;
         for (const Frame& f : s.Between(t, NowMs())) {
             int appSide = 0;
             for (const Interval& r : f.visible) appSide += r.r <= s.TaskbarRect().right - s.Scale(200);
             merged += appSide < 2;
         }
         s.Expect("Start stays separate in every frame", merged == 0, merged ? Format("%d frames without the gap", merged) : "");
     }},
    {"bar", "bar mode: one bar across the taskbar that never moves", false, true, [](fb::Config& c) { c.mode = fb::LayoutMode::Bar; },
     [](Suite& s) {
         s.ExpectLayout("after start");
         double t = s.Mark("open an app");
         s.Open();
         double end = s.Settle(t);
         t = s.Mark("close it");
         s.Close(0);
         end = s.Settle(t);
         s.Expect("the bar never moves", ChangedFrames(s, t, end) == 0);
     }},
    {"tray-hidden", "tray island hidden", false, true, [](fb::Config& c) { c.trayMode = fb::TrayMode::Hide; },
     [](Suite& s) { OpenAndClose(s, ""); }},
    {"monitor-hidden", "this monitor set to hidden: no taskbar at all", false, true,
     [](fb::Config& c) { c.monitorModes[L"\\\\.\\DISPLAY1"] = fb::MonitorMode::Hidden; },
     [](Suite& s) {
         const auto f = s.Latest();
         s.Expect("nothing of the taskbar shows", f && f->visibleCols == 0, f ? Format("%d visible columns", f->visibleCols) : "no frame");
         s.Expect("other monitors keep their islands", SecondariesClipped());
     }},
    {"monitor-normal", "this monitor set to normal: the plain taskbar", false, true,
     [](fb::Config& c) { c.monitorModes[L"\\\\.\\DISPLAY1"] = fb::MonitorMode::Normal; },
     [](Suite& s) {
         const auto f = s.Latest();
         s.Expect("the whole taskbar shows, unclipped", f && s.FullWidth(*f) && !f->region.clipped, f ? RunsText(f->visible) : "no frame");
         s.Expect("other monitors keep their islands", SecondariesClipped());
     }},
    {"disabled", "FloatBar disabled: nothing is clipped", false, true, [](fb::Config& c) { c.enabled = false; },
     [](Suite& s) {
         Sleep(1500);
         const auto f = s.Latest();
         s.Expect("the taskbar stays unclipped", f && s.FullWidth(*f) && !AnyTaskbarClipped(), f ? RunsText(f->visible) : "no frame");
     }},
    {"show-desktop", "full width without the Show Desktop sliver", false, true,
     [](fb::Config& c) {
         c.fillOnMaximise = true;
         c.hideShowDesktop = true;
     },
     [](Suite& s) {
         const auto is = s.ReadIslands();
         double t = s.Mark("open an app");
         HWND w = s.Open();
         s.Settle(t);
         t = s.Mark("maximise it");
         ShowWindowAsync(w, SW_MAXIMIZE);
         s.Settle(t);
         const auto f = s.Latest();
         const bool trimmed = is && is->hasShowDesktop && f && f->visible.size() == 1 && f->visible[0].l <= s.TaskbarRect().left + 1 &&
                              std::abs(f->visible[0].r - static_cast<int>(is->showDesktop.left)) <= 1;
         s.Expect("full width ends where Show Desktop begins", trimmed,
                  (f ? "on screen " + RunsText(f->visible) : std::string("no frame")) +
                      (is && is->hasShowDesktop ? Format(", Show Desktop at %ld", is->showDesktop.left) : ", no Show Desktop button found"));
         ShowWindowAsync(w, SW_RESTORE);
         s.Settle(NowMs());
     }},
    {"border", "a 2 px border follows the islands exactly, frame by frame", false, true,
     [](fb::Config& c) {
         c.borderWidth = 2;
         c.borderColor = RGB(kTestBorderBgra[2], kTestBorderBgra[1], kTestBorderBgra[0]);
         c.borderOpacity = 100;
         c.fillOnMaximise = true;
     },
     [](Suite& s) {
         double t = s.Mark("open an app");
         HWND w = s.Open();
         s.CheckRing("open", t, s.Settle(t));
         t = s.Mark("maximise it");
         ShowWindowAsync(w, SW_MAXIMIZE);
         s.CheckRing("morph to full width", t, s.Settle(t));
         t = s.Mark("restore it");
         ShowWindowAsync(w, SW_RESTORE);
         s.CheckRing("morph back", t, s.Settle(t));
         t = s.Mark("close it");
         s.Close(0);
         s.CheckRing("close", t, s.Settle(t));
     }},
    {"settings", "the Settings window changes the islands live and saves the change", false, false, nullptr,
     [](Suite& s) {
         if (!s.Start(false)) return;
         HWND settings = WaitWindow(L"FloatBarSettings", s.FloatBar().pid, 3000);
         s.Expect("the Settings window opens", settings != nullptr);
         if (!settings) return;
         double t = s.Mark("tick 'Separate Start into its own island'");
         s.Expect("the checkbox exists", Click(settings, L"Separate Start into its own island", true));
         s.Config().separateStart = true;
         s.Settle(t);
         s.ExpectLayout("with Start separate");
         Sleep(600);  // the settings file is written 400 ms after the last change
         s.Expect("the change is saved to config.ini", fb::LoadConfig().separateStart);
         t = s.Mark("untick it");
         Click(settings, L"Separate Start into its own island", true);
         s.Config().separateStart = false;
         s.Settle(t);
         s.ExpectLayout("with Start joined again");
         t = s.Mark("press 'Exit FloatBar'");
         s.Expect("the Exit button exists", Click(settings, L"Exit FloatBar", false));
         s.Expect("FloatBar exits", WaitForSingleObject(s.FloatBar().handle, 5000) == WAIT_OBJECT_0);
         s.Expect("every taskbar is unclipped again after exit", !AnyTaskbarClipped());
         s.Forget();
     }},
    {"second-instance", "starting FloatBar again opens the running one's Settings", false, true, nullptr,
     [](Suite& s) {
         s.Mark("start a second FloatBar");
         Process second = Launch(L"\"" + s.FloatBarExe() + L"\" --config-dir \"" + s.Dir() + L"\"");
         const bool exited = second.handle && WaitForSingleObject(second.handle, 3000) == WAIT_OBJECT_0;
         s.Expect("the second one exits at once", exited);
         if (second.handle) CloseHandle(second.handle);
         HWND settings = WaitWindow(L"FloatBarSettings", s.FloatBar().pid, 3000);
         s.Expect("the running one shows its Settings window", settings != nullptr);
         const double t = s.Mark("close the Settings window");
         if (settings) PostMessageW(settings, WM_CLOSE, 0, 0);
         s.Settle(t);  // its taskbar button slides out
         s.Expect("closing Settings leaves FloatBar running",
                  WaitForSingleObject(s.FloatBar().handle, 0) == WAIT_TIMEOUT && !WindowOf(L"FloatBarSettings", s.FloatBar().pid));
         s.ExpectLayout("still clipped");
     }},
    {"reset", "after a crash, --reset removes the leftover clip; a restart clips again", false, true, nullptr,
     [](Suite& s) {
         s.Mark("kill FloatBar");
         TerminateProcess(s.FloatBar().handle, 1);
         WaitForSingleObject(s.FloatBar().handle, 3000);
         s.Forget();
         s.Expect("a killed FloatBar leaves its clip behind (as expected)", AnyTaskbarClipped());
         s.Mark("floatbar --reset");
         Process reset = Launch(L"\"" + s.FloatBarExe() + L"\" --reset");
         s.Expect("--reset finishes", reset.handle && WaitForSingleObject(reset.handle, 5000) == WAIT_OBJECT_0);
         if (reset.handle) CloseHandle(reset.handle);
         s.Expect("--reset unclips every taskbar", !AnyTaskbarClipped());
         if (s.Start()) s.ExpectLayout("after starting again");
     }},
    {"auto-hide", "auto-hide: hidden until the mouse reaches the taskbar", true, false, [](fb::Config& c) { c.autoHide = true; },
     [](Suite& s) {
         POINT saved;
         GetCursorPos(&saved);
         MoveCursor(s, false);
         if (!s.Start()) return;
         const auto f = s.Latest();
         s.Expect("hidden while the mouse is away", f && f->visibleCols == 0, f ? Format("%d visible columns", f->visibleCols) : "");
         double t = s.Mark("mouse onto the taskbar");
         MoveCursor(s, true);
         const double shown = s.WaitFor([](const Frame& fr) { return fr.visibleCols > 0; }, 2000);
         s.Expect("appears when the mouse arrives", shown >= 0, shown >= 0 ? Format("after %.0f ms", shown - t) : "not within 2 s");
         s.Settle(t);
         s.ExpectLayout("while hovered");
         t = s.Mark("mouse away");
         MoveCursor(s, false);
         const double hidden = s.WaitFor([](const Frame& fr) { return fr.visibleCols == 0; }, 3000);
         s.Expect("hides again after the mouse leaves", hidden >= 0, hidden >= 0 ? Format("after %.0f ms", hidden - t) : "not within 3 s");
         SetCursorPos(saved.x, saved.y);
     }},
    {"tray-hover", "tray island only while the mouse is over the taskbar", true, false, [](fb::Config& c) { c.trayMode = fb::TrayMode::Hover; },
     [](Suite& s) {
         POINT saved;
         GetCursorPos(&saved);
         MoveCursor(s, false);
         if (!s.Start()) return;
         s.ExpectLayout("mouse away (no tray)");
         double t = s.Mark("mouse onto the taskbar");
         MoveCursor(s, true);
         s.Config().trayMode = fb::TrayMode::Show;  // what hovering should look like
         s.CheckMotion("tray appears", t, s.Settle(t), {.lagMs = 0, .checkCuts = false});
         s.ExpectLayout("hovered (tray shown)");
         t = s.Mark("mouse away");
         MoveCursor(s, false);
         s.Config().trayMode = fb::TrayMode::Hover;
         s.CheckMotion("tray disappears", t, s.Settle(t, 500, 1500), {.lagMs = 0, .checkCuts = false});
         s.ExpectLayout("mouse away again");
         SetCursorPos(saved.x, saved.y);
     }},
    {"fullscreen", "a fullscreen app hides the taskbar; closing it brings the islands back", true, true, nullptr,
     [](Suite& s) {
         HWND before = GetForegroundWindow();
         const double t = s.Mark("fullscreen app in front");
         HWND w = SpawnDummy(900, true);
         s.Expect("the fullscreen window takes the focus", w && ForceForeground(w));
         // Explorer may drop the taskbar behind the window itself; FloatBar's own
         // hiding is an empty region (a moment later).
         const double hidden = s.WaitFor(
             [&s](const Frame& fr) {
                 const RowRegion rgn = ReadRegionRow(s.Taskbar(), s.TaskbarRect(), s.RowY());
                 return fr.visibleCols == 0 && rgn.clipped && rgn.runs.empty();
             },
             2000);
         s.Expect("FloatBar hides the taskbar", hidden >= 0, hidden >= 0 ? Format("after %.0f ms", hidden - t) : "not within 2 s");
         const double t2 = s.Mark("close the fullscreen app");
         if (w) PostMessageW(w, WM_CLOSE, 0, 0);
         WaitGone(w, 3000);
         if (before) ForceForeground(before);
         s.Settle(t2);
         s.ExpectLayout("after the fullscreen app");
     }},
    // Does re-sending an unchanged region to an idle taskbar ever make DWM drop it?
    {"region-stress", "re-send the same region as fast as possible for 20 s with nothing moving; count flashes", false, true, nullptr,
     [](Suite& s) {
         const double t = s.Mark("re-send the region");
         int calls = 0;
         while (NowMs() - t < 20000) {
             HRGN copy = CreateRectRgn(0, 0, 0, 0);
             if (GetWindowRgn(s.Taskbar(), copy) != ERROR && SetWindowRgn(s.Taskbar(), copy, TRUE)) ++calls;
             else DeleteObject(copy);
         }
         std::vector<Frame> frames = s.Between(t, NowMs());
         double first = -1;
         const int flashes = s.RemoveFlashes(frames, first);
         s.Note(Format("%d SetWindowRgn calls on an idle taskbar: %d flash(es)%s", calls, flashes,
                       flashes ? Format(", first at +%.0f ms", first - t).c_str() : ""));
         s.ExpectLayout("afterwards");
     },
     true},
};

}  // namespace

int RunSuite(int argc, wchar_t** argv) {
    Options o;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    o.floatbar = dir + L"\\floatbar.exe";
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    o.out = std::wstring(temp) + L"floatbar-suite";
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--floatbar" && i + 1 < argc) o.floatbar = argv[++i];
        else if (a == L"--out" && i + 1 < argc) o.out = argv[++i];
        else if (a == L"--no-interactive") o.interactive = false;
        else if (a == L"--no-floatbar-log") o.floatbarLog = false;
        else if (a == L"--only" && i + 1 < argc) {
            const std::string list = Utf8(argv[++i]);
            for (auto part : std::views::split(list, ',')) {
                if (!part.empty()) o.only.emplace_back(part.begin(), part.end());
            }
        } else if (a == L"--list") {
            for (const Scenario& s : kScenarios)
                std::printf("%-16s %s%s\n", s.name, s.what, s.manual ? " [only with --only]" : s.interactive ? " [interactive]" : "");
            return 0;
        }
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int rc;
    {
        Suite suite(o);
        rc = suite.Run(std::vector<Scenario>(std::begin(kScenarios), std::end(kScenarios)));
    }
    CoUninitialize();
    return rc;
}

}  // namespace probe
