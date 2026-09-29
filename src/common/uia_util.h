#pragma once

#include <windows.h>
#include <UIAutomation.h>

#include <string>

namespace ib {

// RAII wrapper for CoInitializeEx / CoUninitialize.
class ComInit {
public:
    explicit ComInit(DWORD model = COINIT_MULTITHREADED) : hr_(CoInitializeEx(nullptr, model)) {}
    ~ComInit() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
    HRESULT hr() const { return hr_; }

private:
    HRESULT hr_;
};

// Takes ownership of a BSTR and frees it.
std::wstring TakeBstr(BSTR b);

// Readable name for a UIA_*ControlTypeId, or L"Unknown".
const wchar_t* ControlTypeName(CONTROLTYPEID id);

std::string Utf8(const std::wstring& s);
std::wstring FromUtf8(const std::string& s);
std::wstring FormatHwnd(HWND hwnd);
std::wstring FormatRect(const RECT& r);  // "[l,t,r,b WxH]"
std::wstring WindowClass(HWND hwnd);

// Escapes control characters and quotes so a value fits on one line.
std::wstring Escape(const std::wstring& s);

}  // namespace ib
