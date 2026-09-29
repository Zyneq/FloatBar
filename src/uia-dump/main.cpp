// uia-dump: prints the raw UI Automation tree of every Windows taskbar, or the
// islands IslandBar computes from it. Diagnostic tool; matches nothing itself.

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>

#include "common/uia_util.h"
#include "core/bounds.h"
#include "core/region.h"
#include "core/tree_dump.h"

namespace {

// Writes to stdout and, optionally, to a UTF-8 file.
void Emit(const std::wstring& text, FILE* file) {
    const std::string u = ib::Utf8(text);
    fwrite(u.data(), 1, u.size(), stdout);
    if (file) fwrite(u.data(), 1, u.size(), file);
}

std::wstring Islands() {
    ib::BoundsReader reader;
    const HRESULT hr = reader.Init();
    if (FAILED(hr)) return L"BoundsReader::Init failed\r\n";
    std::wstring out;
    for (HWND taskbar : ib::FindTaskbars()) {
        const DWORD start = GetTickCount();
        const ib::BoundsResult r = reader.Compute(taskbar);
        const DWORD ms = GetTickCount() - start;
        out += ib::FormatHwnd(taskbar) + L" " + ib::WindowClass(taskbar) + L": ";
        if (!r.islands) {
            out += L"NO ISLANDS (" + r.error + L")";
        } else {
            out += L"app=" + ib::FormatRect(r.islands->app) + L" (" + std::to_wstring(r.islands->appCount) + L" buttons)";
            for (const RECT& e : r.islands->extras) out += L"  extra=" + ib::FormatRect(e);
            out += r.islands->hasTray ? L"  tray=" + ib::FormatRect(r.islands->tray) + L" (" + std::to_wstring(r.islands->trayCount) + L" buttons)"
                                      : L"  tray=none";
        }
        out += L"  [" + std::to_wstring(ms) + L" ms]\r\n";
    }
    return out;
}

const wchar_t kUsage[] =
    L"usage: uia-dump.exe [--out <file>] [--islands] [--no-names]\r\n"
    L"  Dumps the UI Automation raw view of Shell_TrayWnd and every Shell_SecondaryTrayWnd.\r\n"
    L"  --out <file>  also write the output to <file> (UTF-8)\r\n"
    L"  --islands     print only the island rectangles IslandBar computes\r\n"
    L"  --no-names    remove element names (window titles, tooltips) before sharing\r\n";

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);  // the text already has \r\n line ends

    const wchar_t* outPath = nullptr;
    bool islandsOnly = false;
    bool names = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--out" && i + 1 < argc) outPath = argv[++i];
        else if (arg == L"--islands") islandsOnly = true;
        else if (arg == L"--no-names") names = false;
        else {
            Emit(kUsage, nullptr);
            return arg == L"--help" || arg == L"-h" || arg == L"/?" ? 0 : 2;
        }
    }

    FILE* file = nullptr;
    if (outPath && _wfopen_s(&file, outPath, L"wb") != 0) {
        fwprintf(stderr, L"cannot open output file: %s\n", outPath);
        return 1;
    }

    ib::ComInit com;
    if (FAILED(com.hr())) return 1;
    Emit(islandsOnly ? Islands() : ib::DumpTaskbarTrees(names), file);
    if (file) fclose(file);
    return 0;
}
