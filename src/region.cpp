#include "region.h"

#include <algorithm>

namespace fb {
namespace {

constexpr int kMinSpanHeight = 8;

HRGN SpanRegion(const RECT& windowRect, const RECT& span, const SpanStyle& style) {
    const RECT r = SpanToWindowRect(windowRect, span, style);
    if (r.right <= r.left) return nullptr;

    // CreateRoundRectRgn takes the ellipse size (2 x radius) and excludes the right/bottom edge.
    const int diameter = std::min({style.radius * 2, static_cast<int>(r.right - r.left), static_cast<int>(r.bottom - r.top)});
    if (diameter <= 1) return CreateRectRgn(r.left, r.top, r.right, r.bottom);
    return CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, diameter, diameter);
}

bool SetRegion(HWND taskbar, HRGN region) {
    // On success the system owns the region; on failure we must free it.
    if (SetWindowRgn(taskbar, region, TRUE)) return true;
    DeleteObject(region);
    return false;
}

}  // namespace

RECT SpanToWindowRect(const RECT& windowRect, const RECT& span, const SpanStyle& style) {
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;

    // SetWindowRgn coordinates are relative to the window's top-left corner.
    const int left = std::max(0, static_cast<int>(span.left - windowRect.left));
    const int right = std::min(width, static_cast<int>(span.right - windowRect.left));
    int top = style.marginTop;
    int bottom = height - style.marginBottom;
    if (bottom - top < kMinSpanHeight) {
        top = (height - kMinSpanHeight) / 2;
        bottom = top + kMinSpanHeight;
    }
    return {left, top, right, bottom};
}

std::vector<HWND> FindTaskbars() {
    std::vector<HWND> result;
    if (HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr)) result.push_back(primary);
    HWND secondary = nullptr;
    while ((secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        result.push_back(secondary);
    }
    return result;
}

bool ApplySpans(HWND taskbar, const RECT& windowRect, const std::vector<RECT>& spans, const SpanStyle& style) {
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (!region) return false;
    for (const RECT& span : spans) {
        if (HRGN part = SpanRegion(windowRect, span, style)) {
            CombineRgn(region, region, part, RGN_OR);
            DeleteObject(part);
        }
    }
    return SetRegion(taskbar, region);
}

bool ApplyFullExcept(HWND taskbar, const RECT& windowRect, const RECT& exclude) {
    HRGN region = CreateRectRgn(0, 0, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top);
    HRGN hole = CreateRectRgn(exclude.left - windowRect.left, 0, exclude.right - windowRect.left, windowRect.bottom - windowRect.top);
    if (region && hole) CombineRgn(region, region, hole, RGN_DIFF);
    if (hole) DeleteObject(hole);
    return region && SetRegion(taskbar, region);
}

bool HideTaskbar(HWND taskbar) {
    HRGN empty = CreateRectRgn(0, 0, 0, 0);
    return empty && SetRegion(taskbar, empty);
}

void ClearRegion(HWND taskbar) { SetWindowRgn(taskbar, nullptr, TRUE); }

bool HasRegion(HWND taskbar) {
    RECT box;
    return GetWindowRgnBox(taskbar, &box) != ERROR;
}

void ClearAllTaskbars() {
    for (HWND hwnd : FindTaskbars()) ClearRegion(hwnd);
}

}  // namespace fb
