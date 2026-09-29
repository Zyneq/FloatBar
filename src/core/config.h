#pragma once

#include <string>

namespace ib {

enum class LayoutMode {
    Islands,  // app island + tray island (+ widgets), sized to the actual buttons
    Bar,      // one rounded bar spanning the whole taskbar, inset by the margins
};

enum class TrayMode {
    Show,   // tray island always visible
    Hover,  // tray island only while the mouse is over the taskbar
    Hide,   // tray island never visible
};

// Values in logical pixels; scaled by the taskbar's DPI when applied.
struct Config {
    bool enabled = true;
    LayoutMode mode = LayoutMode::Islands;
    TrayMode trayMode = TrayMode::Show;
    bool showWidgets = true;
    int cornerRadius = 12;
    int marginTop = 6;     // negative pushes the top corners off the window edge
    int marginBottom = 6;  // negative pushes the bottom corners off the window edge
    int islandPadding = 6; // islands: space around the buttons; bar: inset from the screen edges
    bool fillOnMaximise = false;
    bool fillOnTaskSwitch = false;
    bool autoHide = false;
    int pollIntervalMs = 1000;
};

inline constexpr int kMinMargin = -30;
inline constexpr int kMaxMargin = 20;
inline constexpr int kMaxCornerRadius = 30;
inline constexpr int kMaxIslandPadding = 40;

// %APPDATA%\IslandBar (created if missing).
std::wstring ConfigDir();
std::wstring ConfigPath();

// Loads config.json, writing defaults if it does not exist. On a parse error
// returns false, fills `error` and leaves defaults in `out`.
bool LoadConfig(Config& out, std::wstring& error);
bool SaveConfig(const Config& config);

}  // namespace ib
