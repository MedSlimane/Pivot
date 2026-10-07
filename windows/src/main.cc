// Pivot for Windows — tray utility for monitor rotation + arrangement.
//
// Faithful port of the macOS Pivot app to native Win32 (C++17, no deps):
//   - Tray icon with per-display rotation submenu (0/90/180/270), applied instantly
//   - Arrange window: drag display rectangles to reposition; double-click or
//     right-click a display to rotate; edges snap when dropped close together
//   - "Display Settings..." shortcut (ms-settings:display)
//   - Zero polling: UI refreshes only on WM_DISPLAYCHANGE (debounced) + actions
//
// Rotation   : ChangeDisplaySettingsEx + DEVMODE.dmDisplayOrientation
// Arrangement: ChangeDisplaySettingsEx + DEVMODE.dmPosition (CDS_UPDATEREGISTRY)
// Rotation support gating: CDS_TEST probe (like canChangeOrientation on macOS)
//
// Build: g++ -O2 -std=c++17 -DUNICODE -D_UNICODE -mwindows src\main.cc -o Pivot.exe -lshell32
//   or see build.ps1 (auto-detects MinGW g++ / MSVC cl).
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------- model ---

struct DisplayInfo {
    std::wstring device;    // \\.\DISPLAY1
    std::wstring friendly;  // monitor name, fallback = device
    int x = 0, y = 0;       // desktop origin (device px)
    int w = 0, h = 0;       // current mode size (post-rotation)
    int degrees = 0;        // 0/90/180/270
    bool primary = false;
    bool canRotate = false;
};

static const int kDeg[4] = {0, 90, 180, 270};

static int OriFromDeg(int deg) {
    switch (((deg % 360) + 360) % 360) {
        case 90: return DMDO_90;
        case 180: return DMDO_180;
        case 270: return DMDO_270;
        default: return DMDO_DEFAULT;
    }
}

static bool CurrentMode(const std::wstring& dev, DEVMODEW& dm) {
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    return EnumDisplaySettingsExW(dev.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0) != FALSE;
}

// CDS_TEST probe: ask the driver whether a 90-degree-flipped orientation
// would be accepted, without applying anything.
static bool ProbeCanRotate(const std::wstring& dev, const DEVMODEW& cur) {
    DWORD curOri = (cur.dmFields & DM_DISPLAYORIENTATION) ? cur.dmDisplayOrientation : DMDO_DEFAULT;
    DWORD want = (curOri == DMDO_DEFAULT) ? (DWORD)DMDO_90 : (DWORD)DMDO_DEFAULT;
    DEVMODEW dm = cur;
    dm.dmDisplayOrientation = want;
    if ((curOri % 2) != (want % 2)) {
        DWORD t = dm.dmPelsWidth;
        dm.dmPelsWidth = dm.dmPelsHeight;
        dm.dmPelsHeight = t;
    }
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYORIENTATION;
    if (dm.dmDisplayFrequency) dm.dmFields |= DM_DISPLAYFREQUENCY;
    return ChangeDisplaySettingsExW(dev.c_str(), &dm, NULL, CDS_TEST, NULL) == DISP_CHANGE_SUCCESSFUL;
}

