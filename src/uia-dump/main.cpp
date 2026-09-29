// uia-dump: prints the raw UI Automation tree of every Windows taskbar.
//
// Diagnostic tool for milestone 1. It deliberately matches nothing: the only
// hardcoded names are the two top-level taskbar window classes.

#include <windows.h>
#include <UIAutomation.h>
#include <ShellScalingApi.h>
#include <wrl/client.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "common/uia_util.h"
#include "core/bounds.h"
#include "core/region.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kMaxDepth = 40;
constexpr int kMaxSiblings = 5000;

// Writes every line to stdout and, optionally, to a UTF-8 file.
class Output {
public:
    ~Output() { if (file_) fclose(file_); }

    bool OpenFile(const wchar_t* path) { return _wfopen_s(&file_, path, L"wb") == 0 && file_; }

    void Line(const std::wstring& s) {
        const std::string u = ib::Utf8(s);
        fwrite(u.data(), 1, u.size(), stdout);
        fputc('\n', stdout);
        if (file_) {
            fwrite(u.data(), 1, u.size(), file_);
            fwrite("\r\n", 1, 2, file_);
        }
    }

private:
    FILE* file_ = nullptr;
};

Output g_out;

std::wstring Indent(int depth) { return std::wstring(static_cast<size_t>(depth) * 2, L' '); }

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

