#include "app_icon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fb {
namespace {

// Antialiased coverage of pixel (px, py) by a rounded rectangle (signed distance field).
float Coverage(float px, float py, float l, float t, float r, float b, float radius) {
    const float cx = (l + r) / 2, cy = (t + b) / 2;
    const float qx = std::fabs(px - cx) - ((r - l) / 2 - radius);
    const float qy = std::fabs(py - cy) - ((b - t) / 2 - radius);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
    return std::clamp(0.5f - d, 0.0f, 1.0f);
}

uint32_t Premultiplied(float alpha, int r, int g, int b) {
    const auto a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
    return (a << 24) | (static_cast<uint32_t>(r * alpha) << 16) | (static_cast<uint32_t>(g * alpha) << 8) |
           static_cast<uint32_t>(b * alpha);
}

}  // namespace

HICON CreateIslandIcon(int size) {
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;

    // Layout on a 32x32 grid: a wide app island and a narrow tray island.
    const float s = size / 32.0f;
    auto* pixels = static_cast<uint32_t*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float app = Coverage(px, py, 1 * s, 9 * s, 21 * s, 23 * s, 5 * s);
            const float tray = Coverage(px, py, 23.5f * s, 9 * s, 31 * s, 23 * s, 4 * s);
            pixels[y * size + x] = app >= tray ? Premultiplied(app, 59, 130, 246) : Premultiplied(tray, 147, 197, 253);
        }
    }

    std::vector<BYTE> maskBits(static_cast<size_t>((size + 15) / 16 * 2 * size), 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

}  // namespace fb
