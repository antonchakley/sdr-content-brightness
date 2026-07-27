/*
 * SdrContentBrightness.cpp
 *
 * A Windows system tray application in C++ (Win32 + GDI+ class API),
 * built for MSVC (cl) only - no MinGW needed. Styled to match modern
 * Windows 11 look & feel:
 *
 *   - Follows the system Light / Dark theme automatically (reads the same
 *     registry value Windows itself uses, and updates live if you change
 *     it in Settings while the app is running).
 *   - Uses the current Windows accent color for the slider fill/thumb
 *     (via DwmGetColorizationColor), also updates live.
 *   - Rounded corners + drop shadow, like a Windows 11 flyout/popup.
 *   - Fully custom-drawn "pill" slider control, rendered with GDI+
 *     anti-aliasing so the track and thumb are smooth, not pixelated.
 *   - A brightness-style sun icon is drawn at runtime with GDI+ (no
 *     bundled .ico resource) and tinted to match the taskbar's
 *     light/dark setting.
 *
 * Left-click the tray icon -> flyout with a slider appears above it.
 * Click elsewhere / press Esc -> flyout closes.
 * Right-click the tray icon -> Exit menu.
 *
 * BUILD (MSVC, "x64 Native Tools Command Prompt for VS"):
 *  cl SdrContentBrightness.cpp SdrContentBrightnessIcon.cpp
 *  user32.lib gdi32.lib shell32.lib dwmapi.lib gdiplus.lib advapi32.lib  /link /SUBSYSTEM:WINDOWS
 */

#define NOMINMAX               /* avoid clashes between <windows.h> min/max macros and GDI+ */
#define _WIN32_WINNT 0x0A00
#define WINVER       0x0A00

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>

#include "SdrContentBrightnessIcon.h"

using namespace Gdiplus;

#define WM_TRAYICON      (WM_APP + 1)
#define ID_TRAY_ICON     1001
#define ID_MENU_EXIT     3001
#define ID_MENU_AUTORUN  3002

#define AUTORUN_REG_SUBKEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTORUN_VALUE_NAME L"SdrContentBrightnessApp"

/* All layout numbers below are defined at the 96-DPI ("100%") design
 * size and scaled at runtime via Scale() to match the DPI of whichever
 * monitor the flyout is currently on. */
#define BASE_POPUP_WIDTH        260
#define BASE_POPUP_HEIGHT       132
#define BASE_POPUP_RADIUS       12
#define BASE_SLIDER_HEIGHT      40
#define BASE_SLIDER_MARGIN      20
#define BASE_SLIDER_BOTTOM_GAP  18
#define BASE_POPUP_GAP          12   /* gap between the tray icon and the flyout */
#define BASE_THUMB_RADIUS       10
#define BASE_THUMB_RADIUS_DRAG  11
#define BASE_TRACK_HEIGHT       6

#define REG_SUBKEY       L"Software\\SdrContentBrightnessApp"
#define REG_VALUE_NAME   L"Value"
#define DEFAULT_VALUE    55

 /* ------------------------------------------------------------------- */
 /* Globals                                                              */
 /* ------------------------------------------------------------------- */
static HINSTANCE g_hInst = NULL;
static HWND      g_hMainWnd = NULL;   /* hidden, owns the tray icon   */
static HWND      g_hPopupWnd = NULL;   /* flyout window                */
static HWND      g_hSlider = NULL;   /* custom slider child control  */
static NOTIFYICONDATAW g_nid;

static ULONG_PTR g_gdiplusToken = 0;
static HICON     g_hTrayIcon = NULL;  /* runtime-drawn brightness icon */

static BOOL      g_darkMode = FALSE;
static COLORREF  g_accent = RGB(0, 120, 215);
static COLORREF  g_bgColor = RGB(249, 249, 249);
static COLORREF  g_textPrimary = RGB(0, 0, 0);
static COLORREF  g_textSecondary = RGB(96, 96, 96);
static COLORREF  g_trackBg = RGB(214, 214, 214);
static COLORREF  g_borderColor = RGB(220, 220, 220);

static HFONT     g_fontSmall = NULL;   /* "Value" caption   */
static HFONT     g_fontBig = NULL;   /* big numeric value */

static int  g_sliderValue = DEFAULT_VALUE;   /* 0-100 */
static BOOL g_sliderDragging = FALSE;
static BOOL g_sliderHot = FALSE;

