#pragma once

#include <map>
#include <string>

namespace fb {

enum class LayoutMode {
    Islands,  // app island + tray island (+ widgets), sized to the actual buttons
    Bar,      // one rounded bar spanning the whole taskbar, inset by the margins
};

enum class TrayMode {
    Show,   // tray island always visible
    Hover,  // tray island only while the mouse is over the taskbar
    Hide,   // tray island never visible
};

enum class Background {
    Default,   // whatever Windows draws; FloatBar only clips
    Solid,     // FloatBar draws antialiased islands (needs TranslucentTB set to Clear)
    Gradient,
};

// Center: colour 1 at the left and right edges, colour 2 in the middle.
enum class GradientDirection { Horizontal, Vertical, DiagonalDown, DiagonalUp, Center };

// Per-monitor override of the global settings.
enum class MonitorMode {
    Default,  // follow the global settings
    Normal,   // leave this monitor's taskbar as Windows draws it
    Hidden,   // hide this monitor's taskbar completely
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
    bool animate = true;             // move island edges smoothly instead of jumping
    int animationSpeed = 100;        // percent: 200 = twice as fast, 50 = half speed
    bool hideOverFullscreen = true;  // hide while a fullscreen/borderless app covers the monitor
    bool hideShowDesktop = false;    // clip the Show Desktop sliver in full-width and bar modes too
    std::map<std::wstring, MonitorMode> monitorModes;  // key: monitor device name, e.g. \\.\DISPLAY2

    // Appearance (colours are COLORREF, 0x00BBGGRR).
    Background background = Background::Default;
    unsigned long color1 = 0x00202020;
    unsigned long color2 = 0x00F6823B;
    GradientDirection gradientDirection = GradientDirection::Horizontal;
    int opacity = 85;        // percent
    int borderWidth = 0;     // drawn above the taskbar; works without TranslucentTB
    unsigned long borderColor = 0x00FFFFFF;
    int borderOpacity = 15;  // percent

    // Opt-in, extra detail in log.txt (enabled only after the user agrees).
    bool debugLogging = false;
};

inline constexpr int kMaxBorderWidth = 4;
inline constexpr int kMinAnimationSpeed = 25;
inline constexpr int kMaxAnimationSpeed = 400;
inline constexpr int kMinMargin = -30;
inline constexpr int kMaxMargin = 20;
inline constexpr int kMaxCornerRadius = 30;
inline constexpr int kMaxIslandPadding = 40;

// %APPDATA%\FloatBar (created if missing), or the --config-dir given on the
// command line (used by the automated tests so they never touch real settings).
std::wstring ConfigDir();
void SetConfigDir(const std::wstring& dir);
std::wstring ConfigPath();

// %APPDATA%\FloatBar\config.ini, read and written with the Win32 profile API.
// Missing or invalid values fall back to the defaults above; a missing file is
// created with defaults.
Config LoadConfig();
bool SaveConfig(const Config& config);

}  // namespace fb
