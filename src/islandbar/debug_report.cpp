#include "islandbar/debug_report.h"

#include <shellapi.h>

#include <fstream>
#include <sstream>
#include <vector>

#include "common/uia_util.h"
#include "common/version.h"
#include "core/config.h"
#include "core/log.h"
#include "core/tree_dump.h"

namespace ib {
namespace {

constexpr size_t kLogLines = 400;

std::wstring ReadUtf8File(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return FromUtf8(buffer.str());
}

std::wstring LastLines(const std::wstring& text, size_t count) {
    size_t pos = text.size();
    for (size_t found = 0; pos > 0 && found <= count;) {
        pos = text.rfind(L'\n', pos - 1);
        if (pos == std::wstring::npos) return text;
        ++found;
    }
    return text.substr(pos + 1);
}

void ReplaceAll(std::wstring& text, const std::wstring& from, const std::wstring& to) {
    if (from.size() < 3) return;  // too short to redact safely
    for (size_t pos = 0; (pos = text.find(from, pos)) != std::wstring::npos; pos += to.size()) text.replace(pos, from.size(), to);
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(name, buf, MAX_PATH);
    return buf;
}

// Masks the profile path, user name and computer name wherever they appear.
void Redact(std::wstring& text) {
    ReplaceAll(text, Env(L"USERPROFILE"), L"%USERPROFILE%");
    ReplaceAll(text, Env(L"USERNAME"), L"<user>");
    ReplaceAll(text, Env(L"COMPUTERNAME"), L"<computer>");
}

std::wstring Timestamp(bool forFileName) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, forFileName ? L"%04u%02u%02u-%02u%02u%02u" : L"%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay,
               t.wHour, t.wMinute, t.wSecond);
    return buf;
}

}  // namespace

void CreateDebugReport(HWND owner, const std::wstring& status) {
    const int consent = MessageBoxW(
        owner,
        L"IslandBar will create a text file you can attach to a GitHub issue. It contains:\n\n"
        L"• IslandBar version, Windows build, DPI and monitor layout\n"
        L"• your IslandBar settings (config.json)\n"
        L"• the taskbar's UI structure: element types, class names, IDs and positions\n"
        L"• the last 400 lines of log.txt\n\n"
        L"Removed before saving: element names (window titles, app names, tray tooltips such as "
        L"Wi-Fi network names), app IDs of taskbar buttons, your user name, computer name and profile path.\n\n"
        L"Nothing is uploaded. The file is saved in the IslandBar config folder so you can read it "
        L"before deciding to share it.\n\nCreate the debug report?",
        L"IslandBar – debug report", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (consent != IDYES) return;

    std::wstring report;
    report += L"IslandBar debug report\r\n";
    report += L"version: " + std::wstring(kVersion) + L"\r\n";
    report += L"created: " + Timestamp(false) + L"\r\n\r\n";
    report += L"===== status =====\r\n" + status + L"\r\n\r\n";
    report += L"===== config.json =====\r\n" + ReadUtf8File(ConfigPath()) + L"\r\n";
    report += L"===== taskbar UI Automation tree (names removed) =====\r\n" + DumpTaskbarTrees(false) + L"\r\n";
    report += L"===== log.txt (last " + std::to_wstring(kLogLines) + L" lines) =====\r\n" + LastLines(ReadUtf8File(log::Path()), kLogLines);
    Redact(report);

    const std::wstring path = ConfigDir() + L"\\debug-report-" + Timestamp(true) + L".txt";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        const std::string utf8 = "\xEF\xBB\xBF" + Utf8(report);  // BOM so Notepad detects UTF-8
        file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        if (!file.good()) {
            MessageBoxW(owner, (L"Could not write " + path).c_str(), L"IslandBar", MB_ICONERROR);
            return;
        }
    }
    log::Write(L"debug report written: %s", path.c_str());

    const std::wstring select = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);

    if (MessageBoxW(owner,
                    L"The report was saved and is selected in File Explorer. Please open it and check it before sharing.\n\n"
                    L"Open a new GitHub issue now? You can drag the file into the issue.",
                    L"IslandBar – debug report", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        ShellExecuteW(nullptr, L"open", kIssuesUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

}  // namespace ib