static int  g_dpi = 96;  /* current DPI of the monitor the flyout is on; 96 = 100% scaling */

/* Scales a design-time (96-DPI) pixel value to the current DPI. */
static int Scale(int value)
{
    return MulDiv(value, g_dpi, 96);
}

/* ---- Forward declarations ---- */
LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK PopupWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SliderWndProc(HWND, UINT, WPARAM, LPARAM);
static void ShowSliderPopup(void);
static void HideSliderPopup(void);
static void AddTrayIcon(HWND hwnd);
static void RemoveTrayIcon(void);
static void RefreshSystemTheme(void);
static void ApplyRoundedRegion(HWND hwnd, int width, int height, int radius);
static void CreateThemeFonts(void);
static void ApplyDpiToPopup(int newDpi, const RECT* suggestedRect);
static int  SliderRectToValue(HWND hwnd, int x);
static void UpdateSliderValue(int newValue);
static int  LoadSliderValueFromRegistry(void);
static void SaveSliderValueToRegistry(int value);
static BOOL IsAutorunEnabled(void);
static void AddAutorun(void);
static void RemoveAutorun(void);
static Color ColorRefToGdip(COLORREF c);
static HICON CreateBrightnessIcon(COLORREF fg, int size);
static HICON GenerateBrightnessIcon(COLORREF fg, int size);  // not used
static BOOL  IsTaskbarLightTheme(void);
static void  UpdateTrayIcon(void);
static void  UpdateTrayTooltip(void);
static void  FillCapsuleH(Graphics& g, Brush& brush, REAL left, REAL top, REAL right, REAL bottom);



/* ------------------------------------------------------------------- */
/* Small helpers                                                        */
/* ------------------------------------------------------------------- */
void DebugPrint(const wchar_t* format, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    vswprintf_s(buffer, 512, format, args);
    va_end(args);

    OutputDebugStringW(buffer);
}

static Color ColorRefToGdip(COLORREF c)
{
    return Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

/* Fills a horizontal "pill"/capsule shape (two round caps + a straight
 * middle section) with GDI+ so the caps are properly anti-aliased -
 * this is what keeps the slider track/fill from looking blocky. */
static void FillCapsuleH(Graphics& g, Brush& brush, REAL left, REAL top, REAL right, REAL bottom)
{
    REAL r = (bottom - top) / 2.0f;
    if (right < left + 2 * r) right = left + 2 * r;

    g.FillEllipse(&brush, left, top, 2 * r, bottom - top);
    g.FillEllipse(&brush, right - 2 * r, top, 2 * r, bottom - top);
    if (right - 2 * r > left + r)
        g.FillRectangle(&brush, left + r, top, (right - r) - (left + r), bottom - top);
}

/* ------------------------------------------------------------------- */
/* Theme helpers                                                        */
/* ------------------------------------------------------------------- */
static BOOL IsSystemDarkThemeApplied(void)
{
    HKEY hKey;
    DWORD value = 1, size = sizeof(value);
    BOOL dark = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL,
            (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            dark = (value == 0);
        }
        RegCloseKey(hKey);
    }
    return dark;
}

/* The taskbar/Start theme (SystemUsesLightTheme) is tracked separately
 * from the app theme (AppsUseLightTheme) - Windows lets you mix light
 * apps with a dark taskbar or vice versa. We use this one to decide
 * whether the tray icon should be drawn light or dark, so it stays
 * legible against the taskbar itself. */
static BOOL IsTaskbarLightTheme(void)
{
    HKEY hKey;
    DWORD value = 0, size = sizeof(value);
    BOOL light = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"SystemUsesLightTheme", NULL, NULL,
            (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            light = (value != 0);
        }
        RegCloseKey(hKey);
    }
    return light;
}

/* Reads the persisted slider value from
 * HKCU\Software\TraySliderApp\SliderValue. If the key/value doesn't
 * exist yet (first run), it seeds it with DEFAULT_VALUE (55) so the
 * default itself is durable from the very first launch, not just held
 * in memory until the user first touches the slider. */
static int LoadSliderValueFromRegistry(void)
{
    HKEY hKey;
    DWORD value = DEFAULT_VALUE;
    DWORD size = sizeof(value);
    DWORD type = 0;
    BOOL  found = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, REG_VALUE_NAME, NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS
            && type == REG_DWORD) {
            found = TRUE;
        }
        RegCloseKey(hKey);
    }

    if (!found) {
        value = DEFAULT_VALUE;
        SaveSliderValueToRegistry(value); /* persist the default immediately */
    }

    if (value > 100) value = 100; /* guard against corrupted/edited data */
    return (int)value;
}

