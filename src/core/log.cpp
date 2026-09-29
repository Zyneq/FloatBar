#include "core/log.h"

#include <windows.h>
#include <share.h>

#include <atomic>
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
std::atomic<bool> g_verbose = false;

void Open() { g_file = _wfsopen(g_path.c_str(), L"ab", _SH_DENYWR); }

void WriteV(const wchar_t* prefix, const wchar_t* format, va_list args) {
    wchar_t message[2048];
    _vsnwprintf_s(message, _TRUNCATE, format, args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[2200];
    swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %s%s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, t.wMilliseconds, prefix, message);
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

}  // namespace

void Init(const std::wstring& dir) {
    std::lock_guard lock(g_mutex);
    g_path = dir + L"\\log.txt";
    g_oldPath = dir + L"\\log.old.txt";
    Open();
}

std::wstring Path() { return g_path; }

void Write(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    WriteV(L"", format, args);
    va_end(args);
}

void SetVerbose(bool verbose) { g_verbose = verbose; }

void Debug(const wchar_t* format, ...) {
    if (!g_verbose) return;
    va_list args;
    va_start(args, format);
    WriteV(L"[debug] ", format, args);
    va_end(args);
}

}  // namespace ib::log
