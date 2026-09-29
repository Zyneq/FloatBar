#include "bounds.h"

#include <algorithm>

#include "uia_util.h"
#include "match_rules.h"

using Microsoft::WRL::ComPtr;

namespace fb {
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

    if (FAILED(hr = uia_->get_RawViewWalker(&rawWalker_))) return hr;
    if (FAILED(hr = uia_->CreateCacheRequest(&cache_))) return hr;
    cache_->AddProperty(UIA_ClassNamePropertyId);
    cache_->AddProperty(UIA_AutomationIdPropertyId);
    cache_->AddProperty(UIA_BoundingRectanglePropertyId);
    cache_->AddProperty(UIA_IsOffscreenPropertyId);
    return S_OK;
}

bool BoundsReader::Locate(HWND taskbar, Handles& out, std::wstring& error) {
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia_->ElementFromHandle(taskbar, &root)) || !root) {
        error = L"ElementFromHandle failed";
        return false;
    }
    ComPtr<IUIAutomationElement> frame;
    if (FAILED(root->FindFirst(TreeScope_Descendants, appSideRootCond_.Get(), &frame)) || !frame) {
        error = L"taskbar frame not found";
        return false;
    }
    out.frame = frame;
    // Tray buttons are siblings of the frame; searching only those is much
    // cheaper than the whole tree. If a Windows build puts them elsewhere, fall
    // back to searching everything under the taskbar.
    ComPtr<IUIAutomationElement> parent;
    ComPtr<IUIAutomationElementArray> probe;
    int count = 0;
    if (SUCCEEDED(rawWalker_->GetParentElement(frame.Get(), &parent)) && parent &&
        SUCCEEDED(parent->FindAllBuildCache(TreeScope_Children, trayButtonCond_.Get(), cache_.Get(), &probe)) && probe) {
        probe->get_Length(&count);
    }
    if (count > 0) {
        out.trayParent = parent;
        out.trayScope = TreeScope_Children;
    } else {
        out.trayParent = root;
        out.trayScope = TreeScope_Descendants;
    }
    return true;
}

BoundsResult BoundsReader::Compute(HWND taskbar) {
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

    // Use the remembered elements; find them again once if they went stale.
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto it = handles_.find(taskbar);
        if (it == handles_.end()) {
            Handles fresh;
            if (!Locate(taskbar, fresh, result.error)) return result;
            it = handles_.emplace(taskbar, fresh).first;
        }
        if (Read(it->second, wr, dpi, result)) return result;
        handles_.erase(it);
        result = {};
    }
    result.error = L"taskbar elements unavailable";
    return result;
}

bool BoundsReader::Read(Handles& h, const RECT& wr, UINT dpi, BoundsResult& result) {
    // --- App side: every visible button under the frame, clustered by gaps ---
    struct Button {
        RECT rect;
        bool start;
        bool app;  // a pinned/running app (not Start, Search, Task View, ...)
    };
    std::vector<Button> buttons;
    ComPtr<IUIAutomationElementArray> found;
    if (FAILED(h.frame->FindAllBuildCache(TreeScope_Descendants, buttonCond_.Get(), cache_.Get(), &found)) || !found) {
        return false;  // stale frame element
    }
    int length = 0;
    found->get_Length(&length);
    for (int i = 0; i < length; ++i) {
        ComPtr<IUIAutomationElement> e;
        RECT r;
        if (FAILED(found->GetElement(i, &e)) || !e || !UsableRect(e.Get(), r)) continue;
        buttons.push_back({r, CachedString(e.Get(), &IUIAutomationElement::get_CachedAutomationId) == rules::kStartButtonAutomationId,
                           CachedString(e.Get(), &IUIAutomationElement::get_CachedClassName) == rules::kAppButtonClass});
    }
    if (buttons.empty()) {
        result.error = L"no app buttons found";
        return length > 0;  // an empty frame may just be stale: look it up again
    }

    std::sort(buttons.begin(), buttons.end(), [](const Button& a, const Button& b) { return a.rect.left < b.rect.left; });
    for (const Button& b : buttons) {
        if (b.app) h.widestApp = std::max(h.widestApp, b.rect.right - b.rect.left);
    }
    const LONG gap = std::max(static_cast<LONG>(MulDiv(rules::kClusterGapLogicalPx, static_cast<int>(dpi), 96)), 2 * h.widestApp);
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

    // Where the system buttons end and the app buttons begin, inside the Start cluster.
    const Button* previous = nullptr;
    for (const Button& b : buttons) {
        RECT overlap;
        if (!IntersectRect(&overlap, &b.rect, &main->rect)) continue;
        if (b.app && previous && !previous->app) {
            islands.hasSplit = true;
            islands.split = b.rect.left;
            break;
        }
        previous = &b;
    }

    // --- Tray ---
    ComPtr<IUIAutomationElementArray> trayButtons;
    if (FAILED(h.trayParent->FindAllBuildCache(h.trayScope, trayButtonCond_.Get(), cache_.Get(), &trayButtons))) return false;
    if (trayButtons) {
        int trayLength = 0;
        trayButtons->get_Length(&trayLength);
        for (int i = 0; i < trayLength; ++i) {
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
        return true;
    }
    if (islands.hasTray) {
        if (!Contains(bounds, islands.tray)) {
            result.error = L"tray island outside taskbar " + FormatRect(islands.tray);
            return true;
        }
        if (Overlaps(islands.app, islands.tray)) {
            result.error = L"app and tray islands overlap";
            return true;
        }
    }
    for (const Cluster& c : clusters) {
        if (&c == &*main || !Contains(bounds, c.rect)) continue;
        if (islands.hasTray && Overlaps(c.rect, islands.tray)) continue;
        islands.extras.push_back(c.rect);
    }

    result.islands = islands;
    return true;
}

}  // namespace fb