static void SaveSliderValueToRegistry(int value)
{
    HKEY hKey;
    DWORD dwValue = (DWORD)value;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_SUBKEY, 0, NULL, 0,
        KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, REG_VALUE_NAME, 0, REG_DWORD, (const BYTE*)&dwValue, sizeof(dwValue));
        RegCloseKey(hKey);
    }
}

/* Autorun (Start on sign-in) helpers - these read/write the per-user
 * "Run" key at HKCU\...\CurrentVersion\Run, which Windows itself
 * consults on every logon. No admin rights are required since this is
 * HKCU rather than HKLM. */
static BOOL IsAutorunEnabled(void)
{
    HKEY hKey;
    BOOL enabled = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTORUN_REG_SUBKEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, AUTORUN_VALUE_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            enabled = TRUE;
        }
        RegCloseKey(hKey);
    }
    return enabled;
}

static void AddAutorun(void)
{
    HKEY hKey;
    WCHAR exePath[MAX_PATH];

    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return; /* path retrieval failed / truncated */

    if (RegCreateKeyExW(HKEY_CURRENT_USER, AUTORUN_REG_SUBKEY, 0, NULL, 0,
        KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, AUTORUN_VALUE_NAME, 0, REG_SZ,
            (const BYTE*)exePath, (DWORD)((wcslen(exePath) + 1) * sizeof(WCHAR)));
        RegCloseKey(hKey);
    }
}

static void RemoveAutorun(void)
{
    HKEY hKey;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTORUN_REG_SUBKEY, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, AUTORUN_VALUE_NAME);
        RegCloseKey(hKey);
    }
}

static COLORREF GetSystemAccentColor(void)
{
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        BYTE r = (BYTE)((color >> 16) & 0xFF);
        BYTE g = (BYTE)((color >> 8) & 0xFF);
        BYTE b = (BYTE)(color & 0xFF);
        return RGB(r, g, b);
    }
    return RGB(0, 120, 215); /* Windows default blue fallback */
}

static void RefreshSystemTheme(void)
{
    g_darkMode = IsSystemDarkThemeApplied();
    g_accent = GetSystemAccentColor();

    if (g_darkMode) {
        g_bgColor = RGB(32, 32, 32);
        g_textPrimary = RGB(255, 255, 255);
        g_textSecondary = RGB(200, 200, 200);
        g_trackBg = RGB(85, 85, 85);
        g_borderColor = RGB(55, 55, 55);
    }
    else {
        g_bgColor = RGB(249, 249, 249);
        g_textPrimary = RGB(0, 0, 0);
        g_textSecondary = RGB(96, 96, 96);
        g_trackBg = RGB(214, 214, 214);
        g_borderColor = RGB(223, 223, 223);
    }

    if (g_hPopupWnd) {
        BOOL dark = g_darkMode;
        DwmSetWindowAttribute(g_hPopupWnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
            &dark, sizeof(dark));
        InvalidateRect(g_hPopupWnd, NULL, TRUE);
        InvalidateRect(g_hSlider, NULL, TRUE);
    }
}

