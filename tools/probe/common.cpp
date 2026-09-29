#include "common.h"

#include <shobjidl.h>

#include <algorithm>

namespace probe {
namespace {

constexpr wchar_t kDummyClass[] = L"FloatBarProbeDummy";

double QpcPerMs() {
    static const double perMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart / 1000.0;
    }();
    return perMs;
}

std::wstring DummyTitle(int id, bool fullscreen) {
    return (fullscreen ? L"FloatBar probe fullscreen " : L"FloatBar probe ") + std::to_wstring(id);
}

LRESULT CALLBACK DummyProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

double QpcToMs(LONGLONG qpc) { return qpc / QpcPerMs(); }

double NowMs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return QpcToMs(c.QuadPart);
}

int Dist(const BYTE* a, const BYTE* b) { return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]); }

// ------------------------------------------------------------------ test windows

int RunDummy(int id, bool fullscreen) {
    // Its own AppUserModelID, so every dummy gets its own taskbar button.
    const std::wstring appId = L"FloatBar.Probe.Dummy" + std::to_wstring(id) + (fullscreen ? L".Fullscreen" : L"");
    SetCurrentProcessExplicitAppUserModelID(appId.c_str());
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DummyProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kDummyClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = fullscreen ? CreateSolidBrush(RGB(kBackdropBgra[2], kBackdropBgra[1], kBackdropBgra[0])) : GetSysColorBrush(COLOR_WINDOW);
    RegisterClassW(&wc);
    HWND hwnd = nullptr;
    if (fullscreen) {
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        const RECT& m = mi.rcMonitor;
        hwnd = CreateWindowExW(0, kDummyClass, DummyTitle(id, true).c_str(), WS_POPUP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                               nullptr, nullptr, wc.hInstance, nullptr);
    } else {
        // Cascaded, but never down to the taskbar: behind a clipped taskbar only
        // the test backdrop may show.
        const int slot = id % 16;
        hwnd = CreateWindowExW(0, kDummyClass, DummyTitle(id, false).c_str(), WS_OVERLAPPEDWINDOW, 260 + slot * 24, 160 + slot * 24, 520, 340,
                               nullptr, nullptr, wc.hInstance, nullptr);
    }
    // Not activated: opening test windows must not take the focus from the user.
    ShowWindow(hwnd, fullscreen ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

std::vector<HWND> FindDummies() {
    std::vector<HWND> out;
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, kDummyClass, nullptr)) != nullptr) out.push_back(h);
    return out;
}

HWND SpawnDummy(int id, bool fullscreen) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" dummy " + std::to_wstring(id) + (fullscreen ? L" fullscreen" : L"");
    // Fully detached: no console and no standard handles, so the caller's
    // output pipe (e.g. a PowerShell pipeline) isn't held open by the window.
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    const std::wstring title = DummyTitle(id, fullscreen);
    for (int i = 0; i < 300; ++i) {
        if (HWND h = FindWindowW(kDummyClass, title.c_str())) return h;
        Sleep(10);
    }
    return nullptr;
}

bool WaitGone(HWND hwnd, int timeoutMs) {
    for (int waited = 0; IsWindow(hwnd); waited += 10) {
        if (waited >= timeoutMs) return false;
        Sleep(10);
    }
    return true;
}

void CloseDummies() {
    const std::vector<HWND> dummies = FindDummies();
    for (HWND h : dummies) PostMessageW(h, WM_CLOSE, 0, 0);
    for (HWND h : dummies) WaitGone(h, 3000);
}

bool ForceForeground(HWND hwnd) {
    const DWORD me = GetCurrentThreadId();
    HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attach = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attach) AttachThreadInput(me, fgThread, FALSE);
    for (int i = 0; i < 50 && GetForegroundWindow() != hwnd; ++i) Sleep(10);
    return GetForegroundWindow() == hwnd;
}

// ------------------------------------------------------------------ capture

bool Capture::Init(HMONITOR monitor) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            if (desc.Monitor != monitor) continue;
            if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr,
                                         &context_)))
                return false;
            Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.As(&output1)) || FAILED(output1->DuplicateOutput(device_.Get(), &dupl_))) return false;
            origin_ = {desc.DesktopCoordinates.left, desc.DesktopCoordinates.top};
            return true;
        }
    }
    return false;
}

bool Capture::Next(UINT timeoutMs, const RECT& box, std::vector<BYTE>& pixels, LONGLONG& presentQpc, UINT& accumulated) {
    DXGI_OUTDUPL_FRAME_INFO info;
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    const HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        failed_ = true;
        lastError_ = hr;
        return false;
    }
    bool got = false;
    if (info.LastPresentTime.QuadPart != 0) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> frame;
        resource.As(&frame);
        const int w = box.right - box.left, h = box.bottom - box.top;
        if (!staging_) {
            D3D11_TEXTURE2D_DESC desc;
            frame->GetDesc(&desc);
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.SampleDesc = {1, 0};
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            device_->CreateTexture2D(&desc, nullptr, &staging_);
        }
        const D3D11_BOX src = {static_cast<UINT>(box.left - origin_.x), static_cast<UINT>(box.top - origin_.y), 0,
                               static_cast<UINT>(box.right - origin_.x), static_cast<UINT>(box.bottom - origin_.y), 1};
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, frame.Get(), 0, &src);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            pixels.resize(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                memcpy(&pixels[static_cast<size_t>(y) * w * 4], static_cast<BYTE*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, w * 4);
            }
            context_->Unmap(staging_.Get(), 0);
            presentQpc = info.LastPresentTime.QuadPart;
            accumulated = info.AccumulatedFrames;
            got = true;
        }
    }
    dupl_->ReleaseFrame();
    return got;
}

// ------------------------------------------------------------------ region

RowRegion ReadRegionRow(HWND hwnd, const RECT& wr, int y) {
    RowRegion out;
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    // A read can land inside SetWindowRgn's swap and see no region for an
    // instant (the pixels show it never reaches the screen); read again.
    int type = ERROR;
    for (int attempt = 0; attempt < 4 && (type = GetWindowRgn(hwnd, rgn)) == ERROR; ++attempt) SwitchToThread();
    if (type == ERROR) {
        out.runs.push_back({wr.left, wr.right});
    } else {
        out.clipped = true;
        const DWORD size = GetRegionData(rgn, 0, nullptr);
        std::vector<BYTE> buf(size);
        auto* data = reinterpret_cast<RGNDATA*>(buf.data());
        if (size && GetRegionData(rgn, size, data)) {
            const RECT* rects = reinterpret_cast<const RECT*>(data->Buffer);
            for (DWORD i = 0; i < data->rdh.nCount; ++i) {
                const RECT& rc = rects[i];
                if (y - wr.top < rc.top || y - wr.top >= rc.bottom) continue;
                const int l = rc.left + wr.left, r = rc.right + wr.left;
                if (!out.runs.empty() && out.runs.back().r >= l) out.runs.back().r = std::max(out.runs.back().r, r);
                else out.runs.push_back({l, r});
            }
        }
    }
    DeleteObject(rgn);
    return out;
}

}  // namespace probe
