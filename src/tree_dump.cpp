#include "tree_dump.h"

#include <windows.h>
#include <UIAutomation.h>
#include <ShellScalingApi.h>
#include <wrl/client.h>

#include <set>
#include <vector>

#include "bounds.h"
#include "region.h"
#include "uia_util.h"

using Microsoft::WRL::ComPtr;

namespace fb {
namespace {

constexpr int kMaxDepth = 40;
constexpr int kMaxSiblings = 5000;

class Dumper {
public:
    explicit Dumper(bool includeNames) : includeNames_(includeNames) {}

    std::wstring Run() {
        ComPtr<IUIAutomation> uia;
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
        if (FAILED(hr)) hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
        if (FAILED(hr) || !uia || FAILED(uia->get_RawViewWalker(&walker_)) || !walker_) {
            Line(L"ERROR: cannot create UI Automation");
            return out_;
        }
        uia_ = uia;

        const std::vector<HWND> taskbars = FindTaskbars();
        Line(L"taskbars found: " + std::to_wstring(taskbars.size()));
        for (size_t i = 0; i < taskbars.size(); ++i) DumpTaskbar(taskbars[i], static_cast<int>(i));
        return out_;
    }

private:
    void Line(const std::wstring& s) { out_ += s + L"\r\n"; }
    static std::wstring Indent(int depth) { return std::wstring(static_cast<size_t>(depth) * 2, L' '); }

    std::wstring NameValue(const std::wstring& name) const {
        if (includeNames_) return L"\"" + Escape(name) + L"\"";
        return name.empty() ? L"\"\"" : L"<" + std::to_wstring(name.size()) + L" chars removed>";
    }

    // Taskbar app buttons carry "Appid: <application id>", which reveals installed apps.
    std::wstring IdValue(const std::wstring& aid) const {
        static const std::wstring kAppIdPrefix = L"Appid: ";
        if (!includeNames_ && aid.starts_with(kAppIdPrefix)) return L"\"" + kAppIdPrefix + L"<removed>\"";
        return L"\"" + Escape(aid) + L"\"";
    }

    void PrintElement(IUIAutomationElement* e, int depth) {
        CONTROLTYPEID type = 0;
        e->get_CurrentControlType(&type);
        BSTR b = nullptr;
        std::wstring cls, aid, name, fw;
        if (SUCCEEDED(e->get_CurrentClassName(&b))) cls = TakeBstr(b);
        b = nullptr;
        if (SUCCEEDED(e->get_CurrentAutomationId(&b))) aid = TakeBstr(b);
        b = nullptr;
        if (SUCCEEDED(e->get_CurrentName(&b))) name = TakeBstr(b);
        b = nullptr;
        if (SUCCEEDED(e->get_CurrentFrameworkId(&b))) fw = TakeBstr(b);
        RECT rc = {};
        e->get_CurrentBoundingRectangle(&rc);
        BOOL offscreen = FALSE;
        e->get_CurrentIsOffscreen(&offscreen);
        UIA_HWND nativeHwnd = nullptr;
        e->get_CurrentNativeWindowHandle(&nativeHwnd);
        const HWND hwnd = static_cast<HWND>(nativeHwnd);

        std::wstring line = Indent(depth) + ControlTypeName(type) + L"(" + std::to_wstring(type) + L")" + L" cls=\"" + Escape(cls) +
                            L"\" aid=" + IdValue(aid) + L" name=" + NameValue(name) + L" rect=" + FormatRect(rc) +
                            L" fw=" + (fw.empty() ? L"-" : fw) + (offscreen ? L" OFFSCREEN" : L"");
        if (hwnd) {
            line += L" hwnd=" + FormatHwnd(hwnd) + L"(" + WindowClass(hwnd) + L")";
            seen_.insert(hwnd);
        }
        Line(line);
    }

    void Walk(IUIAutomationElement* e, int depth) {
        PrintElement(e, depth);
        if (depth >= kMaxDepth) {
            Line(Indent(depth + 1) + L"... depth cap reached");
            return;
        }
        ComPtr<IUIAutomationElement> child;
        HRESULT hr = walker_->GetFirstChildElement(e, &child);
        int count = 0;
        while (SUCCEEDED(hr) && child) {
            Walk(child.Get(), depth + 1);
            if (++count >= kMaxSiblings) {
                Line(Indent(depth + 1) + L"... sibling cap reached");
                break;
            }
            ComPtr<IUIAutomationElement> next;
            hr = walker_->GetNextSiblingElement(child.Get(), &next);
            child = next;
        }
        if (FAILED(hr)) {
            wchar_t buf[64];
            swprintf_s(buf, L"... walker error 0x%08lX", static_cast<unsigned long>(hr));
            Line(Indent(depth + 1) + buf);
        }
    }

    void DumpFromHwnd(HWND hwnd) {
        ComPtr<IUIAutomationElement> root;
        const HRESULT hr = uia_->ElementFromHandle(hwnd, &root);
        if (FAILED(hr) || !root) {
            wchar_t buf[96];
            swprintf_s(buf, L"ElementFromHandle(%s) failed: 0x%08lX", FormatHwnd(hwnd).c_str(), static_cast<unsigned long>(hr));
            Line(buf);
            return;
        }
        seen_.insert(hwnd);
        Walk(root.Get(), 0);
    }

    static int ParentDepth(HWND child, HWND root) {
        int depth = 0;
        for (HWND p = GetParent(child); p && p != root; p = GetParent(p)) ++depth;
        return depth;
    }