static void CreateThemeFonts(void)
{
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (g_fontBig)   DeleteObject(g_fontBig);

    HDC hdc = GetDC(NULL);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);

    g_fontSmall = CreateFontW(
        -MulDiv(12, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    g_fontBig = CreateFontW(
        -MulDiv(24, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Semibold");
}

/* Clip a window to a rounded-rectangle region, Windows-11-flyout style. */
static void ApplyRoundedRegion(HWND hwnd, int width, int height, int radius)
{
    HRGN rgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    SetWindowRgn(hwnd, rgn, TRUE); /* window now owns rgn */
}

/* Central place that reacts to a DPI change (including the very first
 * time the flyout is positioned on a monitor): updates g_dpi, rebuilds
 * the fonts at the new size, and resizes/repositions both the popup
 * and its slider child so the whole layout scales together instead of
 * just growing blurry. If suggestedRect is non-NULL (as supplied by
 * WM_DPICHANGED), that placement is used; otherwise the popup keeps
 * its current top-left and is just resized. */
static void ApplyDpiToPopup(int newDpi, const RECT* suggestedRect)
{
    if (newDpi <= 0) return;
    g_dpi = newDpi;
    CreateThemeFonts();

    int w = Scale(BASE_POPUP_WIDTH);
    int h = Scale(BASE_POPUP_HEIGHT);

    if (suggestedRect) {
        SetWindowPos(g_hPopupWnd, NULL,
            suggestedRect->left, suggestedRect->top,
            suggestedRect->right - suggestedRect->left,
            suggestedRect->bottom - suggestedRect->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    else if (g_hPopupWnd) {
        SetWindowPos(g_hPopupWnd, NULL, 0, 0, w, h,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (g_hPopupWnd)
        ApplyRoundedRegion(g_hPopupWnd, w, h, Scale(BASE_POPUP_RADIUS));

    if (g_hSlider) {
        int sliderW = w - Scale(BASE_SLIDER_MARGIN) * 2;
        int sliderH = Scale(BASE_SLIDER_HEIGHT);
        int sliderX = Scale(BASE_SLIDER_MARGIN);
        int sliderY = h - sliderH - Scale(BASE_SLIDER_BOTTOM_GAP);
        SetWindowPos(g_hSlider, NULL, sliderX, sliderY, sliderW, sliderH,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (g_hPopupWnd) InvalidateRect(g_hPopupWnd, NULL, TRUE);
    if (g_hSlider)   InvalidateRect(g_hSlider, NULL, TRUE);
}

/* Draws a simple brightness/sun glyph (center disc + 8 round-capped
 * rays) into a fresh alpha-transparent GDI+ bitmap, anti-aliased, then
 * hands back a real HICON via Bitmap::GetHICON. Rendered at runtime so
 * there's no .ico resource to ship. */
static HICON CreateBrightnessIcon(COLORREF fg, int size)
{
    // return GenerateBrightnessIcon(fg, size);
    
    // fg ignored here, for compatibility with GenerateBrightnessIcon
    return IsTaskbarLightTheme() ? 
        IconWhiteToBlack(IconGetBySizeForWindow(size, g_hPopupWnd)) : 
        IconGetBySizeForWindow(size, g_hPopupWnd);
}

static HICON GenerateBrightnessIcon(COLORREF fg, int size)
{
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0)); /* fully transparent */

    Color col = ColorRefToGdip(fg);
    SolidBrush brush(col);

    REAL cx = size / 2.0f, cy = size / 2.0f;
    REAL coreR = size * 0.20f;
    REAL rayInner = size * 0.31f;
    REAL rayOuter = size * 0.47f;
    REAL rayWidth = size * 0.09f;
    if (rayWidth < 1.4f) rayWidth = 1.4f;

    Pen pen(col, rayWidth);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);

    for (int i = 0; i < 8; i++) {
        double angle = i * (3.14159265358979323846 / 4.0);
        REAL x1 = (REAL)(cx + std::cos(angle) * rayInner);
        REAL y1 = (REAL)(cy + std::sin(angle) * rayInner);
        REAL x2 = (REAL)(cx + std::cos(angle) * rayOuter);
        REAL y2 = (REAL)(cy + std::sin(angle) * rayOuter);
        g.DrawLine(&pen, x1, y1, x2, y2);
    }

    /* central disc drawn last so it cleanly caps the inner ray ends */
    g.FillEllipse(&brush, cx - coreR, cy - coreR, coreR * 2, coreR * 2);

    HICON hIcon = NULL;
    bmp.GetHICON(&hIcon);
    return hIcon;
}

/* (Re)generates the tray icon to match the current taskbar theme and
 * installs it, freeing the previous one. Safe to call any time,
 * including from a live theme-change notification. */
static void UpdateTrayIcon(void)
{
    COLORREF iconColor = IsTaskbarLightTheme() ? RGB(0, 0, 0) : RGB(255, 255, 255);
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size <= 0) size = 16;

    HICON hNew = CreateBrightnessIcon(iconColor, size);
    if (!hNew) return;

    HICON hOld = g_hTrayIcon;
    g_hTrayIcon = hNew;

    g_nid.hIcon = g_hTrayIcon;
    if (g_nid.hWnd) Shell_NotifyIconW(NIM_MODIFY, &g_nid);

    if (hOld) DestroyIcon(hOld);
}

/* ------------------------------------------------------------------- */
/* Entry point                                                          */
/* ------------------------------------------------------------------- */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
    LPWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    g_hInst = hInstance;

    /* Must be called before creating any window. PER_MONITOR_AWARE_V2 lets
     * Windows scale our top-level windows' non-client areas (and DWM
     * effects like rounded corners/shadow) automatically per-monitor,
     * while we handle scaling the client-area content ourselves below
     * and in ApplyDpiToPopup/WM_DPICHANGED. */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_dpi = (int)GetDpiForSystem(); /* refined per-monitor once the flyout is first shown */

    GdiplusStartupInput gdiInput;
    GdiplusStartup(&g_gdiplusToken, &gdiInput, NULL);

    RefreshSystemTheme();
    CreateThemeFonts();
    g_sliderValue = LoadSliderValueFromRegistry();

    /* ---- Hidden main window: owns the tray icon ---- */
    WNDCLASSW wcMain;
    ZeroMemory(&wcMain, sizeof(wcMain));
    wcMain.lpfnWndProc = MainWndProc;
    wcMain.hInstance = hInstance;
    wcMain.lpszClassName = L"BrightnessAppMainWndClass";
    wcMain.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wcMain.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wcMain);

    g_hMainWnd = CreateWindowExW(
        0, L"BrightnessAppMainWndClass", L"BrightnessAppSlider", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hMainWnd) return 0;
    ShowWindow(g_hMainWnd, SW_HIDE);

    /* ---- Custom slider control class ---- */
    WNDCLASSW wcSlider;
    ZeroMemory(&wcSlider, sizeof(wcSlider));
    wcSlider.lpfnWndProc = SliderWndProc;
    wcSlider.hInstance = hInstance;
    wcSlider.lpszClassName = L"ModernSliderClass";
    wcSlider.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    RegisterClassW(&wcSlider);

    /* ---- Flyout popup window class: rounded, drop-shadowed ---- */
    WNDCLASSW wcPopup;
    ZeroMemory(&wcPopup, sizeof(wcPopup));
    wcPopup.style = CS_DROPSHADOW; /* soft elevation shadow, Win11 flyout style */
    wcPopup.lpfnWndProc = PopupWndProc;
    wcPopup.hInstance = hInstance;
    wcPopup.lpszClassName = L"BrightnessAppPopupWndClass";
    wcPopup.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wcPopup);

    g_hPopupWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"BrightnessAppPopupWndClass", L"",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, Scale(BASE_POPUP_WIDTH), Scale(BASE_POPUP_HEIGHT),
        NULL, NULL, hInstance, NULL);

    ApplyRoundedRegion(g_hPopupWnd, Scale(BASE_POPUP_WIDTH), Scale(BASE_POPUP_HEIGHT), Scale(BASE_POPUP_RADIUS));
    {
        BOOL dark = g_darkMode;
        DwmSetWindowAttribute(g_hPopupWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_ROUND;
        DwmSetWindowAttribute(g_hPopupWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    }

    g_hSlider = CreateWindowExW(
        0, L"ModernSliderClass", L"",
        WS_CHILD | WS_VISIBLE,
        Scale(BASE_SLIDER_MARGIN), Scale(BASE_POPUP_HEIGHT) - Scale(BASE_SLIDER_HEIGHT) - Scale(BASE_SLIDER_BOTTOM_GAP),
        Scale(BASE_POPUP_WIDTH) - Scale(BASE_SLIDER_MARGIN) * 2, Scale(BASE_SLIDER_HEIGHT),
        g_hPopupWnd, NULL, hInstance, NULL);

    AddTrayIcon(g_hMainWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveTrayIcon();
    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}

/* ------------------------------------------------------------------- */
/* Tray icon helpers                                                    */
/* ------------------------------------------------------------------- */
/* Updates the tray icon's hover tooltip to reflect the current value.
 * Safe to call any time the icon already exists (uses NIM_MODIFY so it
 * takes effect immediately, even while the flyout is open). */
static void UpdateTrayTooltip(void)
{
    if (!g_nid.hWnd) return; /* icon not created yet */
    wsprintfW(g_nid.szTip, L"Brightness: %d%%", g_sliderValue);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void AddTrayIcon(HWND hwnd)
{
    COLORREF iconColor = IsTaskbarLightTheme() ? RGB(0, 0, 0) : RGB(255, 255, 255);
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size <= 0) size = 16;
    g_hTrayIcon = CreateBrightnessIcon(iconColor, size);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = ID_TRAY_ICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_hTrayIcon ? g_hTrayIcon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wsprintfW(g_nid.szTip, L"Brightness: %d%%", g_sliderValue);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_hTrayIcon) {
        DestroyIcon(g_hTrayIcon);
        g_hTrayIcon = NULL;
    }
}

/* ------------------------------------------------------------------- */
/* Flyout show / hide, positioned like a Win11 tray flyout              */
/* ------------------------------------------------------------------- */
static void ShowSliderPopup(void)
{
    RefreshSystemTheme(); /* pick up any theme/accent change since last open */

    POINT pt;
    GetCursorPos(&pt);

    /* Use the work area of whichever monitor the cursor (and tray icon
     * click) is actually on - important once multiple monitors can have
     * different DPI/scaling. */
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    RECT work = mi.rcWork;

    int popupW = Scale(BASE_POPUP_WIDTH);
    int popupH = Scale(BASE_POPUP_HEIGHT);
    int gap = Scale(BASE_POPUP_GAP);

    int x = pt.x - popupW / 2;
    int y = pt.y - popupH - gap;

    if (x < work.left)  x = work.left + 4;
    if (x + popupW > work.right) x = work.right - popupW - 4;
    if (y < work.top)   y = pt.y + gap;

    SetWindowPos(g_hPopupWnd, HWND_TOPMOST, x, y, 0, 0,
        SWP_NOSIZE | SWP_SHOWWINDOW);

    /* WM_DPICHANGED normally fires and calls ApplyDpiToPopup already if
     * that move crossed onto a differently-scaled monitor - this is just
     * a cheap belt-and-braces check for the very first time the flyout
     * is ever shown. */
    UINT actualDpi = GetDpiForWindow(g_hPopupWnd);
    if (actualDpi > 0 && (int)actualDpi != g_dpi) {
        ApplyDpiToPopup((int)actualDpi, NULL);
    }

    AnimateWindow(g_hPopupWnd, 120, AW_BLEND); /* soft fade-in like Win11 flyouts */
    SetForegroundWindow(g_hPopupWnd);
    SetFocus(g_hSlider);
}

static void HideSliderPopup(void)
{
    if (IsWindowVisible(g_hPopupWnd))
        AnimateWindow(g_hPopupWnd, 100, AW_BLEND | AW_HIDE);
}

/* ------------------------------------------------------------------- */
/* Main (hidden) window procedure                                       */
/* ------------------------------------------------------------------- */
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_TRAYICON:
        switch (lParam) {
        case WM_LBUTTONUP:
            if (IsWindowVisible(g_hPopupWnd)) HideSliderPopup();
            else ShowSliderPopup();
            break;
        case WM_RBUTTONUP: {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING | (IsAutorunEnabled() ? MF_CHECKED : MF_UNCHECKED),
                ID_MENU_AUTORUN, L"Autorun");
            AppendMenuW(hMenu, MF_STRING, ID_MENU_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
            break;
        }
        }
        return 0;

    case WM_SETTINGCHANGE:
        /* Fired when the user flips Settings > Personalization > Colors */
        if (lParam && lstrcmpW((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
            RefreshSystemTheme();
            UpdateTrayIcon();
        }
        return 0;

    case WM_DWMCOLORIZATIONCOLORCHANGED:
        RefreshSystemTheme();
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_MENU_EXIT) DestroyWindow(hwnd);
        else if (LOWORD(wParam) == ID_MENU_AUTORUN) {
            if (IsAutorunEnabled()) RemoveAutorun();
            else AddAutorun();
        }
        return 0;

    case WM_DESTROY:
        SaveSliderValueToRegistry(g_sliderValue);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------- */
/* Popup (flyout) window procedure                                      */
/* ------------------------------------------------------------------- */
LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_ERASEBKGND:
        return 1; /* avoid flicker; WM_PAINT below fills everything */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        /* Double buffer the whole flyout for a crisp, flicker-free draw */
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        HBRUSH bgBrush = CreateSolidBrush(g_bgColor);
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        /* subtle 1px border so the card reads clearly against any wallpaper -
         * drawn with GDI+ so the rounded corners are smooth, not jagged */
        {
            Graphics gBorder(memDC);
            gBorder.SetSmoothingMode(SmoothingModeAntiAlias);
            Pen borderPen(ColorRefToGdip(g_borderColor), 1.0f);

            REAL radius = (REAL)Scale(BASE_POPUP_RADIUS);
            REAL d = radius * 2.0f;
            gBorder.DrawArc(&borderPen, 0.5f, 0.5f, d, d, 180, 90);
            gBorder.DrawArc(&borderPen, (REAL)rc.right - d - 0.5f, 0.5f, d, d, 270, 90);
            gBorder.DrawArc(&borderPen, (REAL)rc.right - d - 0.5f, (REAL)rc.bottom - d - 0.5f, d, d, 0, 90);
            gBorder.DrawArc(&borderPen, 0.5f, (REAL)rc.bottom - d - 0.5f, d, d, 90, 90);
            gBorder.DrawLine(&borderPen, radius, 0.5f, (REAL)rc.right - radius, 0.5f);
            gBorder.DrawLine(&borderPen, radius, (REAL)rc.bottom - 0.5f, (REAL)rc.right - radius, (REAL)rc.bottom - 0.5f);
            gBorder.DrawLine(&borderPen, 0.5f, radius, 0.5f, (REAL)rc.bottom - radius);
            gBorder.DrawLine(&borderPen, (REAL)rc.right - 0.5f, radius, (REAL)rc.right - 0.5f, (REAL)rc.bottom - radius);
        }

        SetBkMode(memDC, TRANSPARENT);

        /* Caption */
        RECT capRect = { Scale(20), Scale(16), rc.right - Scale(20), Scale(40) };
        SetTextColor(memDC, g_textSecondary);
        HFONT oldFont = (HFONT)SelectObject(memDC, g_fontSmall);
        DrawTextW(memDC, L"SDR Content Brightness", -1, &capRect, DT_LEFT | DT_SINGLELINE);

        /* Big accent-colored numeric readout */
        wchar_t buf[16];
        wsprintfW(buf, L"%d", g_sliderValue);
        RECT valRect = { Scale(20), Scale(34), rc.right - Scale(20), Scale(74) };
        SelectObject(memDC, g_fontBig);
        SetTextColor(memDC, g_accent);
        DrawTextW(memDC, buf, -1, &valRect, DT_LEFT | DT_SINGLELINE);

        SelectObject(memDC, oldFont);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) HideSliderPopup();
        return 0;

    case WM_DPICHANGED: {
        int newDpi = HIWORD(wParam); /* LOWORD/HIWORD are the same value (x/y dpi) */
        const RECT* suggested = (const RECT*)lParam;
        ApplyDpiToPopup(newDpi, suggested);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        UpdateSliderValue(g_sliderValue + notches * 2);
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) HideSliderPopup();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------- */
/* Custom "pill" slider control                                         */
/* ------------------------------------------------------------------- */
static int SliderRectToValue(HWND hwnd, int x)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int thumbR = Scale(BASE_THUMB_RADIUS);
    int usableLeft = thumbR;
    int usableRight = rc.right - thumbR;
    if (x < usableLeft) x = usableLeft;
    if (x > usableRight) x = usableRight;
    int val = (int)(((double)(x - usableLeft) / (usableRight - usableLeft)) * 100.0 + 0.5);
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    return val;
}

/* ------------------------------------------------------------------- */
/* Windows brightness control                                         */
/* ------------------------------------------------------------------- */

typedef HRESULT(WINAPI* DwmpSdrToHdrBoostFn)(HMONITOR, double);

struct DwmpSdrToHdrBoostContext {
    DwmpSdrToHdrBoostFn fn;
    double boost;
    int successCount;
};


BOOL CALLBACK ApplyDwmSdrToHdrBoostToMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    DwmpSdrToHdrBoostContext* context = reinterpret_cast<DwmpSdrToHdrBoostContext*>(param);
    HRESULT hr = context->fn(monitor, context->boost);
    if (SUCCEEDED(hr)) ++context->successCount;
    return TRUE;
}

bool ApplyDwmSdrToHdrBoost(UINT32 sdrLevel, int* successCount) {
    HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    if (!dwmapi) return false;

    DwmpSdrToHdrBoostFn fn =
        reinterpret_cast<DwmpSdrToHdrBoostFn>(GetProcAddress(dwmapi, MAKEINTRESOURCEA(171)));
    if (!fn) {
        FreeLibrary(dwmapi);
        return false;
    }

    DwmpSdrToHdrBoostContext context = {};
    context.fn = fn;
    context.boost = static_cast<double>(sdrLevel) / 1000.0;
    context.successCount = 0;
    EnumDisplayMonitors(NULL, NULL, ApplyDwmSdrToHdrBoostToMonitor, reinterpret_cast<LPARAM>(&context));
    FreeLibrary(dwmapi);

    if (successCount) *successCount = context.successCount;
    return context.successCount > 0;
}



UINT32 BrightnessPercentToSdrLevel(int brightness) {
    //brightness = std::max(0, std::min(100, value));
    return 1000u + static_cast<UINT32>(brightness) * 50u;
}

/* Central place to change the slider's value: clamps it, repaints only
 * the slider control plus the small numeric-readout area of the popup
 * (not the whole flyout), and forces those repaints to happen right
 * away with UpdateWindow so the thumb and the big number move in
 * lockstep instead of one trailing a frame behind the other. */
static void UpdateSliderValue(int newValue)
{
    if (newValue < 0) newValue = 0;
    if (newValue > 100) newValue = 100;


    if (g_sliderValue == newValue) return;

    g_sliderValue = newValue;

    int dwmSuccess = 0;
    ApplyDwmSdrToHdrBoost(BrightnessPercentToSdrLevel(g_sliderValue), &dwmSuccess);
    //DebugPrint(L"SValue: %u\n", g_sliderValue);
    //DebugPrint(L"ApplyDwmSdrToHdrBoost success: %u\n", dwmSuccess);

    InvalidateRect(g_hSlider, NULL, FALSE);
    UpdateWindow(g_hSlider);

    RECT labelRect = { 0, 0, Scale(BASE_POPUP_WIDTH), Scale(80) }; /* covers "Value" caption + big number only */
    InvalidateRect(g_hPopupWnd, &labelRect, FALSE);
    UpdateWindow(g_hPopupWnd);

    UpdateTrayTooltip();
}

LRESULT CALLBACK SliderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        g_sliderDragging = TRUE;
        UpdateSliderValue(SliderRectToValue(hwnd, GET_X_LPARAM(lParam)));
        return 0;

    case WM_MOUSEMOVE: {
        BOOL wasHot = g_sliderHot;
        g_sliderHot = TRUE;
        if (g_sliderDragging) {
            UpdateSliderValue(SliderRectToValue(hwnd, GET_X_LPARAM(lParam)));
        }
        else if (!wasHot) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        /* Each notch (WHEEL_DELTA = 120) nudges the value by 2. */
        int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        UpdateSliderValue(g_sliderValue + notches * 2);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_sliderHot = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONUP:
        if (g_sliderDragging) {
            g_sliderDragging = FALSE;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        /* background = parent's flyout color, so the control looks "transparent" */
        HBRUSH bgBrush = CreateSolidBrush(g_bgColor);
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        /* Everything below is drawn with GDI+ (anti-aliased) instead of raw
         * GDI Ellipse/RoundRect, which have no anti-aliasing and look
         * jagged/pixelated at this control's size - especially the thumb. */
        {
            Graphics g(memDC);
            g.SetSmoothingMode(SmoothingModeAntiAlias);

            REAL thumbR = g_sliderDragging ? (REAL)Scale(BASE_THUMB_RADIUS_DRAG) : (REAL)Scale(BASE_THUMB_RADIUS);
            REAL centerY = rc.bottom / 2.0f;
            REAL trackH = (REAL)Scale(BASE_TRACK_HEIGHT);
            REAL left = thumbR;
            REAL right = rc.right - thumbR;
            REAL thumbX = left + (REAL)((g_sliderValue / 100.0) * (right - left));

            SolidBrush trackBrush(ColorRefToGdip(g_trackBg));
            SolidBrush accentBrush(ColorRefToGdip(g_accent));
            SolidBrush bgBrush2(ColorRefToGdip(g_bgColor));

            /* full (unfilled) track */
            FillCapsuleH(g, trackBrush,
                left - thumbR, centerY - trackH / 2,
                right + thumbR, centerY + trackH / 2);

            /* active (filled) portion, accent colored */
            FillCapsuleH(g, accentBrush,
                left - thumbR, centerY - trackH / 2,
                thumbX + trackH, centerY + trackH / 2);

            /* thumb: Win11-style ring, fills solid while dragging */
            g.FillEllipse(&accentBrush,
                thumbX - thumbR, centerY - thumbR, thumbR * 2, thumbR * 2);
            if (!g_sliderDragging) {
                REAL innerR = g_sliderHot ? thumbR - (REAL)Scale(5) : thumbR - (REAL)Scale(3);
                g.FillEllipse(&bgBrush2,
                    thumbX - innerR, centerY - innerR, innerR * 2, innerR * 2);
            }
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}