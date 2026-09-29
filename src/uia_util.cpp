#include "uia_util.h"

#include <cwchar>
#include <iterator>

namespace fb {

std::wstring TakeBstr(BSTR b) {
    std::wstring s = b ? std::wstring(b, SysStringLen(b)) : std::wstring();
    SysFreeString(b);
    return s;
}

const wchar_t* ControlTypeName(CONTROLTYPEID id) {
    static const wchar_t* const kNames[] = {
        L"Button",     L"Calendar",    L"CheckBox",   L"ComboBox",     L"Edit",
        L"Hyperlink",  L"Image",       L"ListItem",   L"List",         L"Menu",
        L"MenuBar",    L"MenuItem",    L"ProgressBar", L"RadioButton", L"ScrollBar",
        L"Slider",     L"Spinner",     L"StatusBar",  L"Tab",          L"TabItem",
        L"Text",       L"ToolBar",     L"ToolTip",    L"Tree",         L"TreeItem",
        L"Custom",     L"Group",       L"Thumb",      L"DataGrid",     L"DataItem",
        L"Document",   L"SplitButton", L"Window",     L"Pane",         L"Header",
        L"HeaderItem", L"Table",       L"TitleBar",   L"Separator",    L"SemanticZoom",
        L"AppBar",
    };
    const int index = id - UIA_ButtonControlTypeId;  // 50000
    if (index >= 0 && index < static_cast<int>(std::size(kNames))) return kNames[index];
    return L"Unknown";
}

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring FormatHwnd(HWND hwnd) {
    wchar_t buf[32];
    swprintf_s(buf, L"0x%08llX", static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(hwnd)));
    return buf;
}

std::wstring FormatRect(const RECT& r) {
    wchar_t buf[96];
    swprintf_s(buf, L"[%ld,%ld,%ld,%ld %ldx%ld]", r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
    return buf;
}

std::wstring WindowClass(HWND hwnd) {
    wchar_t buf[256] = {};
    GetClassNameW(hwnd, buf, static_cast<int>(std::size(buf)));
    return buf;
}

std::wstring Escape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
            case L'\r': out += L"\\r"; break;
            case L'\n': out += L"\\n"; break;
            case L'\t': out += L"\\t"; break;
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    swprintf_s(buf, L"\\x%02X", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace fb
