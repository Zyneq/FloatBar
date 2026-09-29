#include "core/bounds.h"

#include <algorithm>

#include "common/uia_util.h"
#include "core/match_rules.h"

using Microsoft::WRL::ComPtr;

namespace ib {
namespace {

bool IsEmptyRect(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

bool Contains(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.right <= outer.right && inner.top >= outer.top && inner.bottom <= outer.bottom;
}

bool Overlaps(const RECT& a, const RECT& b) {
    RECT overlap;
    return IntersectRect(&overlap, &a, &b) != FALSE;
}

HRESULT MakeStringCondition(IUIAutomation* uia, PROPERTYID property, const wchar_t* value, IUIAutomationCondition** out) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(value);
    const HRESULT hr = uia->CreatePropertyCondition(property, v, out);
    VariantClear(&v);
    return hr;
}

HRESULT MakeIntCondition(IUIAutomation* uia, PROPERTYID property, int value, IUIAutomationCondition** out) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = value;
    return uia->CreatePropertyCondition(property, v, out);
}

// Returns false for elements that are hidden or have no size.
bool UsableRect(IUIAutomationElement* e, RECT& rect) {
    BOOL offscreen = FALSE;
    e->get_CachedIsOffscreen(&offscreen);
    if (offscreen) return false;
    if (FAILED(e->get_CachedBoundingRectangle(&rect))) return false;
    return !IsEmptyRect(rect);
}

std::wstring CachedString(IUIAutomationElement* e, HRESULT (STDMETHODCALLTYPE IUIAutomationElement::*getter)(BSTR*)) {
    BSTR b = nullptr;
    if (FAILED((e->*getter)(&b))) return {};
    return TakeBstr(b);
}

struct Cluster {
    RECT rect{};
    int count = 0;
    bool hasStart = false;
};

}  // namespace

HRESULT BoundsReader::Init() {
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia_));
    if (FAILED(hr)) hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia_));
    if (FAILED(hr)) return hr;

    // Never hang the app for long if explorer is busy or restarting.
    ComPtr<IUIAutomation2> uia2;
    if (SUCCEEDED(uia_.As(&uia2))) {
        uia2->put_ConnectionTimeout(2000);
        uia2->put_TransactionTimeout(2000);
    }

    if (FAILED(hr = MakeStringCondition(uia_.Get(), UIA_AutomationIdPropertyId, rules::kAppSideRootAutomationId, &appSideRootCond_))) return hr;
    if (FAILED(hr = MakeStringCondition(uia_.Get(), UIA_AutomationIdPropertyId, rules::kTrayButtonAutomationId, &trayButtonCond_))) return hr;
    if (FAILED(hr = MakeIntCondition(uia_.Get(), UIA_ControlTypePropertyId, UIA_ButtonControlTypeId, &buttonCond_))) return hr;

    if (FAILED(hr = uia_->CreateCacheRequest(&cache_))) return hr;
    cache_->AddProperty(UIA_ClassNamePropertyId);
    cache_->AddProperty(UIA_AutomationIdPropertyId);
    cache_->AddProperty(UIA_BoundingRectanglePropertyId);
    cache_->AddProperty(UIA_IsOffscreenPropertyId);
    return S_OK;
}

