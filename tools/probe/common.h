#pragma once

// Shared pieces of floatbar-probe: the clock, test windows, screen capture and
// region reading.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "motion.h"

namespace probe {

// FloatBar's own clock (QueryPerformanceCounter in ms), so probe timestamps line
// up with FloatBar's per-frame log lines; QpcToMs converts DXGI present times.
using fb::NowMs;
double QpcToMs(LONGLONG qpc);

int Dist(const BYTE* a, const BYTE* b);  // BGRA colour distance (sum of channel differences)

// Icon pixels: rows this far above and below the taskbar's centre row, differing
// from the taskbar background by more than this.
constexpr int kIconBandHalf = 7;
constexpr int kIconTolerance = 70;

// ---- test windows (separate processes, each with its own taskbar button)

// `fullscreen`: a borderless window covering the primary monitor, painted in
// kBackdropColor, that makes itself the foreground window.
int RunDummy(int id, bool fullscreen);  // the `dummy` command, in the child process
HWND SpawnDummy(int id, bool fullscreen = false);
std::vector<HWND> FindDummies();
void CloseDummies();                    // and waits until they are gone
bool WaitGone(HWND hwnd, int timeoutMs);
// Makes `hwnd` the foreground window although this console process isn't the
// foreground process (by briefly sharing input state with the one that is).
bool ForceForeground(HWND hwnd);

// What the backdrop and the fullscreen dummy are painted with: pure green, which
// no icon or taskbar material comes close to.
constexpr BYTE kBackdropBgra[4] = {0, 255, 0, 255};

// ---- capture (DXGI Desktop Duplication of one monitor)

class Capture {
public:
    bool Init(HMONITOR monitor);
    // Waits up to `timeoutMs` for a new frame and copies `box` (screen coords)
    // into `pixels` (BGRA, row after row). False on timeout or a mouse-only update.
    bool Next(UINT timeoutMs, const RECT& box, std::vector<BYTE>& pixels, LONGLONG& presentQpc, UINT& accumulated);
    bool failed() const { return failed_; }
    HRESULT lastError() const { return lastError_; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dupl_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    POINT origin_ = {};
    bool failed_ = false;
    HRESULT lastError_ = S_OK;
};

// ---- region

struct Interval {
    int l, r;  // screen x, [l, r)
    bool operator==(const Interval&) const = default;
};

// The window region of `hwnd` on screen row `y`, as screen-x intervals.
struct RowRegion {
    bool clipped = false;  // false: no region, the whole window shows
    std::vector<Interval> runs;
};
RowRegion ReadRegionRow(HWND hwnd, const RECT& wr, int y);

}  // namespace probe