static std::vector<DisplayInfo> SnapshotDisplays() {
    std::vector<DisplayInfo> out;
    for (DWORD ai = 0;; ++ai) {
        DISPLAY_DEVICEW ad = {};
        ad.cb = sizeof(ad);
        if (!EnumDisplayDevicesW(NULL, ai, &ad, 0)) break;
        if (!(ad.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        if (ad.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;

        DisplayInfo d;
        d.device = ad.DeviceName;
        d.primary = (ad.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;

        DISPLAY_DEVICEW mon = {};
        mon.cb = sizeof(mon);
        if (EnumDisplayDevicesW(ad.DeviceName, 0, &mon, 0) && mon.DeviceString[0])
            d.friendly = mon.DeviceString;
        else
            d.friendly = ad.DeviceName;

        DEVMODEW dm;
        if (!CurrentMode(d.device, dm)) continue;
        d.x = dm.dmPosition.x;
        d.y = dm.dmPosition.y;
        d.w = (int)dm.dmPelsWidth;
        d.h = (int)dm.dmPelsHeight;
        DWORD ori = (dm.dmFields & DM_DISPLAYORIENTATION) ? dm.dmDisplayOrientation : DMDO_DEFAULT;
        d.degrees = ((int)(ori & 3)) * 90;
        d.canRotate = ProbeCanRotate(d.device, dm);
        out.push_back(d);
    }
    return out;
}

static std::wstring Describe(const DisplayInfo& d) {
    wchar_t buf[256];
    // "DELL U2720Q — 3840×2160, 0°"
    swprintf(buf, 256, L"%ls \u2014 %dx%d, %d\u00B0", d.friendly.c_str(), d.w, d.h, d.degrees);
    return buf;
}

// ------------------------------------------------------------ app state ---

static HINSTANCE g_hInst = NULL;
static HWND g_hwndTray = NULL;     // hidden message window
static HWND g_hwndCanvas = NULL;   // arrange window (modeless)
static HICON g_hIcon = NULL;
static UINT g_wmTaskbarCreated = 0;
static std::vector<DisplayInfo> g_boxes;  // last snapshot (menu rebuild basis)

enum {
    WM_APP_TRAY = WM_APP + 1,
    TIMER_REFRESH = 1,
    CMD_ARRANGE = 100,
    CMD_SETTINGS = 101,
    CMD_QUIT = 102,
    CMD_ROT_BASE = 1000,          // + idx*16 + slot(0..3)
    CMD_CANVAS_ROT_BASE = 5000,   // + idx*16 + slot(0..3)
};

static void ShowBalloon(const wchar_t* title, const wchar_t* msg);
static void RefreshAll();
static void ShowArrangeWindow();

// -------------------------------------------------------------- display ---

static bool ApplyRotation(const std::wstring& dev, int deg) {
    DEVMODEW cur;
    if (!CurrentMode(dev, cur)) {
        ShowBalloon(L"Pivot", L"Could not query display mode.");
        return false;
    }
    DWORD curOri = (cur.dmFields & DM_DISPLAYORIENTATION) ? cur.dmDisplayOrientation : DMDO_DEFAULT;
    DWORD want = (DWORD)OriFromDeg(deg);
    if (curOri == want) return true;

    DEVMODEW dm = cur;
    dm.dmDisplayOrientation = want;
    if ((curOri % 2) != (want % 2)) {
        DWORD t = dm.dmPelsWidth;
        dm.dmPelsWidth = dm.dmPelsHeight;
        dm.dmPelsHeight = t;
    }
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYORIENTATION;
    if (dm.dmDisplayFrequency) dm.dmFields |= DM_DISPLAYFREQUENCY;

    LONG r = ChangeDisplaySettingsExW(dev.c_str(), &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
    if (r == DISP_CHANGE_SUCCESSFUL) {
        r = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    }
    if (r != DISP_CHANGE_SUCCESSFUL) {
        wchar_t msg[128];
        swprintf(msg, 128, L"Rotation failed (code %ld). Driver may not support it.", r);
        ShowBalloon(L"Pivot", msg);
        return false;
    }
    return true;
}

static bool ApplyPositions(const std::vector<std::pair<std::wstring, POINT>>& moves) {
    for (const auto& m : moves) {
        DEVMODEW dm;
        if (!CurrentMode(m.first, dm)) {
            ShowBalloon(L"Pivot", L"Could not query display mode.");
            return false;
        }
        dm.dmPosition.x = m.second.x;
        dm.dmPosition.y = m.second.y;
        dm.dmFields = DM_POSITION;
        ChangeDisplaySettingsExW(m.first.c_str(), &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
    }
    LONG r = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        wchar_t msg[128];
        swprintf(msg, 128, L"Arrangement failed (code %ld).", r);
        ShowBalloon(L"Pivot", msg);
        return false;
    }
    return true;
}

// ----------------------------------------------------------------- icon ---

// Runtime-drawn tray glyph: blue rounded square, white monitor + rotation arrow.
static HICON CreatePivotIcon() {
    const int S = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = S;
    bi.bV5Height = -S;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = NULL;
    HDC hdc = GetDC(NULL);
    HBITMAP hColor = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP hMask = CreateBitmap(S, S, 1, 1, NULL);
    HDC mdc = CreateCompatibleDC(hdc);
    HDC cdc = CreateCompatibleDC(hdc);
    HBITMAP oldM = (HBITMAP)SelectObject(mdc, hColor);
    HBITMAP oldC = (HBITMAP)SelectObject(cdc, hMask);

    // Transparent + opaque-white mask, then punch the rounded-rect region.
    PatBlt(mdc, 0, 0, S, S, BLACKNESS);
    PatBlt(cdc, 0, 0, S, S, WHITENESS);
    HBRUSH blue = CreateSolidBrush(RGB(31, 110, 214));
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
    HBRUSH oldB = (HBRUSH)SelectObject(mdc, blue);
    HPEN oldP = (HPEN)SelectObject(mdc, nullPen);
    RoundRect(mdc, 1, 1, S - 1, S - 1, 9, 9);
    SelectObject(cdc, GetStockObject(BLACK_BRUSH));
    SelectObject(cdc, GetStockObject(NULL_PEN));
    RoundRect(cdc, 1, 1, S - 1, S - 1, 9, 9);

    // White monitor outline.
    SelectObject(mdc, white);
    RoundRect(mdc, 7, 8, 25, 20, 3, 3);
    HBRUSH blueIn = CreateSolidBrush(RGB(31, 110, 214));
    SelectObject(mdc, blueIn);
    RoundRect(mdc, 9, 10, 23, 18, 2, 2);
    // Stand.
    RECT stand = {14, 20, 18, 23};
    FillRect(mdc, &stand, white);
    RECT foot = {11, 23, 21, 24};
    FillRect(mdc, &foot, white);
    // Rotation arrow: arc + head in white.
    HPEN wpen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    SelectObject(mdc, wpen);
    SelectObject(mdc, GetStockObject(NULL_BRUSH));
    Arc(mdc, 10, 6, 22, 18, 20, 8, 12, 7);
    POINT tri[3] = {{19, 4}, {23, 7}, {18, 9}};
    SelectObject(mdc, white);
    Polygon(mdc, tri, 3);

    SelectObject(mdc, oldB);
    SelectObject(mdc, oldP);
    SelectObject(mdc, oldM);
    SelectObject(cdc, oldC);
    DeleteObject(blue);
    DeleteObject(white);
    DeleteObject(blueIn);
    DeleteObject(wpen);
    DeleteDC(mdc);
    DeleteDC(cdc);
    ReleaseDC(NULL, hdc);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = hColor;
    ii.hbmMask = hMask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(hColor);
    DeleteObject(hMask);
    return icon;
}

// ----------------------------------------------------------------- tray ---

static void TrayAdd() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwndTray;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_hIcon;
    wcsncpy(nid.szTip, L"Pivot \u2014 monitor rotation & arrangement", 127);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void TrayRemove() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwndTray;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowBalloon(const wchar_t* title, const wchar_t* msg) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwndTray;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_ERROR;
    wcsncpy(nid.szInfoTitle, title, 63);
    wcsncpy(nid.szInfo, msg, 255);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void DispatchCommand(int cmd) {
    if (cmd == CMD_QUIT) {
        PostMessageW(g_hwndTray, WM_CLOSE, 0, 0);
        return;
    }
    if (cmd == CMD_SETTINGS) {
        ShellExecuteW(NULL, L"open", L"ms-settings:display", NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    if (cmd == CMD_ARRANGE) {
        ShowArrangeWindow();
        return;
    }
    bool canvas = (cmd >= CMD_CANVAS_ROT_BASE);
    int base = canvas ? CMD_CANVAS_ROT_BASE : CMD_ROT_BASE;
    if (cmd >= base) {
        int idx = (cmd - base) / 16;
        int slot = (cmd - base) % 16;
        if (slot < 0 || slot > 3) return;
        std::vector<DisplayInfo> cur = SnapshotDisplays();  // fresh, not stale menu data
        if (idx < 0 || idx >= (int)cur.size()) return;
        if (!cur[idx].canRotate) {
            MessageBeep(MB_ICONASTERISK);
            return;
        }
        if (ApplyRotation(cur[idx].device, kDeg[slot])) RefreshAll();
        return;
    }
}

static HMENU BuildRotationMenu(int idx, int cmdBase) {
    const DisplayInfo& b = g_boxes[idx];
    HMENU sub = CreatePopupMenu();
    for (int s = 0; s < 4; ++s) {
        wchar_t label[64];
        if (kDeg[s] == 0)
            swprintf(label, 64, L"0\u00B0 (standard)");
        else
            swprintf(label, 64, L"%d\u00B0", kDeg[s]);
        UINT flags = MF_STRING | (b.degrees == kDeg[s] ? MF_CHECKED : MF_UNCHECKED);
        if (!b.canRotate) flags |= MF_DISABLED | MF_GRAYED;
        AppendMenuW(sub, flags, (UINT_PTR)(cmdBase + idx * 16 + s), label);
    }
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    wchar_t place[64];
    swprintf(place, 64, L"Place at %d, %d", b.x, b.y);
    AppendMenuW(sub, MF_STRING | MF_DISABLED | MF_GRAYED, 0, place);
    return sub;
}

static void ShowTrayMenu() {
    g_boxes = SnapshotDisplays();
    HMENU menu = CreatePopupMenu();

    wchar_t status[64];
    if (g_boxes.empty())
        swprintf(status, 64, L"Status: no displays");
    else
        swprintf(status, 64, L"Status: %d display%ls", (int)g_boxes.size(),
                 g_boxes.size() == 1 ? L"" : L"s");
    AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, status);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    for (int i = 0; i < (int)g_boxes.size(); ++i) {
        HMENU sub = BuildRotationMenu(i, CMD_ROT_BASE);
        std::wstring title = Describe(g_boxes[i]);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, title.c_str());
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_ARRANGE, L"Arrange Displays...");
    AppendMenuW(menu, MF_STRING, CMD_SETTINGS, L"Display Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_QUIT, L"Quit");

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwndTray);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, 0,
                             g_hwndTray, NULL);
    DestroyMenu(menu);
    if (cmd) DispatchCommand(cmd);
    // Dismissal quirk: re-assert foreground so the next click opens cleanly.
    PostMessageW(g_hwndTray, WM_NULL, 0, 0);
}

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_wmTaskbarCreated) {
        TrayAdd();
        return 0;
    }
    switch (msg) {
        case WM_APP_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu();
            return 0;
        case WM_DISPLAYCHANGE:
            // Burst-y during applies; debounce with a one-shot timer (no polling).
            SetTimer(hwnd, TIMER_REFRESH, 400, NULL);
            return 0;
        case WM_TIMER:
            if (wp == TIMER_REFRESH) {
                KillTimer(hwnd, TIMER_REFRESH);
                RefreshAll();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            TrayRemove();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RefreshAll() {
    g_boxes = SnapshotDisplays();
    if (g_hwndCanvas) InvalidateRect(g_hwndCanvas, NULL, TRUE);
}

// --------------------------------------------------------------- canvas ---

struct CanvasState {
    std::vector<DisplayInfo> boxes;
    int drag = -1;
    int hover = -1;
    POINT grabDelta = {0, 0};  // cursor minus box origin at grab (desktop px)
    bool trackLeave = false;
    HFONT hTitle = NULL;
    HFONT hMeta = NULL;
};

static CanvasState g_cv;
static const int kPad = 28;
static const int kSnapPx = 24;

struct URect {
    int x, y, w, h;
};

static URect CanvasUnion() {
    URect u{0, 0, 1, 1};
    if (g_cv.boxes.empty()) return u;
    int x0 = g_cv.boxes[0].x, y0 = g_cv.boxes[0].y;
    int x1 = x0 + g_cv.boxes[0].w, y1 = y0 + g_cv.boxes[0].h;
    for (size_t i = 1; i < g_cv.boxes.size(); ++i) {
        const auto& b = g_cv.boxes[i];
        if (b.x < x0) x0 = b.x;
        if (b.y < y0) y0 = b.y;
        if (b.x + b.w > x1) x1 = b.x + b.w;
        if (b.y + b.h > y1) y1 = b.y + b.h;
    }
    u = {x0, y0, x1 - x0, y1 - y0};
    if (u.w < 1) u.w = 1;
    if (u.h < 1) u.h = 1;
    return u;
}

static double CanvasScale(int cw, int ch) {
    URect u = CanvasUnion();
    double sx = (double)(cw > 2 * kPad ? cw - 2 * kPad : 1) / (double)u.w;
    double sy = (double)(ch > 2 * kPad ? ch - 2 * kPad : 1) / (double)u.h;
    double s = sx < sy ? sx : sy;
    return s > 0 ? s : 1;
}

static RECT CanvasRectFor(const DisplayInfo& b, int cw, int ch) {
    URect u = CanvasUnion();
    double s = CanvasScale(cw, ch);
    RECT r;
    r.left = (LONG)(kPad + (b.x - u.x) * s);
    r.top = (LONG)(kPad + (b.y - u.y) * s);
    r.right = (LONG)(r.left + b.w * s);
    r.bottom = (LONG)(r.top + b.h * s);
    return r;
}

static POINT CanvasToDesktop(POINT p, int cw, int ch) {
    URect u = CanvasUnion();
    double s = CanvasScale(cw, ch);
    POINT d;
    d.x = (LONG)(u.x + (p.x - kPad) / s);
    d.y = (LONG)(u.y + (p.y - kPad) / s);
    return d;
}

static int CanvasHitTest(POINT p, int cw, int ch) {
    for (int i = (int)g_cv.boxes.size() - 1; i >= 0; --i) {
        RECT r = CanvasRectFor(g_cv.boxes[i], cw, ch);
        if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return i;
    }
    return -1;
}

static COLORREF Mix(COLORREF a, COLORREF b, int t256) {
    int t = t256 < 0 ? 0 : (t256 > 256 ? 256 : t256);
    return RGB((GetRValue(a) * t + GetRValue(b) * (256 - t)) / 256,
               (GetGValue(a) * t + GetGValue(b) * (256 - t)) / 256,
               (GetBValue(a) * t + GetBValue(b) * (256 - t)) / 256);
}

static void CanvasPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    int cw = client.right - client.left, ch = client.bottom - client.top;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, cw > 0 ? cw : 1, ch > 0 ? ch : 1);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

    COLORREF bg = GetSysColor(COLOR_WINDOW);
    COLORREF accent = GetSysColor(COLOR_HIGHLIGHT);
    COLORREF text = GetSysColor(COLOR_WINDOWTEXT);
    COLORREF gray = GetSysColor(COLOR_GRAYTEXT);
    HBRUSH bgBrush = CreateSolidBrush(bg);
    FillRect(mem, &client, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(mem, TRANSPARENT);
    for (int i = 0; i < (int)g_cv.boxes.size(); ++i) {
        const DisplayInfo& b = g_cv.boxes[i];
        RECT r = CanvasRectFor(b, cw, ch);
        RECT inner = {r.left + 2, r.top + 2, r.right - 2, r.bottom - 2};

        COLORREF fill = (i == g_cv.drag)    ? Mix(accent, bg, 140)
                        : (i == g_cv.hover) ? Mix(accent, bg, 90)
                                            : Mix(accent, bg, 46);
        HBRUSH fillBrush = CreateSolidBrush(fill);
        HBRUSH oldBrush = (HBRUSH)SelectObject(mem, fillBrush);
        COLORREF edge = (i == g_cv.drag) ? accent
                        : b.primary      ? Mix(accent, text, 200)
                                         : Mix(accent, bg, 115);
        HPEN pen = CreatePen(PS_SOLID, (i == g_cv.drag || b.primary) ? 3 : 1, edge);
        HPEN oldPen = (HPEN)SelectObject(mem, pen);
        RoundRect(mem, inner.left, inner.top, inner.right, inner.bottom, 12, 12);
        SelectObject(mem, oldPen);
        SelectObject(mem, oldBrush);
        DeleteObject(pen);
        DeleteObject(fillBrush);

        int rw = inner.right - inner.left, rh = inner.bottom - inner.top;
        if (rw > 90 && rh > 52) {
            SetTextColor(mem, text);
            SelectObject(mem, g_cv.hTitle);
            RECT tr = {inner.left + 6, inner.top + rh / 2 - 20, inner.right - 6, inner.top + rh / 2 + 2};
            DrawTextW(mem, b.friendly.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            wchar_t meta[128];
            if (b.primary)
                swprintf(meta, 128, L"%dx%d \u00B7 %d\u00B0 \u00B7 primary", b.w, b.h, b.degrees);
            else
                swprintf(meta, 128, L"%dx%d \u00B7 %d\u00B0", b.w, b.h, b.degrees);
            SetTextColor(mem, gray);
            SelectObject(mem, g_cv.hMeta);
            RECT mr = {inner.left + 6, inner.top + rh / 2 + 2, inner.right - 6, inner.top + rh / 2 + 22};
            DrawTextW(mem, meta, -1, &mr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    const wchar_t* hint = L"Drag to arrange \u00B7 double-click or right-click a display to rotate";
    SetTextColor(mem, gray);
    SelectObject(mem, g_cv.hMeta);
    RECT hr = {0, ch - 26, cw, ch - 6};
    DrawTextW(mem, hint, -1, &hr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static POINT CanvasSnap(POINT origin, int movingIdx) {
    const DisplayInfo& mv = g_cv.boxes[movingIdx];
    int ml = origin.x, mt = origin.y, mr = origin.x + mv.w, mb = origin.y + mv.h;
    for (int i = 0; i < (int)g_cv.boxes.size(); ++i) {
        if (i == movingIdx) continue;
        const DisplayInfo& o = g_cv.boxes[i];
        int ol = o.x, ot = o.y, orr = o.x + o.w, ob = o.y + o.h;
        if (abs(mr - ol) < kSnapPx) return {ol - mv.w, origin.y};
        if (abs(ml - orr) < kSnapPx) return {orr, origin.y};
        if (abs(mb - ot) < kSnapPx) return {origin.x, ot - mv.h};
        if (abs(mt - ob) < kSnapPx) return {origin.x, ob};
    }
    return origin;
}

static void CanvasPopupRotate(HWND hwnd, int idx) {
    g_boxes = g_cv.boxes;  // BuildRotationMenu reads g_boxes
    HMENU sub = BuildRotationMenu(idx, CMD_CANVAS_ROT_BASE);
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(sub, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, NULL);
    DestroyMenu(sub);
    if (cmd) DispatchCommand(cmd);
}

static LRESULT CALLBACK CanvasWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_cv.hTitle = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_cv.hMeta = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
            g_cv.boxes = SnapshotDisplays();
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_PAINT:
            CanvasPaint(hwnd);
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* m = (MINMAXINFO*)lp;
            m->ptMinTrackSize.x = 480;
            m->ptMinTrackSize.y = 340;
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT client;
            GetClientRect(hwnd, &client);
            int i = CanvasHitTest(p, client.right, client.bottom);
            if (i < 0) break;
            if ((wp & MK_RBUTTON) == 0) {
                g_cv.drag = i;
                POINT d = CanvasToDesktop(p, client.right, client.bottom);
                g_cv.grabDelta = {d.x - g_cv.boxes[i].x, d.y - g_cv.boxes[i].y};
                SetCapture(hwnd);
                SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT client;
            GetClientRect(hwnd, &client);
            int i = CanvasHitTest(p, client.right, client.bottom);
            if (i < 0) break;
            if (!g_cv.boxes[i].canRotate) {
                MessageBeep(MB_ICONASTERISK);
                break;
            }
            int next = (g_cv.boxes[i].degrees + 90) % 360;
            if (ApplyRotation(g_cv.boxes[i].device, next)) {
                g_cv.boxes = SnapshotDisplays();
                RefreshAll();
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT client;
            GetClientRect(hwnd, &client);
            if (g_cv.drag >= 0) {
                POINT d = CanvasToDesktop(p, client.right, client.bottom);
                int nx = d.x - g_cv.grabDelta.x;
                int ny = d.y - g_cv.grabDelta.y;
                if (nx < -32000) nx = -32000;
                if (nx > 32000) nx = 32000;
                if (ny < -32000) ny = -32000;
                if (ny > 32000) ny = 32000;
                g_cv.boxes[g_cv.drag].x = nx;
                g_cv.boxes[g_cv.drag].y = ny;
                SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
                InvalidateRect(hwnd, NULL, TRUE);
            } else {
                int h = CanvasHitTest(p, client.right, client.bottom);
                if (h != g_cv.hover) {
                    g_cv.hover = h;
                    InvalidateRect(hwnd, NULL, TRUE);
                }
                SetCursor(LoadCursorW(NULL, h >= 0 ? IDC_HAND : IDC_ARROW));
                if (!g_cv.trackLeave) {
                    TRACKMOUSEEVENT t = {};
                    t.cbSize = sizeof(t);
                    t.dwFlags = TME_LEAVE;
                    t.hwndTrack = hwnd;
                    TrackMouseEvent(&t);
                    g_cv.trackLeave = true;
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_cv.trackLeave = false;
            if (g_cv.hover != -1) {
                g_cv.hover = -1;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (g_cv.drag < 0) break;
            int i = g_cv.drag;
            g_cv.drag = -1;
            ReleaseCapture();
            POINT o = CanvasSnap({g_cv.boxes[i].x, g_cv.boxes[i].y}, i);
            g_cv.boxes[i].x = o.x;
            g_cv.boxes[i].y = o.y;
            std::vector<std::pair<std::wstring, POINT>> moves;
            moves.push_back({g_cv.boxes[i].device, o});
            if (ApplyPositions(moves)) {
                g_cv.boxes = SnapshotDisplays();
                RefreshAll();
            }
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT client;
            GetClientRect(hwnd, &client);
            int i = CanvasHitTest(p, client.right, client.bottom);
            if (i >= 0) CanvasPopupRotate(hwnd, i);
            return 0;
        }
        case WM_DISPLAYCHANGE:
            g_cv.boxes = SnapshotDisplays();
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_DESTROY:
            if (g_cv.hTitle) DeleteObject(g_cv.hTitle);
            if (g_cv.hMeta) DeleteObject(g_cv.hMeta);
            g_cv.hTitle = NULL;
            g_cv.hMeta = NULL;
            g_cv.drag = -1;
            g_cv.hover = -1;
            g_hwndCanvas = NULL;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowArrangeWindow() {
    if (g_hwndCanvas) {
        if (IsIconic(g_hwndCanvas)) ShowWindow(g_hwndCanvas, SW_RESTORE);
        SetForegroundWindow(g_hwndCanvas);
        g_cv.boxes = SnapshotDisplays();
        InvalidateRect(g_hwndCanvas, NULL, TRUE);
        return;
    }
    g_hwndCanvas = CreateWindowExW(0, L"PivotCanvas", L"Pivot \u2014 Arrange Displays",
                                   WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 540,
                                   NULL, NULL, g_hInst, NULL);
    if (!g_hwndCanvas) return;
    ShowWindow(g_hwndCanvas, SW_SHOW);
    UpdateWindow(g_hwndCanvas);
}

// ------------------------------------------------------------------- cli ---

static void CliAttach() {
    // GUI-subsystem exe: no console at startup.
    // If stdout is already valid (terminal or redirected pipe/file), use it.
    // Otherwise attach to the invoker's console, or allocate one.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != NULL && out != INVALID_HANDLE_VALUE) return;
    if (GetConsoleWindow() == NULL && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    } else if (GetConsoleWindow() == NULL) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}

static int CliList() {
    std::vector<DisplayInfo> ds = SnapshotDisplays();
    wprintf(L"%d display(s):\n", (int)ds.size());
    for (int i = 0; i < (int)ds.size(); ++i) {
        const auto& d = ds[i];
        wprintf(L"[%d] %ls \"%ls\" %dx%d%+d%+d %ddeg%ls%ls\n", i, d.device.c_str(),
                d.friendly.c_str(), d.w, d.h, d.x, d.y, d.degrees,
                d.primary ? L" primary" : L"", d.canRotate ? L" rotatable" : L" fixed");
    }
    return 0;
}

static int CliMain(int argc, wchar_t** argv) {
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"--list") {
        CliAttach();
        return CliList();
    }
    if (cmd == L"--rotate" && argc == 4) {
        CliAttach();
        int idx = _wtoi(argv[2]);
        int deg = _wtoi(argv[3]);
        if (deg != 0 && deg != 90 && deg != 180 && deg != 270) {
            fwprintf(stderr, L"degrees must be one of 0 90 180 270\n");
            return 2;
        }
        std::vector<DisplayInfo> ds = SnapshotDisplays();
        if (idx < 0 || idx >= (int)ds.size()) {
            fwprintf(stderr, L"display index out of range (see --list)\n");
            return 2;
        }
        return ApplyRotation(ds[idx].device, deg) ? 0 : 1;
    }
    CliAttach();
    wprintf(L"Pivot — monitor rotation & arrangement (Windows port)\n"
            L"Usage:\n"
            L"  Pivot.exe               run tray app\n"
            L"  Pivot.exe --list        list displays\n"
            L"  Pivot.exe --rotate N D  rotate display N to D degrees (0/90/180/270)\n");
    return cmd == L"--help" ? 0 : 2;
}

// ------------------------------------------------------------------ main ---

static void EnableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL(WINAPI * SetCtxFn)(DPI_AWARENESS_CONTEXT);
        SetCtxFn fn = (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {
        int rc = CliMain(argc, argv);
        LocalFree(argv);
        return rc;
    }
    LocalFree(argv);

    HANDLE mutex = CreateMutexW(NULL, FALSE, L"PivotTrayMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_hInst = hInst;
    EnableDpiAwareness();
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInst;
    wc.lpfnWndProc = TrayWndProc;
    wc.lpszClassName = L"PivotTray";
    RegisterClassExW(&wc);

    wc.lpfnWndProc = CanvasWndProc;
    wc.lpszClassName = L"PivotCanvas";
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    g_hIcon = CreatePivotIcon();
    g_hwndTray =
        CreateWindowExW(0, L"PivotTray", L"Pivot", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwndTray) return 1;
    TrayAdd();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hIcon) DestroyIcon(g_hIcon);
    return 0;
}