BoundsResult BoundsReader::Compute(HWND taskbar) const {
    BoundsResult result;
    if (!uia_) {
        result.error = L"UI Automation not initialized";
        return result;
    }

    RECT wr = {};
    if (!GetWindowRect(taskbar, &wr) || IsEmptyRect(wr)) {
        result.error = L"taskbar window has no size";
        return result;
    }
    if (!IsWindowVisible(taskbar)) {
        result.error = L"taskbar is hidden";
        return result;
    }
    UINT dpi = GetDpiForWindow(taskbar);
    if (!dpi) dpi = 96;

    ComPtr<IUIAutomationElement> root;
    HRESULT hr = uia_->ElementFromHandle(taskbar, &root);
    if (FAILED(hr) || !root) {
        result.error = L"ElementFromHandle failed";
        return result;
    }

    // --- App side: every visible button under the frame, clustered by gaps ---
    ComPtr<IUIAutomationElement> frame;
    hr = root->FindFirst(TreeScope_Descendants, appSideRootCond_.Get(), &frame);
    if (FAILED(hr) || !frame) {
        result.error = L"taskbar frame not found";
        return result;
    }

    struct Button {
        RECT rect;
        bool start;
    };
    std::vector<Button> buttons;
    ComPtr<IUIAutomationElementArray> found;
    if (SUCCEEDED(frame->FindAllBuildCache(TreeScope_Descendants, buttonCond_.Get(), cache_.Get(), &found)) && found) {
        int length = 0;
        found->get_Length(&length);
        for (int i = 0; i < length; ++i) {
            ComPtr<IUIAutomationElement> e;
            RECT r;
            if (FAILED(found->GetElement(i, &e)) || !e || !UsableRect(e.Get(), r)) continue;
            buttons.push_back({r, CachedString(e.Get(), &IUIAutomationElement::get_CachedAutomationId) == rules::kStartButtonAutomationId});
        }
    }
    if (buttons.empty()) {
        result.error = L"no app buttons found";
        return result;
    }

    std::sort(buttons.begin(), buttons.end(), [](const Button& a, const Button& b) { return a.rect.left < b.rect.left; });
    const LONG gap = MulDiv(rules::kClusterGapLogicalPx, static_cast<int>(dpi), 96);
    std::vector<Cluster> clusters;
    for (const Button& b : buttons) {
        if (clusters.empty() || b.rect.left - clusters.back().rect.right > gap) {
            clusters.push_back({b.rect, 0, false});
        } else {
            UnionRect(&clusters.back().rect, &clusters.back().rect, &b.rect);
        }
        clusters.back().count++;
        clusters.back().hasStart |= b.start;
    }
    auto main = std::find_if(clusters.begin(), clusters.end(), [](const Cluster& c) { return c.hasStart; });
    if (main == clusters.end()) {
        main = std::max_element(clusters.begin(), clusters.end(), [](const Cluster& a, const Cluster& b) {
            return (a.rect.right - a.rect.left) < (b.rect.right - b.rect.left);
        });
    }

    Islands islands;
    islands.app = main->rect;
    islands.appCount = main->count;

    // --- Tray ---
    ComPtr<IUIAutomationElementArray> trayButtons;
    if (SUCCEEDED(root->FindAllBuildCache(TreeScope_Descendants, trayButtonCond_.Get(), cache_.Get(), &trayButtons)) && trayButtons) {
        int length = 0;
        trayButtons->get_Length(&length);
        for (int i = 0; i < length; ++i) {
            ComPtr<IUIAutomationElement> e;
            RECT r;
            if (FAILED(trayButtons->GetElement(i, &e)) || !e || !UsableRect(e.Get(), r)) continue;
            const std::wstring cls = CachedString(e.Get(), &IUIAutomationElement::get_CachedClassName);
            if (cls == rules::kShowDesktopClass) {
                islands.hasShowDesktop = true;
                islands.showDesktop = r;
                continue;
            }
            if (!rules::IsTrayIslandMember(cls)) continue;
            if (islands.trayCount++ == 0) islands.tray = r;
            else UnionRect(&islands.tray, &islands.tray, &r);
        }
    }
    islands.hasTray = islands.trayCount > 0;

    // --- Sanity checks: reject anything that does not look like a real layout ---
    RECT bounds = wr;
    InflateRect(&bounds, 2, 2);
    if (!Contains(bounds, islands.app)) {
        result.error = L"app island outside taskbar " + FormatRect(islands.app);
        return result;
    }
    if (islands.hasTray) {
        if (!Contains(bounds, islands.tray)) {
            result.error = L"tray island outside taskbar " + FormatRect(islands.tray);
            return result;
        }
        if (Overlaps(islands.app, islands.tray)) {
            result.error = L"app and tray islands overlap";
            return result;
        }
    }
    for (const Cluster& c : clusters) {
        if (&c == &*main || !Contains(bounds, c.rect)) continue;
        if (islands.hasTray && Overlaps(c.rect, islands.tray)) continue;
        islands.extras.push_back(c.rect);
    }

    result.islands = islands;
    return result;
}

}  // namespace ib
