#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/config.h"

namespace ib::settings {

// How the settings window talks to the rest of the app.
struct Host {
    std::function<Config()> getConfig;
    std::function<void(const Config&)> setConfig;  // applies live and saves
    std::function<bool()> getAutostart;
    std::function<void(bool)> setAutostart;
    std::function<void()> openConfigFolder;
    std::function<void()> createDebugReport;       // asks for consent itself
    std::function<std::wstring()> getStatus;
    std::function<std::vector<std::pair<std::wstring, std::wstring>>()> getMonitors;  // {key, label}
    std::function<void()> exitApp;
};

void Show(HINSTANCE instance, const Host& host, HICON smallIcon, HICON largeIcon);
void RefreshControls();  // re-read config and autostart into the controls
void RefreshStatus();
HWND Window();

}  // namespace ib::settings