    void DumpTaskbar(HWND taskbar, int index) {
        RECT wr = {};
        GetWindowRect(taskbar, &wr);
        MONITORINFO mi = {sizeof(mi)};
        HMONITOR mon = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfoW(mon, &mi);
        UINT mdx = 0, mdy = 0;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &mdx, &mdy);

        Line(L"");
        Line(L"================================================================================");
        Line(L"TASKBAR #" + std::to_wstring(index) + L"  " + FormatHwnd(taskbar) + L"  class=" + WindowClass(taskbar));
        Line(L"  window rect:  " + FormatRect(wr));
        Line(L"  monitor rect: " + FormatRect(mi.rcMonitor) + L"  work: " + FormatRect(mi.rcWork) +
             ((mi.dwFlags & MONITORINFOF_PRIMARY) ? L"  PRIMARY" : L""));
        Line(L"  dpi (window): " + std::to_wstring(GetDpiForWindow(taskbar)) + L"  dpi (monitor): " + std::to_wstring(mdx) +
             L"  visible: " + (IsWindowVisible(taskbar) ? L"yes" : L"no"));
        Line(L"================================================================================");
        Line(L"");
        Line(L"--- UIA raw view from taskbar HWND ---");
        seen_.clear();
        DumpFromHwnd(taskbar);

        std::vector<HWND> children;
        EnumChildWindows(
            taskbar,
            [](HWND h, LPARAM lp) -> BOOL {
                reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&children));

        Line(L"");
        Line(L"--- child HWNDs (" + std::to_wstring(children.size()) + L") ---");
        for (HWND c : children) {
            RECT cr = {};
            GetWindowRect(c, &cr);
            Line(Indent(ParentDepth(c, taskbar)) + FormatHwnd(c) + L" class=\"" + Escape(WindowClass(c)) + L"\" rect=" +
                 FormatRect(cr) + (IsWindowVisible(c) ? L"" : L" HIDDEN") + (seen_.count(c) ? L" (in UIA walk)" : L" (NOT in UIA walk)"));
        }
        for (HWND c : children) {
            if (seen_.count(c)) continue;
            Line(L"");
            Line(L"--- UIA raw view from child HWND " + FormatHwnd(c) + L" (" + WindowClass(c) + L") ---");
            DumpFromHwnd(c);
        }
    }

    bool includeNames_;
    ComPtr<IUIAutomation> uia_;
    ComPtr<IUIAutomationTreeWalker> walker_;
    std::set<HWND> seen_;
    std::wstring out_;
};

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"<missing>";
    return buf;
}

std::wstring RegDword(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD v = 0;
    DWORD size = sizeof(v);
    if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return L"<missing>";
    return std::to_wstring(v);
}

}  // namespace

std::wstring DescribeEnvironment() {
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const wchar_t* adv = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
    const bool pmv2 = AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;

    std::wstring s;
    // ProductName still says "Windows 10" on Windows 11; the build number is what matters.
    s += L"os: " + RegString(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion") + L" build " + RegString(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild") +
         L"." + RegDword(HKEY_LOCAL_MACHINE, nt, L"UBR") + L" (" + RegString(HKEY_LOCAL_MACHINE, nt, L"EditionID") + L")\r\n";
    s += L"dpi awareness: " + std::to_wstring(GetAwarenessFromDpiAwarenessContext(ctx)) +
         (pmv2 ? L" (PerMonitorV2)" : L" (NOT PerMonitorV2 - coordinates may be virtualized!)") + L"\r\n";
    s += L"TaskbarAl (0=left, 1/missing=center): " + RegDword(HKEY_CURRENT_USER, adv, L"TaskbarAl") + L"\r\n";
    s += L"system dpi: " + std::to_wstring(GetDpiForSystem()) + L"\r\n";
    s += L"monitors: " + std::to_wstring(GetSystemMetrics(SM_CMONITORS)) + L"\r\n";
    return s;
}

// What FloatBar computes from the trees above, one line per taskbar.
std::wstring ComputedIslands() {
    BoundsReader reader;
    if (FAILED(reader.Init())) return L"BoundsReader::Init failed\r\n";
    std::wstring out;
    for (HWND taskbar : FindTaskbars()) {
        const BoundsResult r = reader.Compute(taskbar);
        out += FormatHwnd(taskbar) + L" " + WindowClass(taskbar) + L": ";
        if (!r.islands) {
            out += L"NO ISLANDS (" + r.error + L")\r\n";
            continue;
        }
        out += L"app=" + FormatRect(r.islands->app) + L" (" + std::to_wstring(r.islands->appCount) + L" buttons)";
        if (r.islands->hasSplit) out += L"  startSplit=" + std::to_wstring(r.islands->split);
        for (const RECT& e : r.islands->extras) out += L"  extra=" + FormatRect(e);
        out += r.islands->hasTray ? L"  tray=" + FormatRect(r.islands->tray) : L"  tray=none";
        if (r.islands->hasShowDesktop) out += L"  showDesktop=" + FormatRect(r.islands->showDesktop);
        out += L"\r\n";
    }
    return out;
}

std::wstring DumpTaskbarTrees(bool includeNames) {
    return DescribeEnvironment() + Dumper(includeNames).Run() + L"\r\n===== computed islands =====\r\n" + ComputedIslands();
}

}  // namespace fb
