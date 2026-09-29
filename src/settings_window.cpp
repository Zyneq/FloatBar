#include "settings_window.h"

#include "app.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <iterator>
#include <string>

namespace fb::settings {
namespace {

constexpr wchar_t kClassName[] = L"FloatBarSettings";

enum ControlId : int {
    kIdHeader = 100,
    kIdEnabled,
    // Layout column
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
    kIdAnimate,
    kIdHideFullscreen,
    kIdHideShowDesktop,
    kIdAutostart,
    kIdDebugLogging,
    // Shape column
    kIdSecShape,
    kIdRadiusLabel,
    kIdRadius,
    kIdTopLabel,
    kIdTop,
    kIdBottomLabel,
    kIdBottom,
    kIdPaddingLabel,
    kIdPadding,
    kIdSecMonitors,
    kIdMonitorLabel,
    kIdMonitor,
    kIdMonitorModeLabel,
    kIdMonitorMode,
    // Appearance column
    kIdSecAppearance,
    kIdBackgroundLabel,
    kIdBackground,
    kIdColorsLabel,
    kIdColor1,
    kIdColor2,
    kIdDirectionLabel,
    kIdDirection,
    kIdOpacityLabel,
    kIdOpacity,
    kIdBorderWidthLabel,
    kIdBorderWidth,
    kIdBorderColorLabel,
    kIdBorderColor,
    kIdBorderOpacityLabel,
    kIdBorderOpacity,
    kIdAppearanceNote,
    // Bottom
    kIdSecStatus,
    kIdStatus,
    kIdHint,
    kIdDefaults,
    kIdFolder,
    kIdDebugReport,
    kIdExit,
    kIdClose,
};

struct Slider {
    int id;
    int labelId;
    const wchar_t* text;
    const wchar_t* unit;
    int min;
    int max;
    int Config::*field;
};

const Slider kSliders[] = {
    {kIdRadius, kIdRadiusLabel, L"Corner radius", L"px", 0, kMaxCornerRadius, &Config::cornerRadius},
    {kIdTop, kIdTopLabel, L"Top gap", L"px", kMinMargin, kMaxMargin, &Config::marginTop},
    {kIdBottom, kIdBottomLabel, L"Bottom gap", L"px", kMinMargin, kMaxMargin, &Config::marginBottom},
    {kIdPadding, kIdPaddingLabel, L"Side spacing", L"px", 0, kMaxIslandPadding, &Config::islandPadding},
    {kIdOpacity, kIdOpacityLabel, L"Opacity", L"%", 0, 100, &Config::opacity},
    {kIdBorderWidth, kIdBorderWidthLabel, L"Border width", L"px", 0, kMaxBorderWidth, &Config::borderWidth},
    {kIdBorderOpacity, kIdBorderOpacityLabel, L"Border opacity", L"%", 0, 100, &Config::borderOpacity},
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
    {kIdAnimate, &Config::animate},
    {kIdHideFullscreen, &Config::hideOverFullscreen},
    {kIdHideShowDesktop, &Config::hideShowDesktop},
};

struct Swatch {
    int id;
    unsigned long Config::*field;
};

const Swatch kSwatches[] = {
    {kIdColor1, &Config::color1},
    {kIdColor2, &Config::color2},
    {kIdBorderColor, &Config::borderColor},
};

HWND g_wnd = nullptr;
HFONT g_font = nullptr;
HFONT g_sectionFont = nullptr;
HFONT g_headerFont = nullptr;
COLORREF g_customColors[16] = {};
std::vector<std::wstring> g_monitorKeys;  // parallel to the monitor combo

HWND Item(int id) { return GetDlgItem(g_wnd, id); }

bool IsSection(int id) {
    return id == kIdSecLayout || id == kIdSecBehaviour || id == kIdSecShape || id == kIdSecMonitors || id == kIdSecAppearance ||
           id == kIdSecStatus;
}

bool IsGrayText(int id) { return id == kIdStatus || id == kIdHint || id == kIdAppearanceNote; }

HFONT CreateUiFont(UINT dpi, int weight, float scale) {
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    ncm.lfMessageFont.lfWeight = weight;
    ncm.lfMessageFont.lfHeight = static_cast<LONG>(ncm.lfMessageFont.lfHeight * scale);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

void DeleteFonts() {
    for (HFONT* f : {&g_font, &g_sectionFont, &g_headerFont}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

void UpdateFonts(UINT dpi) {
    DeleteFonts();
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
    std::wstring text = std::wstring(s.text) + L": " + std::to_wstring(value) + L" " + s.unit;
    if (value < 0) text += L" (corners past the edge)";
    SetWindowTextW(Item(s.labelId), text.c_str());
}

// Positions every control for `dpi`; returns the client size needed.
SIZE Layout(UINT dpi) {
    auto S = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
    auto place = [](int id, int x, int y, int w, int h) {
        SetWindowPos(Item(id), nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    auto slider = [&](int id, int x, int& y, int col) {
        for (const Slider& s : kSliders) {
            if (s.id != id) continue;
            place(s.labelId, x, y, col, S(20));
            place(s.id, x - S(6), y + S(20), col + S(12), S(30));
        }
        y += S(54);
    };
    auto comboRow = [&](int labelId, int comboId, int x, int& y, int col) {
        const int labelW = S(96);
        place(labelId, x, y + S(4), labelW, S(20));
        place(comboId, x + labelW, y, col - labelW, S(200));
        y += S(34);
    };

    const int pad = S(20), col = S(280), colGap = S(28);
    const int x1 = pad, x2 = pad + col + colGap, x3 = pad + 2 * (col + colGap);
    const int width = x3 + col + pad;

    int y = S(14);
    place(kIdHeader, pad, y, col, S(32));
    place(kIdEnabled, x2, y + S(6), col, S(24));
    y += S(48);
    const int top = y;

    // Column 1: layout + behaviour.
    place(kIdSecLayout, x1, y, col, S(20));
    y += S(26);
    comboRow(kIdModeLabel, kIdMode, x1, y, col);
    comboRow(kIdTrayLabel, kIdTray, x1, y, col);
    place(kIdWidgets, x1, y, col, S(24));
    y += S(38);
    place(kIdSecBehaviour, x1, y, col, S(20));
    y += S(26);
    for (int id : {kIdFillMaximise, kIdFillTaskSwitch, kIdAutoHide, kIdAnimate, kIdHideFullscreen, kIdHideShowDesktop, kIdAutostart,
                   kIdDebugLogging}) {
        place(id, x1, y, col, S(24));
        y += S(28);
    }
    int bottom = y;

    // Column 2: shape + per-monitor.
    y = top;
    place(kIdSecShape, x2, y, col, S(20));
    y += S(26);
    for (int id : {kIdRadius, kIdTop, kIdBottom, kIdPadding}) slider(id, x2, y, col);
    y += S(4);
    place(kIdSecMonitors, x2, y, col, S(20));
    y += S(26);
    comboRow(kIdMonitorLabel, kIdMonitor, x2, y, col);
    comboRow(kIdMonitorModeLabel, kIdMonitorMode, x2, y, col);
    bottom = std::max(bottom, y);

    // Column 3: appearance.
    y = top;
    place(kIdSecAppearance, x3, y, col, S(20));
    y += S(26);
    comboRow(kIdBackgroundLabel, kIdBackground, x3, y, col);
    {
        const int labelW = S(96), sw = S(64), sh = S(26);
        place(kIdColorsLabel, x3, y + S(4), labelW, S(20));
        place(kIdColor1, x3 + labelW, y, sw, sh);
        place(kIdColor2, x3 + labelW + sw + S(8), y, sw, sh);
        y += S(34);
    }
    comboRow(kIdDirectionLabel, kIdDirection, x3, y, col);
    slider(kIdOpacity, x3, y, col);
    slider(kIdBorderWidth, x3, y, col);
    {
        const int labelW = S(96);
        place(kIdBorderColorLabel, x3, y + S(4), labelW, S(20));
        place(kIdBorderColor, x3 + labelW, y, S(64), S(26));
        y += S(34);
    }
    slider(kIdBorderOpacity, x3, y, col);
    place(kIdAppearanceNote, x3, y, col, S(34));
    y += S(38);
    bottom = std::max(bottom, y);

    // Full width: status, hint, buttons.
    y = bottom + S(8);
    const int fullW = width - 2 * pad;
    place(kIdSecStatus, pad, y, fullW, S(20));
    y += S(24);
    place(kIdStatus, pad, y, fullW, S(72));
    y += S(78);
    place(kIdHint, pad, y, fullW, S(20));
    y += S(30);

    const int bh = S(30), gap = S(8);
    int bx = pad;
    for (auto [id, w] : {std::pair{kIdDefaults, S(96)}, {kIdFolder, S(116)}, {kIdDebugReport, S(140)}}) {
        place(id, bx, y, w, bh);
        bx += w + gap;
    }
    place(kIdClose, width - pad - S(96), y, S(96), bh);
    place(kIdExit, width - pad - S(96) - gap - S(116), y, S(116), bh);
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

void AddSlider(int id) {
    for (const Slider& s : kSliders) {
        if (s.id != id) continue;
        AddControl(WC_STATICW, s.text, SS_LEFT, s.labelId);
        HWND tb = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, s.id);
        SendMessageW(tb, TBM_SETRANGEMIN, FALSE, s.min);
        SendMessageW(tb, TBM_SETRANGEMAX, TRUE, s.max);
        SendMessageW(tb, TBM_SETPAGESIZE, 0, s.max - s.min > 20 ? 5 : 1);
    }
}

// Controls are created in visual (tab) order.
void CreateControls() {
    const DWORD check = BS_AUTOCHECKBOX | WS_TABSTOP;
    const DWORD swatch = BS_OWNERDRAW | WS_TABSTOP;
    AddControl(WC_STATICW, L"FloatBar", SS_LEFT, kIdHeader);
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
    AddControl(WC_BUTTONW, L"Smooth island animation", check, kIdAnimate);
    AddControl(WC_BUTTONW, L"Hide taskbar over fullscreen apps", check, kIdHideFullscreen);
    AddControl(WC_BUTTONW, L"Hide the Show Desktop sliver", check, kIdHideShowDesktop);
    AddControl(WC_BUTTONW, L"Start with Windows", check, kIdAutostart);
    AddControl(WC_BUTTONW, L"Verbose debug logging", check, kIdDebugLogging);

    AddControl(WC_STATICW, L"Shape", SS_LEFT, kIdSecShape);
    for (int id : {kIdRadius, kIdTop, kIdBottom, kIdPadding}) AddSlider(id);

    AddControl(WC_STATICW, L"Monitors", SS_LEFT, kIdSecMonitors);
    AddControl(WC_STATICW, L"Monitor", SS_LEFT, kIdMonitorLabel);
    AddCombo(kIdMonitor, {});
    AddControl(WC_STATICW, L"Taskbar", SS_LEFT, kIdMonitorModeLabel);
    AddCombo(kIdMonitorMode, {L"Use settings above", L"Normal Windows taskbar", L"Hidden"});

    AddControl(WC_STATICW, L"Appearance", SS_LEFT, kIdSecAppearance);
    AddControl(WC_STATICW, L"Background", SS_LEFT, kIdBackgroundLabel);
    AddCombo(kIdBackground, {L"Windows default", L"Solid colour", L"Gradient"});
    AddControl(WC_STATICW, L"Colours", SS_LEFT, kIdColorsLabel);
    AddControl(WC_BUTTONW, L"Colour 1", swatch, kIdColor1);
    AddControl(WC_BUTTONW, L"Colour 2", swatch, kIdColor2);
    AddControl(WC_STATICW, L"Direction", SS_LEFT, kIdDirectionLabel);
    AddCombo(kIdDirection, {L"Left → right", L"Top → bottom", L"Diagonal ↘", L"Diagonal ↗", L"Left → centre ← right"});
    AddSlider(kIdOpacity);
    AddSlider(kIdBorderWidth);
    AddControl(WC_STATICW, L"Border colour", SS_LEFT, kIdBorderColorLabel);
    AddControl(WC_BUTTONW, L"Border colour", swatch, kIdBorderColor);
    AddSlider(kIdBorderOpacity);
    AddControl(WC_STATICW, L"The border works everywhere. A custom background needs TranslucentTB with the taskbar set to Clear.",
               SS_LEFT, kIdAppearanceNote);

    AddControl(WC_STATICW, L"Status", SS_LEFT, kIdSecStatus);
    AddControl(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, kIdStatus);
    AddControl(WC_STATICW,
               L"Changes apply instantly and are saved automatically. Win+F2 toggles the tray island. "
               L"FloatBar keeps running in the notification area after you close this window.",
               SS_LEFT, kIdHint);
    AddControl(WC_BUTTONW, L"Defaults", BS_PUSHBUTTON | WS_TABSTOP, kIdDefaults);
    AddControl(WC_BUTTONW, L"Config folder", BS_PUSHBUTTON | WS_TABSTOP, kIdFolder);
    AddControl(WC_BUTTONW, L"Debug report…", BS_PUSHBUTTON | WS_TABSTOP, kIdDebugReport);
    AddControl(WC_BUTTONW, L"Exit FloatBar", BS_PUSHBUTTON | WS_TABSTOP, kIdExit);
    AddControl(WC_BUTTONW, L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdClose);
}

void UpdateEnabledStates(const Config& c) {
    const bool on = c.enabled;
    const bool islands = on && c.mode == LayoutMode::Islands;
    const bool custom = on && c.background != Background::Default;
    const bool gradient = custom && c.background == Background::Gradient;
    const bool border = on && c.borderWidth > 0;  // the border works with any background
    for (int id : {kIdMode, kIdFillMaximise, kIdFillTaskSwitch, kIdAutoHide, kIdAnimate, kIdHideFullscreen, kIdHideShowDesktop, kIdRadius,
                   kIdTop, kIdBottom, kIdPadding, kIdBackground, kIdBorderWidth, kIdMonitor, kIdMonitorMode})
        EnableWindow(Item(id), on);
    for (int id : {kIdTray, kIdWidgets}) EnableWindow(Item(id), islands);
    for (int id : {kIdColor1, kIdOpacity}) EnableWindow(Item(id), custom);
    for (int id : {kIdColor2, kIdDirection}) EnableWindow(Item(id), gradient);
    for (int id : {kIdBorderColor, kIdBorderOpacity}) EnableWindow(Item(id), border);
}

void ApplyFromControls(Config c) {
    for (const Check& ch : kChecks) c.*ch.field = Button_GetCheck(Item(ch.id)) == BST_CHECKED;
    c.mode = ComboBox_GetCurSel(Item(kIdMode)) == 1 ? LayoutMode::Bar : LayoutMode::Islands;
    c.trayMode = static_cast<TrayMode>(std::max(0, ComboBox_GetCurSel(Item(kIdTray))));
    c.background = static_cast<Background>(std::max(0, ComboBox_GetCurSel(Item(kIdBackground))));
    c.gradientDirection = static_cast<GradientDirection>(std::max(0, ComboBox_GetCurSel(Item(kIdDirection))));
    for (const Slider& s : kSliders) {
        c.*s.field = static_cast<int>(SendMessageW(Item(s.id), TBM_GETPOS, 0, 0));
        SetSliderLabel(s, c.*s.field);
    }
    UpdateEnabledStates(c);
    app::SetConfig(c);
    RefreshStatus();
}

void ApplyFromControls() { ApplyFromControls(app::GetConfig()); }

int SelectedMonitor() {
    const int index = ComboBox_GetCurSel(Item(kIdMonitor));
    return index >= 0 && index < static_cast<int>(g_monitorKeys.size()) ? index : -1;
}

void ShowSelectedMonitorMode() {
    const int index = SelectedMonitor();
    const Config c = app::GetConfig();
    const auto it = index < 0 ? c.monitorModes.end() : c.monitorModes.find(g_monitorKeys[index]);
    ComboBox_SetCurSel(Item(kIdMonitorMode), it == c.monitorModes.end() ? 0 : static_cast<int>(it->second));
}

void PopulateMonitors() {
    const int previous = std::max(0, ComboBox_GetCurSel(Item(kIdMonitor)));
    HWND combo = Item(kIdMonitor);
    ComboBox_ResetContent(combo);
    g_monitorKeys.clear();
    for (const auto& [key, label] : app::Monitors()) {
        g_monitorKeys.push_back(key);
        ComboBox_AddString(combo, label.c_str());
    }
    if (!g_monitorKeys.empty()) ComboBox_SetCurSel(combo, std::min(previous, static_cast<int>(g_monitorKeys.size()) - 1));
    ShowSelectedMonitorMode();
}

void ApplyMonitorMode() {
    const int index = SelectedMonitor();
    if (index < 0) return;
    Config c = app::GetConfig();
    const auto mode = static_cast<MonitorMode>(std::max(0, ComboBox_GetCurSel(Item(kIdMonitorMode))));
    if (mode == MonitorMode::Default) c.monitorModes.erase(g_monitorKeys[index]);
    else c.monitorModes[g_monitorKeys[index]] = mode;
    app::SetConfig(c);
    RefreshStatus();
}

void PickColor(const Swatch& sw) {
    Config c = app::GetConfig();
    CHOOSECOLORW cc = {sizeof(cc)};
    cc.hwndOwner = g_wnd;
    cc.rgbResult = c.*sw.field;
    cc.lpCustColors = g_customColors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;
    c.*sw.field = cc.rgbResult;
    InvalidateRect(Item(sw.id), nullptr, TRUE);
    ApplyFromControls(c);
}

void DrawSwatch(const DRAWITEMSTRUCT& di) {
    const Config c = app::GetConfig();
    COLORREF color = RGB(128, 128, 128);
    for (const Swatch& sw : kSwatches) {
        if (static_cast<int>(di.CtlID) == sw.id) color = c.*sw.field;
    }
    const bool disabled = (di.itemState & ODS_DISABLED) != 0;
    RECT r = di.rcItem;
    FillRect(di.hDC, &r, GetSysColorBrush(COLOR_WINDOW));
    HBRUSH frame = GetSysColorBrush(disabled ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT);
    InflateRect(&r, -1, -1);
    FrameRect(di.hDC, &r, frame);
    InflateRect(&r, -2, -2);
    if (disabled) {
        FillRect(di.hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
    } else {
        HBRUSH fill = CreateSolidBrush(color);
        FillRect(di.hDC, &r, fill);
        DeleteObject(fill);
    }
    if (di.itemState & ODS_FOCUS) {
        RECT f = di.rcItem;
        DrawFocusRect(di.hDC, &f);
    }
}

// Verbose logging records more detail (window classes and process names of the
// foreground window). Ask before turning it on.
bool ConfirmDebugLogging() {
    return MessageBoxW(g_wnd,
                       L"Verbose debug logging writes extra detail to log.txt in the FloatBar config folder:\n\n"
                       L"• every taskbar update and the button rectangles read\n"
                       L"• the window class and process name of the foreground window\n"
                       L"• which monitors have a maximised window\n\n"
                       L"Nothing is sent anywhere. The log stays on this PC until you delete it or choose to share it. "
                       L"You can turn this off at any time.\n\nEnable verbose debug logging?",
                       L"FloatBar – debug logging", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_HSCROLL:
            if (lParam) ApplyFromControls();
            return 0;

        case WM_DRAWITEM:
            DrawSwatch(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            switch (id) {
                case kIdMode:
                case kIdTray:
                case kIdBackground:
                case kIdDirection:
                    if (code == CBN_SELCHANGE) ApplyFromControls();
                    return 0;
                case kIdMonitor:
                    if (code == CBN_SELCHANGE) ShowSelectedMonitorMode();
                    return 0;
                case kIdMonitorMode:
                    if (code == CBN_SELCHANGE) ApplyMonitorMode();
                    return 0;
                case kIdAutostart:
                    if (code == BN_CLICKED) app::SetAutostart(Button_GetCheck(Item(kIdAutostart)) == BST_CHECKED);
                    return 0;
                case kIdDebugLogging:
                    if (code == BN_CLICKED) {
                        Config c = app::GetConfig();
                        const bool want = Button_GetCheck(Item(kIdDebugLogging)) == BST_CHECKED;
                        c.debugLogging = want && ConfirmDebugLogging();
                        Button_SetCheck(Item(kIdDebugLogging), c.debugLogging ? BST_CHECKED : BST_UNCHECKED);
                        app::SetConfig(c);
                    }
                    return 0;
                case kIdDefaults: {
                    Config defaults;
                    defaults.debugLogging = app::GetConfig().debugLogging;
                    app::SetConfig(defaults);
                    RefreshControls();
                    return 0;
                }
                case kIdFolder:
                    app::OpenConfigFolder();
                    return 0;
                case kIdDebugReport:
                    app::CreateDebugReport();
                    return 0;
                case kIdExit:
                    app::Exit();
                    return 0;
                case kIdClose:
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;
            }
            for (const Swatch& sw : kSwatches) {
                if (id == sw.id && code == BN_CLICKED) PickColor(sw);
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
            SetTextColor(dc, GetSysColor(IsGrayText(id) ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_DPICHANGED:
            UpdateFonts(HIWORD(wParam));
            ResizeToLayout(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;

        case WM_DESTROY:
            g_wnd = nullptr;
            DeleteFonts();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void Show(HINSTANCE instance, HICON smallIcon, HICON largeIcon) {
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

    g_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, L"FloatBar Settings",
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
    const Config c = app::GetConfig();
    for (const Check& ch : kChecks) Button_SetCheck(Item(ch.id), c.*ch.field ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(Item(kIdAutostart), app::IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(Item(kIdDebugLogging), c.debugLogging ? BST_CHECKED : BST_UNCHECKED);
    ComboBox_SetCurSel(Item(kIdMode), c.mode == LayoutMode::Bar ? 1 : 0);
    ComboBox_SetCurSel(Item(kIdTray), static_cast<int>(c.trayMode));
    ComboBox_SetCurSel(Item(kIdBackground), static_cast<int>(c.background));
    ComboBox_SetCurSel(Item(kIdDirection), static_cast<int>(c.gradientDirection));
    for (const Slider& s : kSliders) {
        SendMessageW(Item(s.id), TBM_SETPOS, TRUE, c.*s.field);
        SetSliderLabel(s, c.*s.field);
    }
    for (const Swatch& sw : kSwatches) InvalidateRect(Item(sw.id), nullptr, TRUE);
    PopulateMonitors();
    UpdateEnabledStates(c);
    RefreshStatus();
}

void RefreshStatus() {
    if (!g_wnd) return;
    const std::wstring status = app::Status();
    wchar_t current[2048] = {};
    GetWindowTextW(Item(kIdStatus), current, static_cast<int>(std::size(current)));
    if (status != current) SetWindowTextW(Item(kIdStatus), status.c_str());
}

HWND Window() { return g_wnd; }

}  // namespace fb::settings
