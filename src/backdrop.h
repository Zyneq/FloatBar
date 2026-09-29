#pragma once

#include <windows.h>

#include <vector>

#include "config.h"

namespace fb {

// A click-through layered window that draws antialiased island shapes next to
// one taskbar in z-order.
//   Fill:   directly below the taskbar. Only visible when the taskbar's own
//           background is transparent (TranslucentTB set to Clear).
//   Border: directly above the taskbar. Always visible, and it covers the
//           stepped edge of the pixel-exact clip.
class Backdrop {
public:
    enum class Layer { Fill, Border };

    explicit Backdrop(Layer layer) : layer_(layer) {}
    Backdrop(const Backdrop&) = delete;
    Backdrop& operator=(const Backdrop&) = delete;
    ~Backdrop();

    // `shapes` are window-relative rectangles (as SpanToWindowRect returns them);
    // `dpi` scales the config's logical border width.
    void Show(HWND taskbar, const RECT& windowRect, const std::vector<RECT>& shapes, int radius, const Config& config, UINT dpi);
    void Hide();

private:
    void Render(const RECT& windowRect, const std::vector<RECT>& shapes, int radius, const Config& config, UINT dpi);
    void Restack(HWND taskbar);

    Layer layer_;
    HWND hwnd_ = nullptr;
    std::vector<LONG> key_;
};

}  // namespace fb
