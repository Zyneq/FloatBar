#pragma once

#include <windows.h>

// The Settings window. It reads and changes settings through app.h.
namespace fb::settings {

void Show(HINSTANCE instance, HICON smallIcon, HICON largeIcon);
void RefreshControls();  // re-read config and autostart into the controls
void RefreshStatus();
HWND Window();

}  // namespace fb::settings
