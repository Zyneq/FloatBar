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
    };
    std::ofstream file(ConfigPath(), std::ios::binary | std::ios::trunc);
    file << j.dump(2) << "\n";
    return file.good();
}

}  // namespace ib
