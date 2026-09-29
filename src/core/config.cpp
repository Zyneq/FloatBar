#include "core/config.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

#include "common/uia_util.h"

namespace ib {
namespace {

void Clamp(Config& c) {
    c.cornerRadius = std::clamp(c.cornerRadius, 0, kMaxCornerRadius);
    c.marginTop = std::clamp(c.marginTop, kMinMargin, kMaxMargin);
    c.marginBottom = std::clamp(c.marginBottom, kMinMargin, kMaxMargin);
    c.islandPadding = std::clamp(c.islandPadding, 0, kMaxIslandPadding);
    c.pollIntervalMs = std::clamp(c.pollIntervalMs, 250, 10000);
    c.opacity = std::clamp(c.opacity, 0, 100);
    c.borderWidth = std::clamp(c.borderWidth, 0, kMaxBorderWidth);
    c.borderOpacity = std::clamp(c.borderOpacity, 0, 100);
}

// Colours are stored as "#RRGGBB".
std::string ToHex(unsigned long colorref) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", GetRValue(colorref), GetGValue(colorref), GetBValue(colorref));
    return buf;
}

unsigned long ParseHex(const std::string& s, unsigned long fallback) {
    if (s.size() != 7 || s[0] != '#') return fallback;
    char* end = nullptr;
    const unsigned long rgb = strtoul(s.c_str() + 1, &end, 16);
    if (*end) return fallback;
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

constexpr const char* kBackgroundNames[] = {"default", "solid", "gradient"};
constexpr const char* kDirectionNames[] = {"horizontal", "vertical", "diagonalDown", "diagonalUp"};

template <typename Enum, size_t N>
Enum ParseEnum(const std::string& s, const char* const (&names)[N], Enum fallback) {
    for (size_t i = 0; i < N; ++i) {
        if (s == names[i]) return static_cast<Enum>(i);
    }
    return fallback;
}

const char* ToString(LayoutMode m) { return m == LayoutMode::Bar ? "bar" : "islands"; }
const char* ToString(TrayMode m) {
    switch (m) {
        case TrayMode::Hover: return "hover";
        case TrayMode::Hide: return "hide";
        default: return "show";
    }
}

LayoutMode ParseLayout(const std::string& s, LayoutMode fallback) {
    if (s == "bar") return LayoutMode::Bar;
    if (s == "islands") return LayoutMode::Islands;
    return fallback;
}

TrayMode ParseTray(const std::string& s, TrayMode fallback) {
    if (s == "show") return TrayMode::Show;
    if (s == "hover") return TrayMode::Hover;
    if (s == "hide") return TrayMode::Hide;
    return fallback;
}

}  // namespace

std::wstring ConfigDir() {
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData))) {
        dir = std::wstring(appData) + L"\\IslandBar";
    }
    CoTaskMemFree(appData);
    if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ConfigPath() { return ConfigDir() + L"\\config.json"; }

bool LoadConfig(Config& out, std::wstring& error) {
    out = Config{};
    const std::wstring path = ConfigPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SaveConfig(out);
        return true;
    }

    try {
        std::ifstream file(path, std::ios::binary);
        const nlohmann::json j = nlohmann::json::parse(file);
        Config c;
        c.enabled = j.value("enabled", c.enabled);
        c.mode = ParseLayout(j.value("mode", std::string()), c.mode);
        c.trayMode = ParseTray(j.value("trayMode", std::string()), c.trayMode);
        c.showWidgets = j.value("showWidgets", c.showWidgets);
        c.cornerRadius = j.value("cornerRadius", c.cornerRadius);
        c.marginTop = j.value("marginTop", c.marginTop);
        c.marginBottom = j.value("marginBottom", c.marginBottom);
        c.islandPadding = j.value("islandPadding", c.islandPadding);
        c.fillOnMaximise = j.value("fillOnMaximise", c.fillOnMaximise);
        c.fillOnTaskSwitch = j.value("fillOnTaskSwitch", c.fillOnTaskSwitch);
        c.autoHide = j.value("autoHide", c.autoHide);
        c.pollIntervalMs = j.value("pollIntervalMs", c.pollIntervalMs);
        c.background = ParseEnum(j.value("background", std::string()), kBackgroundNames, c.background);
        c.color1 = ParseHex(j.value("color1", std::string()), c.color1);
        c.color2 = ParseHex(j.value("color2", std::string()), c.color2);
        c.gradientDirection = ParseEnum(j.value("gradientDirection", std::string()), kDirectionNames, c.gradientDirection);
        c.opacity = j.value("opacity", c.opacity);
        c.borderWidth = j.value("borderWidth", c.borderWidth);
        c.borderColor = ParseHex(j.value("borderColor", std::string()), c.borderColor);
        c.borderOpacity = j.value("borderOpacity", c.borderOpacity);
        c.debugLogging = j.value("debugLogging", c.debugLogging);
        Clamp(c);
        out = c;
        return true;
    } catch (const std::exception& e) {
        error = L"config.json: " + FromUtf8(e.what());
        return false;
    }
}

bool SaveConfig(const Config& c) {
    const nlohmann::ordered_json j = {
        {"enabled", c.enabled},
        {"mode", ToString(c.mode)},
        {"trayMode", ToString(c.trayMode)},
        {"showWidgets", c.showWidgets},
        {"cornerRadius", c.cornerRadius},
        {"marginTop", c.marginTop},
        {"marginBottom", c.marginBottom},
        {"islandPadding", c.islandPadding},
        {"fillOnMaximise", c.fillOnMaximise},
        {"fillOnTaskSwitch", c.fillOnTaskSwitch},
        {"autoHide", c.autoHide},
        {"pollIntervalMs", c.pollIntervalMs},
        {"background", kBackgroundNames[static_cast<int>(c.background)]},
        {"color1", ToHex(c.color1)},
        {"color2", ToHex(c.color2)},
        {"gradientDirection", kDirectionNames[static_cast<int>(c.gradientDirection)]},
        {"opacity", c.opacity},
        {"borderWidth", c.borderWidth},
        {"borderColor", ToHex(c.borderColor)},
        {"borderOpacity", c.borderOpacity},
        {"debugLogging", c.debugLogging},
    };
    std::ofstream file(ConfigPath(), std::ios::binary | std::ios::trunc);
    file << j.dump(2) << "\n";
    return file.good();
}

}  // namespace ib
