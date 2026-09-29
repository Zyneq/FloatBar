#include "islandbar/backdrop.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ib {
namespace {

constexpr wchar_t kClassName[] = L"IslandBarBackdrop";

// Signed distance from a point to a rounded rectangle (negative inside).
float RoundRectDistance(float px, float py, const RECT& r, float radius) {
    const float hw = (r.right - r.left) / 2.0f, hh = (r.bottom - r.top) / 2.0f;
    radius = std::min({radius, hw, hh});
    const float qx = std::fabs(px - (r.left + hw)) - (hw - radius);
    const float qy = std::fabs(py - (r.top + hh)) - (hh - radius);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

// Antialiased coverage of a pixel whose centre is at signed distance d.
float Coverage(float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); }

// Gradient position (0..1) of a point inside `r`.
float GradientT(float px, float py, const RECT& r, GradientDirection dir) {
    const float u = (px - r.left) / std::max(1L, r.right - r.left);
    const float v = (py - r.top) / std::max(1L, r.bottom - r.top);
    switch (dir) {
        case GradientDirection::Vertical: return v;
        case GradientDirection::DiagonalDown: return (u + v) / 2;
        case GradientDirection::DiagonalUp: return (u + 1 - v) / 2;
        default: return u;
    }
}

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

void EnsureClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    registered = RegisterClassExW(&wc) != 0;
}

}  // namespace

Backdrop::~Backdrop() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void Backdrop::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void Backdrop::Show(HWND taskbar, const RECT& windowRect, const std::vector<RECT>& shapes, int radius, const Config& config, UINT dpi) {
    if (!hwnd_) {
        EnsureClass();
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kClassName,
                                L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd_) return;
        key_.clear();
    }

    std::vector<LONG> key = {windowRect.left, windowRect.top, windowRect.right, windowRect.bottom, radius,
                             static_cast<LONG>(dpi), static_cast<LONG>(config.background), static_cast<LONG>(config.color1),
                             static_cast<LONG>(config.color2), static_cast<LONG>(config.gradientDirection), config.opacity,
                             config.borderWidth, static_cast<LONG>(config.borderColor), config.borderOpacity};
    for (const RECT& s : shapes) key.insert(key.end(), {s.left, s.top, s.right, s.bottom});
    if (key != key_) {
        Render(windowRect, shapes, radius, config, dpi);
        key_ = std::move(key);
    }

    // Directly below the taskbar. Inserting after a non-topmost taskbar (fullscreen
    // app) also drops the backdrop out of the topmost band, so it never covers games.
    if (GetWindow(taskbar, GW_HWNDNEXT) != hwnd_ || !IsWindowVisible(hwnd_)) {
        SetWindowPos(hwnd_, taskbar, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void Backdrop::Render(const RECT& windowRect, const std::vector<RECT>& shapes, int radius, const Config& config, UINT dpi) {
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    if (width <= 0 || height <= 0) return;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bitmap);

    const float fillAlpha = config.opacity / 100.0f;
    const float borderAlpha = config.borderOpacity / 100.0f;
    const float border = static_cast<float>(MulDiv(config.borderWidth, static_cast<int>(dpi), 96));
    const bool gradient = config.background == Background::Gradient;
    const float r1 = GetRValue(config.color1), g1 = GetGValue(config.color1), b1 = GetBValue(config.color1);
    const float r2 = GetRValue(config.color2), g2 = GetGValue(config.color2), b2 = GetBValue(config.color2);
    const float rb = GetRValue(config.borderColor), gb = GetGValue(config.borderColor), bb = GetBValue(config.borderColor);

    auto* pixels = static_cast<uint32_t*>(bits);
    std::fill(pixels, pixels + static_cast<size_t>(width) * height, 0u);
    for (const RECT& shape : shapes) {
        // Only visit the pixels this shape can touch.
        const int x0 = std::max(0, static_cast<int>(shape.left) - 1), x1 = std::min(width, static_cast<int>(shape.right) + 1);
        const int y0 = std::max(0, static_cast<int>(shape.top) - 1), y1 = std::min(height, static_cast<int>(shape.bottom) + 1);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float d = RoundRectDistance(px, py, shape, static_cast<float>(radius));
                const float outer = Coverage(d);
                if (outer <= 0) continue;
                // The border is the ring between the outline and `border` px inside it.
                const float inner = border > 0 ? Coverage(d + border) : outer;
                const float t = gradient ? GradientT(px, py, shape, config.gradientDirection) : 0.0f;
                const float fa = fillAlpha * inner;
                const float ba = borderAlpha * (outer - inner);
                const float a = fa + ba;
                if (a <= 0) continue;
                // Premultiplied BGRA.
                const float r = Lerp(r1, r2, t) * fa + rb * ba;
                const float g = Lerp(g1, g2, t) * fa + gb * ba;
                const float b = Lerp(b1, b2, t) * fa + bb * ba;
                uint32_t& p = pixels[static_cast<size_t>(y) * width + x];
                // Shapes never overlap in practice; keep the stronger pixel if they touch.
                if ((p >> 24) >= static_cast<uint32_t>(a * 255.0f)) continue;
                p = (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | (static_cast<uint32_t>(r + 0.5f) << 16) |
                    (static_cast<uint32_t>(g + 0.5f) << 8) | static_cast<uint32_t>(b + 0.5f);
            }
        }
    }

    POINT dst = {windowRect.left, windowRect.top};
    SIZE size = {width, height};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

}  // namespace ib
