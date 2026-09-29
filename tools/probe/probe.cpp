// floatbar-probe: measurement and test tool for FloatBar development (not shipped).
//
//   floatbar-probe suite [--only a,b] [--no-interactive] [--out <dir>] [--floatbar <exe>] [--list]
//       end-to-end tests of FloatBar, checked frame by frame on screen (see suite.cpp)
//   floatbar-probe replay <recording.csv>...
//       feeds recorded UI Automation readings through FloatBar's motion code and
//       checks the islands against the icons recorded on screen; no desktop needed
//   floatbar-probe record [--action open|close|close-first|maximize|restore|none] [--count N]
//                         [--spacing ms] [--ms 2500] [--csv <file>] [--verbose] [--no-uia]
//       captures every frame on the primary monitor (DXGI Desktop Duplication) and
//       records where the app icons are drawn, where the taskbar's clip is and,
//       on a second thread, what UI Automation reports. With FloatBar not running,
//       --csv writes a recording for `replay`.
//   floatbar-probe cleanup           close all probe test windows
//   floatbar-probe dummy <id> [fullscreen]   (internal) a test window with its own taskbar button
//
// All times are in ms on the QueryPerformanceCounter clock.

#include "common.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bounds.h"
#include "motion.h"
#include "region.h"

namespace probe {
int RunSuite(int argc, wchar_t** argv);
}

