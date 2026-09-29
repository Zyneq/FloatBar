#pragma once

#include <windows.h>

#include <vector>

namespace ib {

// Vertical shape shared by every rounded span, in physical pixels.
struct SpanStyle {
    int marginTop = 0;     // may be negative: pushes the top corners past the window edge
    int marginBottom = 0;  // may be negative: pushes the bottom corners past the window edge
    int radius = 0;
};

// The window-relative rectangle a span occupies (may extend past the window
// vertically when a margin is negative). Shared by the clip and the backdrop.
RECT SpanToWindowRect(const RECT& windowRect, const RECT& span, const SpanStyle& style);

// Primary taskbar (Shell_TrayWnd) first, then every Shell_SecondaryTrayWnd.
std::vector<HWND> FindTaskbars();

// Clips `taskbar` to one rounded rectangle per span. Only left/right of each span
// (screen coordinates) are used; the vertical extent comes from `style`.
bool ApplySpans(HWND taskbar, const RECT& windowRect, const std::vector<RECT>& spans, const SpanStyle& style);

// Clips the taskbar away entirely (auto-hide).
bool HideTaskbar(HWND taskbar);

void ClearRegion(HWND taskbar);
bool HasRegion(HWND taskbar);

// Removes any clip from every taskbar. Safe to call from a crash handler.
void ClearAllTaskbars();

}  // namespace ib
