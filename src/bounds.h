#pragma once

#include <windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fb {

// Island rectangles in physical screen coordinates.
struct Islands {
    RECT app{};                 // the button cluster that holds Start
    int appCount = 0;
    std::vector<RECT> extras;  // other app-side clusters, e.g. Widgets when centered
    bool hasTray = false;
    RECT tray{};
    int trayCount = 0;
    bool hasShowDesktop = false;
    RECT showDesktop{};         // the sliver at the far edge
};

struct BoundsResult {
    std::optional<Islands> islands;  // empty when UIA returned nothing usable
    std::wstring error;
};

// Reads island bounds from a taskbar window through UI Automation.
// Must be used on the thread (MTA) that called Init(). It remembers the
// taskbar's button frame and the tray's parent element between reads, so a
// read is two cross-process calls instead of four; stale elements (explorer
// restarted) are found again automatically.
class BoundsReader {
public:
    HRESULT Init();
    BoundsResult Compute(HWND taskbar);

private:
    struct Handles {
        Microsoft::WRL::ComPtr<IUIAutomationElement> frame;       // TaskbarFrame
        Microsoft::WRL::ComPtr<IUIAutomationElement> trayParent;  // where tray buttons live
        TreeScope trayScope = TreeScope_Children;
        LONG widestApp = 0;  // widest app button seen (labels make them wider)
    };
    bool Locate(HWND taskbar, Handles& out, std::wstring& error);
    // Returns false if the cached elements went stale.
    bool Read(Handles& h, const RECT& wr, UINT dpi, BoundsResult& result);

    Microsoft::WRL::ComPtr<IUIAutomation> uia_;
    Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> rawWalker_;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> appSideRootCond_;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> buttonCond_;
    Microsoft::WRL::ComPtr<IUIAutomationCondition> trayButtonCond_;
    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cache_;
    std::map<HWND, Handles> handles_;  // per taskbar
};

}  // namespace fb
