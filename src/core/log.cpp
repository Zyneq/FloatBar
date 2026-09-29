#include "core/log.h"

#include <windows.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "common/uia_util.h"

namespace ib::log {
namespace {

constexpr long kMaxBytes = 1024 * 1024;

std::mutex g_mutex;
std::wstring g_path;
std::wstring g_oldPath;
FILE* g_file = nullptr;

void Open() { g_file = _wfsopen(g_path.c_str(), L"ab", _SH_DENYWR); }

}  // namespace

void Init(const std::wstring& dir) {
    std::lock_guard lock(g_mutex);
    g_path = dir + L"\\log.txt";
    g_oldPath = dir + L"\\log.old.txt";
    Open();
}

void Write(const wchar_t* format, ...) {
    wchar_t message[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[2200];
    swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, t.wMilliseconds, message);
    const std::string utf8 = Utf8(line);

    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    fwrite(utf8.data(), 1, utf8.size(), g_file);
    fflush(g_file);
    if (ftell(g_file) > kMaxBytes) {
        fclose(g_file);
        MoveFileExW(g_path.c_str(), g_oldPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        Open();
    }
}

}  // namespace ib::log