void PrintEnvironment() {
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const wchar_t* adv = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t ts[64];
    swprintf_s(ts, L"%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

    const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
    const bool pmv2 = AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;

    g_out.Line(L"uia-dump  " + std::wstring(ts));
    g_out.Line(L"os: " + RegString(HKEY_LOCAL_MACHINE, nt, L"ProductName") + L" " +
               RegString(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion") + L" build " +
               RegString(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild") + L"." + RegDword(HKEY_LOCAL_MACHINE, nt, L"UBR"));
    g_out.Line(L"dpi awareness: " + std::to_wstring(GetAwarenessFromDpiAwarenessContext(ctx)) +
               (pmv2 ? L" (PerMonitorV2)" : L" (NOT PerMonitorV2 - coordinates may be virtualized!)"));
    g_out.Line(L"TaskbarAl (0=left, 1/missing=center): " + RegDword(HKEY_CURRENT_USER, adv, L"TaskbarAl"));
    g_out.Line(L"system dpi: " + std::to_wstring(GetDpiForSystem()));
}

void PrintElement(IUIAutomationElement* e, int depth, std::set<HWND>& seenHwnds) {
    CONTROLTYPEID type = 0;
    e->get_CurrentControlType(&type);

    BSTR b = nullptr;
    std::wstring cls, aid, name, fw;
    if (SUCCEEDED(e->get_CurrentClassName(&b))) cls = ib::TakeBstr(b);
    b = nullptr;
    if (SUCCEEDED(e->get_CurrentAutomationId(&b))) aid = ib::TakeBstr(b);
    b = nullptr;
    if (SUCCEEDED(e->get_CurrentName(&b))) name = ib::TakeBstr(b);
    b = nullptr;
    if (SUCCEEDED(e->get_CurrentFrameworkId(&b))) fw = ib::TakeBstr(b);

    RECT rc = {};
    e->get_CurrentBoundingRectangle(&rc);

    BOOL offscreen = FALSE;
    e->get_CurrentIsOffscreen(&offscreen);

    UIA_HWND nativeHwnd = nullptr;
    e->get_CurrentNativeWindowHandle(&nativeHwnd);
    const HWND hwnd = static_cast<HWND>(nativeHwnd);

    std::wstring line = Indent(depth) + ib::ControlTypeName(type) + L"(" + std::to_wstring(type) + L")" +
                        L" cls=\"" + ib::Escape(cls) + L"\"" +
                        L" aid=\"" + ib::Escape(aid) + L"\"" +
                        L" name=\"" + ib::Escape(name) + L"\"" +
                        L" rect=" + ib::FormatRect(rc) +
                        L" fw=" + (fw.empty() ? L"-" : fw) +
                        (offscreen ? L" OFFSCREEN" : L"");
    if (hwnd) {
        line += L" hwnd=" + ib::FormatHwnd(hwnd) + L"(" + ib::WindowClass(hwnd) + L")";
        seenHwnds.insert(hwnd);
    }
    g_out.Line(line);
}

void Walk(IUIAutomationTreeWalker* walker, IUIAutomationElement* e, int depth, std::set<HWND>& seenHwnds) {
    PrintElement(e, depth, seenHwnds);
    if (depth >= kMaxDepth) {
        g_out.Line(Indent(depth + 1) + L"... depth cap reached");
        return;
    }

    ComPtr<IUIAutomationElement> child;
    HRESULT hr = walker->GetFirstChildElement(e, &child);
    int count = 0;
    while (SUCCEEDED(hr) && child) {
        Walk(walker, child.Get(), depth + 1, seenHwnds);
        if (++count >= kMaxSiblings) {
            g_out.Line(Indent(depth + 1) + L"... sibling cap reached");
            break;
        }
        ComPtr<IUIAutomationElement> next;
        hr = walker->GetNextSiblingElement(child.Get(), &next);
        child = next;
    }
    if (FAILED(hr)) {
        wchar_t buf[64];
        swprintf_s(buf, L"... walker error 0x%08lX", static_cast<unsigned long>(hr));
        g_out.Line(Indent(depth + 1) + buf);
    }
}

bool DumpFromHwnd(IUIAutomation* uia, IUIAutomationTreeWalker* walker, HWND hwnd, int depth, std::set<HWND>& seenHwnds) {
    ComPtr<IUIAutomationElement> root;
    const HRESULT hr = uia->ElementFromHandle(hwnd, &root);
    if (FAILED(hr) || !root) {
        wchar_t buf[96];
        swprintf_s(buf, L"ElementFromHandle(%s) failed: 0x%08lX", ib::FormatHwnd(hwnd).c_str(), static_cast<unsigned long>(hr));
        g_out.Line(Indent(depth) + buf);
        return false;
    }
    seenHwnds.insert(hwnd);
    Walk(walker, root.Get(), depth, seenHwnds);
    return true;
}

// Prints the islands IslandBar would use for every taskbar.
int PrintIslands() {
    ib::BoundsReader reader;
    const HRESULT hr = reader.Init();
    if (FAILED(hr)) {
        fwprintf(stderr, L"BoundsReader::Init failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }
    for (HWND taskbar : ib::FindTaskbars()) {
        const DWORD start = GetTickCount();
        const ib::BoundsResult r = reader.Compute(taskbar);
        const DWORD ms = GetTickCount() - start;
        std::wstring line = ib::FormatHwnd(taskbar) + L" " + ib::WindowClass(taskbar) + L": ";
        if (!r.islands) {
            line += L"NO ISLANDS (" + r.error + L")";
        } else {
            line += L"app=" + ib::FormatRect(r.islands->app) + L" (" + std::to_wstring(r.islands->appCount) + L" buttons)";
            line += r.islands->hasTray ? L"  tray=" + ib::FormatRect(r.islands->tray) + L" (" + std::to_wstring(r.islands->trayCount) + L" buttons)"
                                       : L"  tray=none";
        }
        line += L"  [" + std::to_wstring(ms) + L" ms]";
        g_out.Line(line);
    }
    return 0;
}

int ParentDepth(HWND child, HWND root) {
    int depth = 0;
    for (HWND p = GetParent(child); p && p != root; p = GetParent(p)) ++depth;
    return depth;
}

void DumpTaskbar(IUIAutomation* uia, IUIAutomationTreeWalker* walker, HWND taskbar, int index) {
    RECT wr = {};
    GetWindowRect(taskbar, &wr);

    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    UINT mdx = 0, mdy = 0;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &mdx, &mdy);

    DWORD pid = 0;
    GetWindowThreadProcessId(taskbar, &pid);

    g_out.Line(L"");
    g_out.Line(L"================================================================================");
    g_out.Line(L"TASKBAR #" + std::to_wstring(index) + L"  " + ib::FormatHwnd(taskbar) + L"  class=" + ib::WindowClass(taskbar));
    g_out.Line(L"  window rect:  " + ib::FormatRect(wr));
    g_out.Line(L"  monitor rect: " + ib::FormatRect(mi.rcMonitor) + L"  work: " + ib::FormatRect(mi.rcWork) +
               ((mi.dwFlags & MONITORINFOF_PRIMARY) ? L"  PRIMARY" : L""));
    g_out.Line(L"  dpi (window): " + std::to_wstring(GetDpiForWindow(taskbar)) + L"  dpi (monitor): " + std::to_wstring(mdx) +
               L"  pid: " + std::to_wstring(pid) + L"  visible: " + (IsWindowVisible(taskbar) ? L"yes" : L"no"));
    g_out.Line(L"================================================================================");

    g_out.Line(L"");
    g_out.Line(L"--- UIA raw view from taskbar HWND ---");
    std::set<HWND> seenHwnds;
    DumpFromHwnd(uia, walker, taskbar, 0, seenHwnds);

    std::vector<HWND> children;
    EnumChildWindows(
        taskbar,
        [](HWND h, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&children));

    g_out.Line(L"");
    g_out.Line(L"--- child HWNDs (" + std::to_wstring(children.size()) + L") ---");
    for (HWND c : children) {
        RECT cr = {};
        GetWindowRect(c, &cr);
        g_out.Line(Indent(ParentDepth(c, taskbar)) + ib::FormatHwnd(c) + L" class=\"" + ib::Escape(ib::WindowClass(c)) + L"\" rect=" +
                   ib::FormatRect(cr) + (IsWindowVisible(c) ? L"" : L" HIDDEN") +
                   (seenHwnds.count(c) ? L" (in UIA walk)" : L" (NOT in UIA walk)"));
    }

    for (HWND c : children) {
        if (seenHwnds.count(c)) continue;
        g_out.Line(L"");
        g_out.Line(L"--- UIA raw view from child HWND " + ib::FormatHwnd(c) + L" (" + ib::WindowClass(c) + L") ---");
        DumpFromHwnd(uia, walker, c, 0, seenHwnds);
    }
}

void PrintUsage() {
    g_out.Line(L"usage: uia-dump.exe [--out <file>] [--islands]");
    g_out.Line(L"  Dumps the UI Automation raw view of Shell_TrayWnd and every Shell_SecondaryTrayWnd.");
    g_out.Line(L"  --out <file>  also write the output to <file> (UTF-8)");
    g_out.Line(L"  --islands     print only the island rectangles IslandBar computes");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    const wchar_t* outPath = nullptr;
    bool islandsOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--out" && i + 1 < argc) {
            outPath = argv[++i];
        } else if (arg == L"--islands") {
            islandsOnly = true;
        } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            PrintUsage();
            return 0;
        } else {
            PrintUsage();
            return 2;
        }
    }
    if (outPath && !g_out.OpenFile(outPath)) {
        fwprintf(stderr, L"cannot open output file: %s\n", outPath);
        return 1;
    }

    ib::ComInit com;
    if (FAILED(com.hr())) {
        fwprintf(stderr, L"CoInitializeEx failed: 0x%08lX\n", static_cast<unsigned long>(com.hr()));
        return 1;
    }
    if (islandsOnly) return PrintIslands();

    ComPtr<IUIAutomation> uia;
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    if (FAILED(hr)) hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    if (FAILED(hr) || !uia) {
        fwprintf(stderr, L"cannot create CUIAutomation: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    ComPtr<IUIAutomationTreeWalker> walker;
    hr = uia->get_RawViewWalker(&walker);
    if (FAILED(hr) || !walker) {
        fwprintf(stderr, L"cannot get RawViewWalker: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    PrintEnvironment();

    const std::vector<HWND> taskbars = ib::FindTaskbars();
    g_out.Line(L"taskbars found: " + std::to_wstring(taskbars.size()));
    if (taskbars.empty()) {
        g_out.Line(L"ERROR: no Shell_TrayWnd found (is explorer.exe running?)");
        return 1;
    }

    for (size_t i = 0; i < taskbars.size(); ++i) DumpTaskbar(uia.Get(), walker.Get(), taskbars[i], static_cast<int>(i));

    g_out.Line(L"");
    g_out.Line(L"done.");
    return 0;
}
