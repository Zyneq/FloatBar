#pragma once

#include <string>
#include <vector>

#include "config.h"
#include "engine.h"

// App-level actions the Settings window calls. Implemented in main.cpp.
namespace fb::app {

Config GetConfig();
void SetConfig(const Config& config);  // applies live and saves
bool IsAutostartEnabled();
void SetAutostart(bool enable);
void OpenConfigFolder();
void CreateDebugReport();
std::wstring Status();
std::vector<MonitorEntry> Monitors();
void Exit();

}  // namespace fb::app
