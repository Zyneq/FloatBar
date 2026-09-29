#include "islandbar/settings_window.h"

#include <commctrl.h>
#include <windowsx.h>

#include <iterator>
#include <string>

namespace ib::settings {
namespace {

constexpr wchar_t kClassName[] = L"IslandBarSettings";

enum ControlId : int {
    kIdHeader = 100,
    kIdEnabled,
    kIdSecLayout,
    kIdModeLabel,
    kIdMode,
    kIdTrayLabel,
    kIdTray,
    kIdWidgets,
    kIdSecBehaviour,
    kIdFillMaximise,
    kIdFillTaskSwitch,
    kIdAutoHide,
    kIdAutostart,
    kIdSecShape,
    kIdRadiusLabel,
    kIdRadius,
    kIdTopLabel,
    kIdTop,
    kIdBottomLabel,
    kIdBottom,
    kIdPaddingLabel,
    kIdPadding,
    kIdSecStatus,
    kIdStatus,
    kIdHint,
    kIdDefaults,
    kIdFolder,
    kIdExit,
    kIdClose,
};

struct Slider {
    int id;
    int labelId;
    const wchar_t* text;
    int min;
    int max;
    int Config::*field;
};

const Slider kSliders[] = {
    {kIdRadius, kIdRadiusLabel, L"Corner radius", 0, kMaxCornerRadius, &Config::cornerRadius},
    {kIdTop, kIdTopLabel, L"Top gap", kMinMargin, kMaxMargin, &Config::marginTop},
    {kIdBottom, kIdBottomLabel, L"Bottom gap", kMinMargin, kMaxMargin, &Config::marginBottom},
    {kIdPadding, kIdPaddingLabel, L"Side spacing", 0, kMaxIslandPadding, &Config::islandPadding},
};

struct Check {
    int id;
    bool Config::*field;
};

const Check kChecks[] = {
    {kIdEnabled, &Config::enabled},
    {kIdWidgets, &Config::showWidgets},
    {kIdFillMaximise, &Config::fillOnMaximise},
    {kIdFillTaskSwitch, &Config::fillOnTaskSwitch},
    {kIdAutoHide, &Config::autoHide},
};

// Controls that only make sense while IslandBar is enabled.
const int kDependsOnEnabled[] = {kIdMode, kIdTray, kIdWidgets, kIdFillMaximise, kIdFillTaskSwitch, kIdAutoHide,
                                 kIdRadius, kIdTop, kIdBottom, kIdPadding};

HWND g_wnd = nullptr;
Host g_host;
HFONT g_font = nullptr;
HFONT g_sectionFont = nullptr;
HFONT g_headerFont = nullptr;

HWND Item(int id) { return GetDlgItem(g_wnd, id); }

bool IsSection(int id) { return id == kIdSecLayout || id == kIdSecBehaviour || id == kIdSecShape || id == kIdSecStatus; }

HFONT CreateUiFont(UINT dpi, int weight, float scale) {
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    ncm.lfMessageFont.lfWeight = weight;
    ncm.lfMessageFont.lfHeight = static_cast<LONG>(ncm.lfMessageFont.lfHeight * scale);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

void UpdateFonts(UINT dpi) {
    for (HFONT* f : {&g_font, &g_sectionFont, &g_headerFont}) {
        if (*f) DeleteObject(*f);
    }
    g_font = CreateUiFont(dpi, FW_NORMAL, 1.0f);
    g_sectionFont = CreateUiFont(dpi, FW_SEMIBOLD, 1.0f);
    g_headerFont = CreateUiFont(dpi, FW_SEMIBOLD, 1.5f);
    for (HWND child = GetWindow(g_wnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const int id = GetDlgCtrlID(child);
        HFONT font = id == kIdHeader ? g_headerFont : IsSection(id) ? g_sectionFont : g_font;
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void SetSliderLabel(const Slider& s, int value) {
    std::wstring text = std::wstring(s.text) + L": " + std::to_wstring(value) + L" px";
    if (value < 0) text += L" (corners past the edge)";
    SetWindowTextW(Item(s.labelId), text.c_str());
}

// Positions every control for `dpi`; returns the client size needed.
SIZE Layout(UINT dpi) {
    auto S = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
    auto place = [](int id, int x, int y, int w, int h) {
        SetWindowPos(Item(id), nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const int pad = S(20), col = S(290), colGap = S(28);
    const int width = pad * 2 + col * 2 + colGap;
    const int leftX = pad, rightX = pad + col + colGap;

    int y = S(14);
    place(kIdHeader, pad, y, col, S(32));
    place(kIdEnabled, rightX, y + S(6), col, S(24));
    y += S(48);
    const int columnsTop = y;

    // Left column: layout + behaviour.
    const int labelW = S(90), comboW = col - labelW;
    place(kIdSecLayout, leftX, y, col, S(20));
    y += S(26);
    place(kIdModeLabel, leftX, y + S(4), labelW, S(20));
    place(kIdMode, leftX + labelW, y, comboW, S(200));
    y += S(34);
    place(kIdTrayLabel, leftX, y + S(4), labelW, S(20));
    place(kIdTray, leftX + labelW, y, comboW, S(200));
    y += S(34);
    place(kIdWidgets, leftX, y, col, S(24));
    y += S(40);
    place(kIdSecBehaviour, leftX, y, col, S(20));
    y += S(26);
    for (int id : {kIdFillMaximise, kIdFillTaskSwitch, kIdAutoHide, kIdAutostart}) {
        place(id, leftX, y, col, S(24));
        y += S(28);
    }
    const int leftBottom = y;

    // Right column: shape sliders.
    y = columnsTop;
    place(kIdSecShape, rightX, y, col, S(20));
    y += S(26);
    for (const Slider& s : kSliders) {
        place(s.labelId, rightX, y, col, S(20));
        place(s.id, rightX - S(6), y + S(20), col + S(12), S(30));
        y += S(56);
    }

    y = std::max(leftBottom, y) + S(12);
    const int fullW = width - 2 * pad;
    place(kIdSecStatus, pad, y, fullW, S(20));
    y += S(24);
    place(kIdStatus, pad, y, fullW, S(54));
    y += S(60);
    place(kIdHint, pad, y, fullW, S(36));
    y += S(46);

    const int bh = S(30), bw = S(96), gap = S(8);
    place(kIdDefaults, pad, y, bw, bh);
    place(kIdFolder, pad + bw + gap, y, S(116), bh);
    place(kIdClose, width - pad - bw, y, bw, bh);
    place(kIdExit, width - pad - bw - gap - S(110), y, S(110), bh);
    y += bh + S(18);
    return {width, y};
}

void ResizeToLayout(UINT dpi, const RECT* suggested) {
    const SIZE client = Layout(dpi);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(g_wnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(g_wnd, GWL_EXSTYLE));
    RECT r = {0, 0, client.cx, client.cy};
    AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, dpi);
    const int w = r.right - r.left, h = r.bottom - r.top;

    if (suggested) {
        SetWindowPos(g_wnd, nullptr, suggested->left, suggested->top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    // Center on the monitor under the cursor.
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& work = mi.rcWork;
    SetWindowPos(g_wnd, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

HWND AddControl(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

void AddCombo(int id, std::initializer_list<const wchar_t*> items) {
    HWND combo = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
    for (const wchar_t* item : items) ComboBox_AddString(combo, item);
}

void CreateControls() {
    const DWORD check = BS_AUTOCHECKBOX | WS_TABSTOP;
    AddControl(WC_STATICW, L"IslandBar", SS_LEFT, kIdHeader);
    AddControl(WC_BUTTONW, L"Enabled", check, kIdEnabled);

    AddControl(WC_STATICW, L"Layout", SS_LEFT, kIdSecLayout);
    AddControl(WC_STATICW, L"Style", SS_LEFT, kIdModeLabel);
    AddCombo(kIdMode, {L"Split islands (dynamic)", L"Single rounded bar"});
    AddControl(WC_STATICW, L"Tray island", SS_LEFT, kIdTrayLabel);
    AddCombo(kIdTray, {L"Always shown", L"Shown on hover", L"Hidden"});
    AddControl(WC_BUTTONW, L"Show widgets island (separate buttons)", check, kIdWidgets);

    AddControl(WC_STATICW, L"Behaviour", SS_LEFT, kIdSecBehaviour);
    AddControl(WC_BUTTONW, L"Extend taskbar when a window is maximised", check, kIdFillMaximise);
    AddControl(WC_BUTTONW, L"Extend taskbar during Alt+Tab / Task View", check, kIdFillTaskSwitch);
    AddControl(WC_BUTTONW, L"Auto-hide (reveal on mouse over)", check, kIdAutoHide);
    AddControl(WC_BUTTONW, L"Start with Windows", check, kIdAutostart);

    AddControl(WC_STATICW, L"Shape", SS_LEFT, kIdSecShape);
    for (const Slider& s : kSliders) {
        AddControl(WC_STATICW, s.text, SS_LEFT, s.labelId);
        HWND tb = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, s.id);
        SendMessageW(tb, TBM_SETRANGEMIN, FALSE, s.min);
        SendMessageW(tb, TBM_SETRANGEMAX, TRUE, s.max);
        SendMessageW(tb, TBM_SETPAGESIZE, 0, 2);
    }

    AddControl(WC_STATICW, L"Status", SS_LEFT, kIdSecStatus);
    AddControl(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, kIdStatus);
    AddControl(WC_STATICW,
               L"Changes apply instantly and are saved automatically. Win+F2 toggles the tray island. "
               L"IslandBar keeps running in the notification area after you close this window.",
               SS_LEFT, kIdHint);
    AddControl(WC_BUTTONW, L"Defaults", BS_PUSHBUTTON | WS_TABSTOP, kIdDefaults);
    AddControl(WC_BUTTONW, L"Config folder", BS_PUSHBUTTON | WS_TABSTOP, kIdFolder);
    AddControl(WC_BUTTONW, L"Exit IslandBar", BS_PUSHBUTTON | WS_TABSTOP, kIdExit);
    AddControl(WC_BUTTONW, L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdClose);
}

void UpdateEnabledStates(const Config& c) {
    for (int id : kDependsOnEnabled) EnableWindow(Item(id), c.enabled);
    const bool islands = c.enabled && c.mode == LayoutMode::Islands;
    EnableWindow(Item(kIdTray), islands);
    EnableWindow(Item(kIdWidgets), islands);
}

void ApplyFromControls() {
    Config c = g_host.getConfig();
    for (const Check& ch : kChecks) c.*ch.field = Button_GetCheck(Item(ch.id)) == BST_CHECKED;
    c.mode = ComboBox_GetCurSel(Item(kIdMode)) == 1 ? LayoutMode::Bar : LayoutMode::Islands;
    switch (ComboBox_GetCurSel(Item(kIdTray))) {
        case 1: c.trayMode = TrayMode::Hover; break;
        case 2: c.trayMode = TrayMode::Hide; break;
        default: c.trayMode = TrayMode::Show; break;
    }
    for (const Slider& s : kSliders) {
        c.*s.field = static_cast<int>(SendMessageW(Item(s.id), TBM_GETPOS, 0, 0));
        SetSliderLabel(s, c.*s.field);
    }
    UpdateEnabledStates(c);
    g_host.setConfig(c);
    RefreshStatus();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_HSCROLL:
            if (lParam) ApplyFromControls();
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            switch (id) {
                case kIdMode:
                case kIdTray:
                    if (code == CBN_SELCHANGE) ApplyFromControls();
                    return 0;
                case kIdAutostart:
                    if (code == BN_CLICKED) g_host.setAutostart(Button_GetCheck(Item(kIdAutostart)) == BST_CHECKED);
                    return 0;
                case kIdDefaults: {
                    Config defaults;
                    defaults.pollIntervalMs = g_host.getConfig().pollIntervalMs;
                    g_host.setConfig(defaults);
                    RefreshControls();
                    return 0;
                }
                case kIdFolder:
                    g_host.openConfigFolder();
                    return 0;
                case kIdExit:
                    g_host.exitApp();
                    return 0;
                case kIdClose:
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;
            }
            for (const Check& ch : kChecks) {
                if (id == ch.id && code == BN_CLICKED) ApplyFromControls();
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(id == kIdStatus || id == kIdHint ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_DPICHANGED:
            UpdateFonts(HIWORD(wParam));
            ResizeToLayout(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;

        case WM_DESTROY:
            g_wnd = nullptr;
            for (HFONT* f : {&g_font, &g_sectionFont, &g_headerFont}) {
                if (*f) DeleteObject(*f);
                *f = nullptr;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void Show(HINSTANCE instance, const Host& host, HICON smallIcon, HICON largeIcon) {
    g_host = host;
    if (g_wnd) {
        ShowWindow(g_wnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_wnd);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = kClassName;
        wc.hIcon = largeIcon;
        wc.hIconSm = smallIcon;
        registered = RegisterClassExW(&wc) != 0;
    }

    g_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, L"IslandBar Settings",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 400,
                            600, nullptr, nullptr, instance, nullptr);
    if (!g_wnd) return;

    CreateControls();
    const UINT dpi = GetDpiForWindow(g_wnd);
    UpdateFonts(dpi);
    ResizeToLayout(dpi, nullptr);
    RefreshControls();
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_wnd);
}

void RefreshControls() {
    if (!g_wnd) return;
    const Config c = g_host.getConfig();
    for (const Check& ch : kChecks) Button_SetCheck(Item(ch.id), c.*ch.field ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(Item(kIdAutostart), g_host.getAutostart() ? BST_CHECKED : BST_UNCHECKED);
    ComboBox_SetCurSel(Item(kIdMode), c.mode == LayoutMode::Bar ? 1 : 0);
    ComboBox_SetCurSel(Item(kIdTray), c.trayMode == TrayMode::Hover ? 1 : c.trayMode == TrayMode::Hide ? 2 : 0);
    for (const Slider& s : kSliders) {
        SendMessageW(Item(s.id), TBM_SETPOS, TRUE, c.*s.field);
        SetSliderLabel(s, c.*s.field);
    }
    UpdateEnabledStates(c);
    RefreshStatus();
}

void RefreshStatus() {
    if (!g_wnd) return;
    const std::wstring status = g_host.getStatus();
    wchar_t current[1024] = {};
    GetWindowTextW(Item(kIdStatus), current, static_cast<int>(std::size(current)));
    if (status != current) SetWindowTextW(Item(kIdStatus), status.c_str());
}

HWND Window() { return g_wnd; }

}  // namespace ib::settings
