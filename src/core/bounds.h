#pragma once

#include <windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

namespace ib {

// Island rectangles in physical screen coordinates.
struct Islands {
    RECT app{};                 // the button cluster that holds Start
    int appCount = 0;
    std::vector<RECT> extras;   // other app-side clusters, e.g. Widgets when centered
    bool hasTray = false;
    RECT tray{};
    int trayCount = 0;
};

struct BoundsResult {
    std::optional<Islands> islands;  // empty when UIA returned nothing usable
    std::wstring error;
};

// Reads island bounds from a taskbar window through UI Automation.
// Must be used on the thread (MTA) that called Init().
class BoundsReader {
public:
    HRESULT Init();
    BoundsResult Compute(HWND taskbar) const;

private:
    Microsoft::WRL::ComPtr<IUIAutomation> uia_;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> appSideRootCond_;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> buttonCond_;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> trayButtonCond_;
    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cache_;
};

}  // namespace ib