namespace {

using namespace probe;

constexpr int kIconBandHalf = 7;  // rows above/below the icon centre that are scanned
constexpr int kColorThreshold = 70;

// ------------------------------------------------------------------ record

struct FrameSample {
    double ms;
    UINT accumulated;  // presents merged into this frame
    int iconL, iconR;  // drawn app-icon extent (screen x), -1 if none
    int clipL, clipR;  // app-side clip extent at the icon row; full taskbar if no region
    bool cutL, cutR;   // an icon touches the clip edge (it is being cut)
};

struct UiaSample {
    double start, end;
    bool ok;
    LONG appL, appR;  // union of the visible app-side buttons (what FloatBar reads)
    int count;
};

struct Changes {
    double first = -1, last = -1;
    int distinct = 0, reversals = 0, maxStep = 0;
};

// How a value moved over the frames after the action.
Changes Analyze(const std::vector<std::pair<double, int>>& series, double actionMs) {
    Changes c;
    int prev = INT_MIN, prevDir = 0;
    for (const auto& [ms, v] : series) {
        if (prev != INT_MIN && v != prev && ms >= actionMs) {
            const int step = v - prev;
            const int dir = step > 0 ? 1 : -1;
            if (c.first < 0) c.first = ms;
            c.last = ms;
            ++c.distinct;
            c.maxStep = std::max(c.maxStep, std::abs(step));
            if (prevDir && dir != prevDir) ++c.reversals;
            prevDir = dir;
        }
        prev = v;
    }
    return c;
}

void PrintChanges(const char* name, const Changes& c, int from, int to, double actionMs) {
    if (c.first < 0) {
        std::printf("  %-10s no change (stays %d)\n", name, from);
        return;
    }
    std::printf("  %-10s %5d -> %5d | starts +%6.1f ms, ends +%6.1f ms (%.0f ms) | %3d positions, max step %3d px, %d reversal(s)\n", name, from,
                to, c.first - actionMs, c.last - actionMs, c.last - c.first, c.distinct, c.maxStep, c.reversals);
}

struct RecordOptions {
    std::wstring action = L"none";
    int count = 1;       // windows the action opens or closes
    int spacingMs = 120;
    int ms = 2500;
    const wchar_t* csv = nullptr;
    bool verbose = false, pollUia = true;
};

int Record(const RecordOptions& o) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) {
        std::printf("no taskbar\n");
        return 1;
    }
    RECT wr;
    GetWindowRect(taskbar, &wr);
    const int rowY = (wr.top + wr.bottom) / 2;
    const std::wstring& action = o.action;

    // Preconditions for the action.
    std::vector<HWND> dummies = FindDummies();
    if (action == L"close" || action == L"close-first" || action == L"maximize" || action == L"restore") {
        for (int id = static_cast<int>(dummies.size()) + 1; static_cast<int>(dummies.size()) < o.count; ++id) dummies.push_back(SpawnDummy(id));
        if (action == L"restore") ShowWindowAsync(dummies.back(), SW_MAXIMIZE);
        if (action == L"maximize") ShowWindowAsync(dummies.back(), SW_RESTORE);
        Sleep(2000);
    }

    // Where the tray starts: the icon search stops before it, and region
    // intervals left of the gap between app buttons and tray are the app side.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int trayLeft = wr.right, startMargin = wr.left + 3, sideSplit = wr.right;
    {
        fb::BoundsReader reader;
        if (SUCCEEDED(reader.Init())) {
            const auto r = reader.Compute(taskbar);
            if (r.islands) {
                if (r.islands->hasTray) {
                    trayLeft = r.islands->tray.left - 4;
                    sideSplit = (r.islands->app.right + r.islands->tray.left) / 2;
                }
                startMargin = r.islands->app.left + 3;
            }
        }
    }

    Capture capture;
    if (!capture.Init(MonitorFromWindow(taskbar, MONITOR_DEFAULTTOPRIMARY))) {
        std::printf("desktop duplication unavailable\n");
        return 1;
    }

    // Capture band: from a wallpaper row just above the taskbar (what shows
    // through where the taskbar is clipped) down to the last icon row.
    const RECT box = {wr.left, wr.top - 3, wr.right, rowY + kIconBandHalf + 1};
    const int bw = box.right - box.left, bh = box.bottom - box.top;
    const int iconRow0 = rowY - kIconBandHalf - box.top;

    std::vector<FrameSample> frames;
    std::vector<UiaSample> uia;
    std::mutex uiaMutex;
    std::atomic<bool> stop = false;
    const double origin = NowMs();

    std::thread uiaThread([&] {
        if (!o.pollUia) return;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            fb::BoundsReader reader;
            if (SUCCEEDED(reader.Init())) {
                while (!stop) {
                    const double t0 = NowMs() - origin;
                    const auto r = reader.Compute(taskbar);
                    UiaSample s = {t0, NowMs() - origin, r.islands.has_value(), 0, 0, 0};
                    if (r.islands) {
                        s.appL = r.islands->app.left;
                        s.appR = r.islands->app.right;
                        s.count = r.islands->appCount;
                    }
                    std::lock_guard lock(uiaMutex);
                    uia.push_back(s);
                }
            }
        }
        CoUninitialize();
    });

    const double actionAt = 400;
    int acted = 0;
    double actionMs = actionAt, nextAction = actionAt;
    std::vector<BYTE> px;
    BYTE bg[3] = {32, 32, 32};
    bool haveBg = false;
    // What shows through where the taskbar is clipped: the wallpaper row above
    // the taskbar, taken from the first frame (a window maximised later covers it).
    std::vector<BYTE> wallpaper;
    while (NowMs() - origin < o.ms && !capture.failed()) {
        if (acted < o.count && NowMs() - origin >= nextAction) {
            if (!acted) actionMs = NowMs() - origin;
            ++acted;
            nextAction += o.spacingMs;
            if (action == L"open") dummies.push_back(SpawnDummy(static_cast<int>(FindDummies().size()) + 1));
            else if ((action == L"close" || action == L"close-first") && !dummies.empty()) {
                const auto it = action == L"close" ? dummies.end() - 1 : dummies.begin();
                PostMessageW(*it, WM_CLOSE, 0, 0);
                dummies.erase(it);
            } else if (action == L"maximize" && !dummies.empty()) ShowWindowAsync(dummies.back(), SW_MAXIMIZE);
            else if (action == L"restore" && !dummies.empty()) ShowWindowAsync(dummies.back(), SW_RESTORE);
        }
        LONGLONG present = 0;
        UINT accumulated = 0;
        if (!capture.Next(20, box, px, present, accumulated)) continue;
        if (!haveBg) {  // taskbar background: inside the Start button, beside its icon
            const size_t i = (static_cast<size_t>(rowY - box.top) * bw + (startMargin - box.left)) * 4;
            bg[0] = px[i], bg[1] = px[i + 1], bg[2] = px[i + 2];
            wallpaper.assign(px.begin(), px.begin() + static_cast<size_t>(bw) * 4);
            haveBg = true;
        }
        FrameSample s = {};
        s.ms = QpcToMs(present) - origin;
        s.accumulated = accumulated;
        s.iconL = s.iconR = -1;
        for (int x = 0; x < bw && x < trayLeft - box.left; ++x) {
            const BYTE* above = &wallpaper[static_cast<size_t>(x) * 4];
            bool icon = false;
            for (int y = iconRow0; y < bh && !icon; ++y) {
                const BYTE* p = &px[(static_cast<size_t>(y) * bw + x) * 4];
                icon = Dist(p, bg) > kColorThreshold && Dist(p, above) > kColorThreshold;
            }
            if (!icon) continue;
            if (s.iconL < 0) s.iconL = box.left + x;
            s.iconR = box.left + x + 1;
        }
        const RowRegion rgn = ReadRegionRow(taskbar, wr, rowY);
        s.clipL = s.clipR = -1;
        for (const Interval& r : rgn.runs) {
            if (r.l >= sideSplit) continue;  // the tray island
            if (s.clipL < 0) s.clipL = r.l;
            s.clipR = r.r;
        }
        if (rgn.clipped && s.clipL >= 0 && s.iconL >= 0) {
            s.cutL = s.iconL <= s.clipL + 1;
            s.cutR = s.iconR >= s.clipR - 1;
        }
        frames.push_back(s);
    }
    stop = true;
    uiaThread.join();
    if (capture.failed()) std::printf("capture failed: 0x%08lX\n", static_cast<unsigned long>(capture.lastError()));

    // ---- report
    std::printf("action '%ls' x%d at %.1f ms, %zu frames, %zu UIA reads\n", action.c_str(), o.count, actionMs, frames.size(), uia.size());
    if (frames.empty()) return 1;
    std::vector<std::pair<double, int>> iconL, iconR, clipL, clipR;
    int cuts = 0;
    double firstCut = -1, lastCut = -1;
    for (const auto& s : frames) {
        if (s.iconL >= 0) {
            iconL.push_back({s.ms, s.iconL});
            iconR.push_back({s.ms, s.iconR});
        }
        if (s.clipL >= 0) {
            clipL.push_back({s.ms, s.clipL});
            clipR.push_back({s.ms, s.clipR});
        }
        if (s.cutL || s.cutR) {
            ++cuts;
            if (firstCut < 0) firstCut = s.ms;
            lastCut = s.ms;
        }
    }
    auto before = [&](const std::vector<std::pair<double, int>>& v) {
        int value = v.empty() ? 0 : v.front().second;
        for (auto& [ms, x] : v) {
            if (ms > actionMs) break;
            value = x;
        }
        return value;
    };
    std::printf("drawn icons:\n");
    PrintChanges("left", Analyze(iconL, actionMs), before(iconL), iconL.empty() ? 0 : iconL.back().second, actionMs);
    PrintChanges("right", Analyze(iconR, actionMs), before(iconR), iconR.empty() ? 0 : iconR.back().second, actionMs);
    if (!clipL.empty()) {
        std::printf("clip:\n");
        PrintChanges("left", Analyze(clipL, actionMs), before(clipL), clipL.back().second, actionMs);
        PrintChanges("right", Analyze(clipR, actionMs), before(clipR), clipR.back().second, actionMs);
    }
    std::printf("frames cutting an icon: %d", cuts);
    if (cuts) std::printf(" (from +%.1f to +%.1f ms)", firstCut - actionMs, lastCut - actionMs);
    std::printf("\nUIA readings after the action (start ms: buttons [L,R] #count):\n");
    {
        LONG pl = 0, pr = 0;
        int pc = -1;
        for (const auto& u : uia) {
            if (!u.ok) continue;
            if (u.start >= actionMs - 50 && (u.appL != pl || u.appR != pr || u.count != pc))
                std::printf("  +%6.1f: buttons [%ld,%ld] #%d\n", u.start - actionMs, u.appL, u.appR, u.count);
            pl = u.appL, pr = u.appR, pc = u.count;
        }
    }
    if (o.verbose) {
        std::printf("per-frame changes (ms: icons L-R | clip L-R | cut):\n");
        int pl = INT_MIN, pr = INT_MIN, pcl = INT_MIN, pcr = INT_MIN;
        for (const auto& s : frames) {
            if (s.iconL == pl && s.iconR == pr && s.clipL == pcl && s.clipR == pcr) continue;
            std::printf("  +%7.1f: icons %5d-%5d | clip %5d-%5d%s%s\n", s.ms - actionMs, s.iconL, s.iconR, s.clipL, s.clipR, s.cutL ? " CUT-L" : "",
                        s.cutR ? " CUT-R" : "");
            pl = s.iconL, pr = s.iconR, pcl = s.clipL, pcr = s.clipR;
        }
    }
    if (o.csv) {
        FILE* file = nullptr;
        if (_wfopen_s(&file, o.csv, L"w") == 0 && file) {
            std::fprintf(file, "kind,ms,a,b,c,d,e\n");
            std::fprintf(file, "action,%.2f,,,,,\n", actionMs);
            std::fprintf(file, "taskbar,0,%ld,%ld,,,\n", wr.left, wr.right);
            for (const auto& s : frames)
                std::fprintf(file, "frame,%.2f,%d,%d,%d,%d,%d\n", s.ms, s.iconL, s.iconR, s.clipL, s.clipR, (s.cutL ? 1 : 0) | (s.cutR ? 2 : 0));
            for (const auto& u : uia) std::fprintf(file, "uia,%.2f,%.2f,%ld,%ld,%d\n", u.start, u.end, u.appL, u.appR, u.count);
            std::fclose(file);
        }
    }
    CoUninitialize();
    return 0;
}

