#include "config.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace fb {
namespace {

constexpr wchar_t kSection[] = L"FloatBar";
constexpr wchar_t kMonitorsSection[] = L"Monitors";

// Written once when the file is created. UTF-16 with BOM makes the profile API
// keep the file Unicode.
constexpr wchar_t kHeader[] =
    L"﻿; FloatBar settings. The Settings window writes this file; hand edits\r\n"
    L"; are read on the next start. Sizes are in pixels at 100% scaling.\r\n"
    L"; [Monitors] maps a monitor (e.g. \\\\.\\DISPLAY2) to normal or hidden.\r\n\r\n";

constexpr const wchar_t* kModeNames[] = {L"islands", L"bar"};
constexpr const wchar_t* kTrayNames[] = {L"show", L"hover", L"hide"};
constexpr const wchar_t* kBackgroundNames[] = {L"default", L"solid", L"gradient"};
constexpr const wchar_t* kDirectionNames[] = {L"horizontal", L"vertical", L"diagonalDown", L"diagonalUp", L"center"};
constexpr const wchar_t* kMonitorModeNames[] = {L"default", L"normal", L"hidden"};

class Ini {
public:
    explicit Ini(std::wstring path) : path_(std::move(path)) {}

    std::wstring Get(const wchar_t* key) const {
        wchar_t buf[256] = {};
        GetPrivateProfileStringW(kSection, key, L"", buf, static_cast<DWORD>(std::size(buf)), path_.c_str());
        return buf;
    }
    void Set(const wchar_t* key, const std::wstring& value) const {
        WritePrivateProfileStringW(kSection, key, value.c_str(), path_.c_str());
    }

    void Read(const wchar_t* key, bool& v) const {
        const std::wstring s = Get(key);
        if (s == L"true" || s == L"1") v = true;
        else if (s == L"false" || s == L"0") v = false;
    }
    void Read(const wchar_t* key, int& v, int lo, int hi) const {
        const std::wstring s = Get(key);
        wchar_t* end = nullptr;
        const long n = wcstol(s.c_str(), &end, 10);
        if (!s.empty() && *end == 0) v = std::clamp(static_cast<int>(n), lo, hi);
    }
    // "#RRGGBB" <-> COLORREF.
    void ReadColor(const wchar_t* key, unsigned long& v) const {
        const std::wstring s = Get(key);
        if (s.size() != 7 || s[0] != L'#') return;
        wchar_t* end = nullptr;
        const unsigned long rgb = wcstoul(s.c_str() + 1, &end, 16);
        if (*end == 0) v = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    }
    template <typename Enum, size_t N>
    void ReadEnum(const wchar_t* key, Enum& v, const wchar_t* const (&names)[N]) const {
        const std::wstring s = Get(key);
        for (size_t i = 0; i < N; ++i) {
            if (_wcsicmp(s.c_str(), names[i]) == 0) v = static_cast<Enum>(i);
        }
    }

    void Write(const wchar_t* key, bool v) const { Set(key, v ? L"true" : L"false"); }
    void Write(const wchar_t* key, int v) const { Set(key, std::to_wstring(v)); }
    void WriteColor(const wchar_t* key, unsigned long v) const {
        wchar_t buf[8];
        swprintf_s(buf, L"#%02X%02X%02X", GetRValue(v), GetGValue(v), GetBValue(v));
        Set(key, buf);
    }
    template <typename Enum, size_t N>
    void WriteEnum(const wchar_t* key, Enum v, const wchar_t* const (&names)[N]) const {
        Set(key, names[static_cast<size_t>(v)]);
    }

    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

std::wstring g_configDirOverride;

}  // namespace

void SetConfigDir(const std::wstring& dir) { g_configDirOverride = dir; }

std::wstring ConfigDir() {
    std::wstring dir = g_configDirOverride;
    if (dir.empty()) {
        PWSTR appData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData))) {
            dir = std::wstring(appData) + L"\\FloatBar";
        }
        CoTaskMemFree(appData);
    }
    if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ConfigPath() { return ConfigDir() + L"\\config.ini"; }

