#pragma once

#include <windows.h>

#include <vector>

#include "config.h"

namespace fb {

// A click-through layered window kept directly below one taskbar in z-order.
// It draws antialiased island backgrounds; they only show through when the
// taskbar's own background is transparent (TranslucentTB set to Clear).
class Backdrop {
public:
    Backdrop() = default;
    Backdrop(const Backdrop&) = delete;
    Backdrop& operator=(const Backdrop&) = delete;
    ~Backdrop();

    // `shapes` are window-relative rectangles (as SpanToWindowRect returns them);
    // `dpi` scales the config's logical border width.
    void Show(HWND taskbar, const RECT& windowRect, const std::vector<RECT>& shapes, int radius, const Config& config, UINT dpi);
    void Hide();

private:
    void Render(const RECT& windowRect, const std::vector<RECT>& shapes, int radius, const Config& config, UINT dpi);

    HWND hwnd_ = nullptr;
    std::vector<LONG> key_;
};

}  // namespace fb