// ------------------------------------------------------------------ replay

// Replays one recording (made with `record --csv` while FloatBar was not
// running) through ReadingFilter and Pursuit exactly as the engine drives them,
// at 100 % speed, 96 dpi and no padding, and checks every recorded frame: the
// island must contain every drawn icon. A frame shows the region set one
// refresh before it was presented, so each frame is checked against the
// islands as they were one refresh earlier.
bool ReplayFile(const wchar_t* path, bool verbose) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"r") != 0 || !file) {
        std::printf("%ls: can't open\n", path);
        return false;
    }
    struct Reading {
        double at;
        LONG l, r;
        int count;
    };
    struct Shot {
        double ms;
        int iconL, iconR;
    };
    std::vector<Reading> readings;
    std::vector<Shot> shots;
    double actionMs = 0;
    long taskbarLeft = 0, taskbarRight = 1920;  // recordings without a taskbar row: the 1920 px primary monitor
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        double a = 0, b = 0;
        long c = 0, d = 0;
        int e = 0, f = 0;
        if (sscanf_s(line, "frame,%lf,%d,%d", &a, &e, &f) == 3 && e >= 0) shots.push_back({a, e, f});
        else if (sscanf_s(line, "uia,%lf,%lf,%ld,%ld,%d", &a, &b, &c, &d, &e) == 5 && e > 0) readings.push_back({b, c, d, e});
        else if (sscanf_s(line, "action,%lf", &a) == 1) actionMs = a;
        else if (sscanf_s(line, "taskbar,%lf,%ld,%ld", &a, &c, &d) == 3) taskbarLeft = c, taskbarRight = d;
    }
    std::fclose(file);
    std::sort(readings.begin(), readings.end(), [](const Reading& x, const Reading& y) { return x.at < y.at; });
    if (readings.empty() || shots.empty()) {
        std::printf("%ls: no readings or frames\n", path);
        return false;
    }

    // The engine's constants at 100 % speed and 96 dpi (engine.cpp).
    constexpr double kGlideMs = 200, kTrackSpeed = 0.6;
    const fb::SpanStyle flat{0, 0, 0};
    // One display refresh: the typical time between recorded frames.
    std::vector<double> gaps;
    for (size_t i = 1; i < shots.size(); ++i) gaps.push_back(shots[i].ms - shots[i - 1].ms);
    std::sort(gaps.begin(), gaps.end());
    const double refresh = gaps.empty() ? 16.7 : std::clamp(gaps[gaps.size() / 4], 2.0, 17.0);

    fb::ReadingFilter filter;
    fb::Pursuit pursuit;
    size_t next = 0;
    bool started = false;
    int cuts = 0, reversals = 0, checked = 0;
    double firstCut = -1, minSlack = 1e9, slackAt = 0;
    LONG shownL = LONG_MIN, shownR = LONG_MIN;
    int dirL = 0, dirR = 0;
    double settledAt = -1, iconsSettledAt = -1;
    int lastIconL = -1, lastIconR = -1;
    for (const Shot& shot : shots) {
        const bool iconsChanged = shot.iconL != lastIconL || shot.iconR != lastIconR;
        if (iconsChanged) iconsSettledAt = shot.ms;
        lastIconL = shot.iconL, lastIconR = shot.iconR;

        // What this frame shows was presented one refresh earlier.
        const double presented = shot.ms - refresh;
        while (next < readings.size() && readings[next].at <= presented) {
            const Reading& r = readings[next++];
            fb::Islands is;
            is.app = {r.l, 0, r.r, 48};
            is.appCount = r.count;
            const fb::Islands d = filter.Apply(is, 96, r.at, taskbarLeft + taskbarRight);
            const fb::TrackedEdges tr = filter.Tracked();
            pursuit.SetTarget({{d.app, 0, tr.appLeft, tr.appRight}}, flat, !started, kGlideMs, kTrackSpeed, r.at);
            started = true;
            if (verbose && r.at > actionMs - 50) {
                std::printf("  +%7.1f read %ld-%ld #%d -> %ld-%ld%s%s%s\n", r.at - actionMs, r.l, r.r, r.count, d.app.left, d.app.right,
                            tr.appLeft ? " trackL" : "", tr.appRight ? " trackR" : "", filter.Transitioning() ? "" : " (settled)");
            }
        }
        if (!started) continue;
        pursuit.Step(presented);
        const std::vector<fb::Span> shown = pursuit.Shown();
        if (shown.empty()) continue;
        const LONG l = shown.front().rect.left, r = shown.front().rect.right;
        const bool cut = shot.iconL < l || shot.iconR > r;
        if (shot.ms > actionMs) {
            ++checked;
            const double slack = std::min<double>(shot.iconL - l, r - shot.iconR);
            if (slack < minSlack) minSlack = slack, slackAt = shot.ms;
            if (cut) {
                if (!cuts) firstCut = shot.ms;
                ++cuts;
            }
        }
        if (verbose && shot.ms > actionMs && (l != shownL || r != shownR || iconsChanged || cut)) {
            std::printf("  +%7.1f frame icons %d-%d  island %ld-%ld%s\n", shot.ms - actionMs, shot.iconL, shot.iconR, l, r, cut ? "  CUT" : "");
        }
        if (shownL != LONG_MIN && shot.ms > actionMs) {
            if (l != shownL) {
                const int dir = l > shownL ? 1 : -1;
                reversals += dirL && dir != dirL;
                dirL = dir;
                settledAt = shot.ms;
            }
            if (r != shownR) {
                const int dir = r > shownR ? 1 : -1;
                reversals += dirR && dir != dirR;
                dirR = dir;
                settledAt = shot.ms;
            }
        }
        shownL = l, shownR = r;
    }
    const Reading& last = readings.back();
    const bool finalOk = shownL == last.l && shownR == last.r;
    const bool ok = cuts == 0 && reversals == 0 && finalOk;
    std::printf("%s %ls: %d frames checked, %zu readings | cuts %d%s | closest icon %.0f px at +%.0f ms | reversals %d | "
                "islands settle +%.0f ms, icons +%.0f ms | final %ld-%ld %s\n",
                ok ? "PASS" : "FAIL", path, checked, readings.size(), cuts, cuts ? (" (first +" + std::to_string(static_cast<int>(firstCut - actionMs)) + " ms)").c_str() : "",
                minSlack, slackAt - actionMs, reversals, settledAt - actionMs, iconsSettledAt - actionMs, shownL, shownR,
                finalOk ? "ok" : ("expected " + std::to_string(last.l) + "-" + std::to_string(last.r)).c_str());
    return ok;
}