Config LoadConfig() {
    Config c;
    const Ini ini(ConfigPath());
    if (GetFileAttributesW(ini.path().c_str()) == INVALID_FILE_ATTRIBUTES) {
        SaveConfig(c);
        return c;
    }
    ini.Read(L"enabled", c.enabled);
    ini.ReadEnum(L"mode", c.mode, kModeNames);
    ini.ReadEnum(L"trayMode", c.trayMode, kTrayNames);
    ini.Read(L"showWidgets", c.showWidgets);
    ini.Read(L"cornerRadius", c.cornerRadius, 0, kMaxCornerRadius);
    ini.Read(L"marginTop", c.marginTop, kMinMargin, kMaxMargin);
    ini.Read(L"marginBottom", c.marginBottom, kMinMargin, kMaxMargin);
    ini.Read(L"islandPadding", c.islandPadding, 0, kMaxIslandPadding);
    ini.Read(L"fillOnMaximise", c.fillOnMaximise);
    ini.Read(L"fillOnTaskSwitch", c.fillOnTaskSwitch);
    ini.Read(L"autoHide", c.autoHide);
    ini.Read(L"animate", c.animate);
    ini.Read(L"animationSpeed", c.animationSpeed, kMinAnimationSpeed, kMaxAnimationSpeed);
    ini.Read(L"separateStart", c.separateStart);
    ini.Read(L"hideOverFullscreen", c.hideOverFullscreen);
    ini.Read(L"hideShowDesktop", c.hideShowDesktop);
    ini.ReadEnum(L"background", c.background, kBackgroundNames);
    ini.ReadColor(L"color1", c.color1);
    ini.ReadColor(L"color2", c.color2);
    ini.ReadEnum(L"gradientDirection", c.gradientDirection, kDirectionNames);
    ini.Read(L"opacity", c.opacity, 0, 100);
    ini.Read(L"borderWidth", c.borderWidth, 0, kMaxBorderWidth);
    ini.ReadColor(L"borderColor", c.borderColor);
    ini.Read(L"borderOpacity", c.borderOpacity, 0, 100);
    ini.Read(L"debugLogging", c.debugLogging);

    // [Monitors] is a list of "device=mode" lines.
    wchar_t section[4096] = {};
    GetPrivateProfileSectionW(kMonitorsSection, section, static_cast<DWORD>(std::size(section)), ini.path().c_str());
    for (const wchar_t* line = section; *line; line += wcslen(line) + 1) {
        const std::wstring entry = line;
        const size_t eq = entry.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring value = entry.substr(eq + 1);
        for (size_t i = 1; i < std::size(kMonitorModeNames); ++i) {
            if (_wcsicmp(value.c_str(), kMonitorModeNames[i]) == 0) c.monitorModes[entry.substr(0, eq)] = static_cast<MonitorMode>(i);
        }
    }
    return c;
}

bool SaveConfig(const Config& c) {
    const Ini ini(ConfigPath());
    if (GetFileAttributesW(ini.path().c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE file = CreateFileW(ini.path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        WriteFile(file, kHeader, static_cast<DWORD>(wcslen(kHeader) * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(file);
    }
    ini.Write(L"enabled", c.enabled);
    ini.WriteEnum(L"mode", c.mode, kModeNames);
    ini.WriteEnum(L"trayMode", c.trayMode, kTrayNames);
    ini.Write(L"showWidgets", c.showWidgets);
    ini.Write(L"cornerRadius", c.cornerRadius);
    ini.Write(L"marginTop", c.marginTop);
    ini.Write(L"marginBottom", c.marginBottom);
    ini.Write(L"islandPadding", c.islandPadding);
    ini.Write(L"fillOnMaximise", c.fillOnMaximise);
    ini.Write(L"fillOnTaskSwitch", c.fillOnTaskSwitch);
    ini.Write(L"autoHide", c.autoHide);
    ini.Write(L"animate", c.animate);
    ini.Write(L"animationSpeed", c.animationSpeed);
    ini.Write(L"separateStart", c.separateStart);
    ini.Write(L"hideOverFullscreen", c.hideOverFullscreen);
    ini.Write(L"hideShowDesktop", c.hideShowDesktop);
    ini.WriteEnum(L"background", c.background, kBackgroundNames);
    ini.WriteColor(L"color1", c.color1);
    ini.WriteColor(L"color2", c.color2);
    ini.WriteEnum(L"gradientDirection", c.gradientDirection, kDirectionNames);
    ini.Write(L"opacity", c.opacity);
    ini.Write(L"borderWidth", c.borderWidth);
    ini.WriteColor(L"borderColor", c.borderColor);
    ini.Write(L"borderOpacity", c.borderOpacity);
    ini.Write(L"debugLogging", c.debugLogging);

    // Replace the whole [Monitors] section: "key=value\0...\0\0".
    std::wstring monitors;
    for (const auto& [device, mode] : c.monitorModes) {
        monitors += device + L"=" + kMonitorModeNames[static_cast<size_t>(mode)];
        monitors += L'\0';
    }
    monitors += L'\0';
    WritePrivateProfileSectionW(kMonitorsSection, monitors.c_str(), ini.path().c_str());
    return true;
}

}  // namespace fb