// Prints the raw taskbar button list (as BoundsReader sees it, before
// clustering) every time it changes, while `open` test windows open and then
// close `spacing` ms apart.
int TraceButtons(int open, int spacing) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Microsoft::WRL::ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) return 1;
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    Microsoft::WRL::ComPtr<IUIAutomationElement> root, frame;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> frameCond, buttonCond;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"TaskbarFrame");
    uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &frameCond);
    VariantClear(&v);
    v.vt = VT_I4;
    v.lVal = UIA_ButtonControlTypeId;
    uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &buttonCond);
    if (FAILED(uia->ElementFromHandle(taskbar, &root)) || FAILED(root->FindFirst(TreeScope_Descendants, frameCond.Get(), &frame)) || !frame) {
        std::printf("no taskbar frame\n");
        return 1;
    }
    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cache;
    uia->CreateCacheRequest(&cache);
    cache->AddProperty(UIA_ClassNamePropertyId);
    cache->AddProperty(UIA_BoundingRectanglePropertyId);
    cache->AddProperty(UIA_IsOffscreenPropertyId);
    cache->AddProperty(UIA_NamePropertyId);
    const double origin = NowMs();
    std::vector<HWND> windows;
    std::string previous;
    int opened = 0, closed = 0;
    double next = 300;
    while (NowMs() - origin < 300 + 2.0 * open * spacing + 2500) {
        const double t = NowMs() - origin;
        if (t >= next && opened < open) {
            windows.push_back(SpawnDummy(100 + opened++));
            next = t + spacing;
            if (opened == open) next = t + 1500;
        } else if (t >= next && opened == open && closed < open) {
            PostMessageW(windows[closed++], WM_CLOSE, 0, 0);
            next = t + spacing;
        }
        Microsoft::WRL::ComPtr<IUIAutomationElementArray> found;
        if (FAILED(frame->FindAllBuildCache(TreeScope_Descendants, buttonCond.Get(), cache.Get(), &found)) || !found) continue;
        int n = 0;
        found->get_Length(&n);
        std::string line;
        for (int i = 0; i < n; ++i) {
            Microsoft::WRL::ComPtr<IUIAutomationElement> e;
            RECT r = {};
            BOOL off = FALSE;
            BSTR cls = nullptr;
            if (FAILED(found->GetElement(i, &e)) || !e) continue;
            e->get_CachedBoundingRectangle(&r);
            e->get_CachedIsOffscreen(&off);
            e->get_CachedClassName(&cls);
            const bool app = cls && wcscmp(cls, L"Taskbar.TaskListButtonAutomationPeer") == 0;
            SysFreeString(cls);
            line += " " + std::to_string(r.left) + "-" + std::to_string(r.right) + (app ? "" : "s") + (off ? "(off)" : "") +
                    (r.right <= r.left ? "(empty)" : "");
        }
        if (line != previous) {
            std::printf("+%7.1f #%2d:%s\n", NowMs() - origin, n, line.c_str());
            previous = line;
        }
    }
    CloseDummies();
    CoUninitialize();
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring command = argc >= 2 ? argv[1] : L"";
    if (command == L"dummy" && argc >= 3) return RunDummy(_wtoi(argv[2]), argc >= 4 && std::wstring(argv[3]) == L"fullscreen");
    if (command == L"cleanup") {
        CloseDummies();
        return 0;
    }
    if (command == L"suite") return RunSuite(argc, argv);
    if (command == L"trace") return TraceButtons(argc >= 3 ? _wtoi(argv[2]) : 6, argc >= 4 ? _wtoi(argv[3]) : 120);
    if (command == L"replay" && argc >= 3) {
        bool ok = true, verbose = false;
        for (int i = 2; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--verbose") verbose = true;
            else ok &= ReplayFile(argv[i], verbose);
        }
        return ok ? 0 : 1;
    }
    if (command == L"record") {
        RecordOptions o;
        for (int i = 2; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--action" && i + 1 < argc) o.action = argv[++i];
            else if (a == L"--count" && i + 1 < argc) o.count = std::max(1, _wtoi(argv[++i]));
            else if (a == L"--spacing" && i + 1 < argc) o.spacingMs = _wtoi(argv[++i]);
            else if (a == L"--ms" && i + 1 < argc) o.ms = _wtoi(argv[++i]);
            else if (a == L"--csv" && i + 1 < argc) o.csv = argv[++i];
            else if (a == L"--verbose") o.verbose = true;
            else if (a == L"--no-uia") o.pollUia = false;  // don't compete with FloatBar for explorer
        }
        return Record(o);
    }
    std::printf("usage: floatbar-probe suite [--only a,b] [--no-interactive] [--out dir] [--floatbar exe] [--list]\n"
                "       floatbar-probe replay <recording.csv>...\n"
                "       floatbar-probe record [--action open|close|close-first|maximize|restore|none] [--count N] [--spacing ms]\n"
                "                             [--ms 2500] [--csv file] [--verbose] [--no-uia]\n"
                "       floatbar-probe cleanup\n");
    return 2;
}
