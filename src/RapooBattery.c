// RapooBattery - 极简雷柏无线鼠标电量托盘工具
// 依赖：纯 Win32 API（无 CRT 重依赖、无运行时框架）
// 原理：雷柏 2.4G 接收器的厂商 HID 集合 (UsagePage 0xFF00, Usage 0x0002, 7 字节)
//       每约 3 秒主动上报 BB ... XX，末字节即为电量百分比。
// 设计：单线程 + overlapped ReadFile 常驻挂起，空闲时几乎零 CPU 唤醒。

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <dbt.h>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#endif

#define RAPOO_VID        0x24AE
#define WM_TRAY          (WM_APP + 1)
#define WM_REFRESH_HID   (WM_APP + 2)
#define TIMER_STALE      1

#define MENU_STATE       1
#define MENU_FLOAT       2
#define MENU_AUTORUN     3
#define MENU_RECONNECT   4
#define MENU_EXIT        5

#define ICON_SIZE        32
#define BT_L             3
#define BT_T             9
#define BT_R             25
#define BT_B             23
#define NUB_L            26
#define NUB_R            28
#define NUB_T            14
#define NUB_B            18

static const WCHAR kClass[] = L"RapooBatteryWnd";
static const WCHAR kRegKey[] = L"Software\\RapooBattery";
static const WCHAR kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR kRunVal[] = L"RapooBattery";
static const WCHAR kMutex[] = L"Local\\RapooBattery_SingleInstance";

static HINSTANCE g_inst;
static HWND      g_hwnd;
static HWND      g_float;
static HANDLE    g_hid;
static HANDLE    g_hev;
static HANDLE    g_mutex;
static OVERLAPPED g_ov;
static BYTE      g_buf[64];
static BOOL      g_pending;
static int       g_level = -1;
static BOOL      g_linked;
static ULONGLONG g_lastTick;
static HICON     g_icon;
static HFONT     g_font;
static BOOL      g_showFloat;
static BOOL      g_dragging;
static POINT     g_dragOfs;
static int       g_dpi = 96;
static BOOL      g_lowNotified;
static BYTE      g_raw[16];
static int       g_rawLen;
static int       g_cachedMin;      /* 缓存读数的“分钟前” */

/* 100ns ticks per minute —— 用于把时间戳压进一个 DWORD */
#define STAMP_UNIT 600000000ULL

static DWORD stamp_now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG v = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (DWORD)(v / STAMP_UNIT);
}

static int  scale(int v) { return MulDiv(v, g_dpi, 96); }
static int  clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

/* 仅在 -DRB_DEBUG 编译时启用的诊断日志 */
#ifdef RB_DEBUG
static void dlog_num(const WCHAR* tag, int v)
{
    WCHAR path[MAX_PATH], line[192];
    DWORD n = GetTempPathW(MAX_PATH, path);
    lstrcpynW(path + n, L"raboo_dbg.log", MAX_PATH - n);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD w;
    int len = wsprintfW(line, L"%s=%d\r\n", tag, v);
    WriteFile(f, line, (DWORD)len * sizeof(WCHAR), &w, NULL);
    CloseHandle(f);
}
#define DLOG(tag, val) dlog_num(tag, val)
#else
#define DLOG(tag, val) ((void)0)
#endif

/* ---------------- 注册表设置 ---------------- */

static DWORD reg_get(const WCHAR* name, DWORD def)
{
    DWORD val = def, sz = sizeof(val), type = 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, name, NULL, &type, (LPBYTE)&val, &sz) != ERROR_SUCCESS)
            val = def;
        RegCloseKey(k);
    }
    return val;
}

static void reg_set(const WCHAR* name, DWORD val)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)
        == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        RegCloseKey(k);
    }
}

static BOOL autostart_enabled(void)
{
    HKEY k;
    BOOL on = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        WCHAR buf[512];
        DWORD sz = sizeof(buf), type = 0;
        on = (RegQueryValueExW(k, kRunVal, NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS);
        RegCloseKey(k);
    }
    return on;
}

static void autostart_apply(BOOL on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)
        != ERROR_SUCCESS)
        return;
    if (on) {
        WCHAR path[MAX_PATH + 8];
        DWORD n = GetModuleFileNameW(NULL, path + 1, MAX_PATH);
        path[0] = L'"';
        path[n + 1] = L'"';
        path[n + 2] = 0;
        RegSetValueExW(k, kRunVal, 0, REG_SZ, (const BYTE*)path,
                       (DWORD)((n + 3) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(k, kRunVal);
    }
    RegCloseKey(k);
}

/* ---------------- HID ---------------- */

static void hid_close(void)
{
    if (g_hid) {
        CancelIo(g_hid);
        CloseHandle(g_hid);
        g_hid = NULL;
    }
    g_pending = FALSE;
    g_linked = FALSE;
}

static HANDLE hid_open(void)
{
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO hdi = SetupDiGetClassDevsW(&guid, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdi == INVALID_HANDLE_VALUE)
        return NULL;

    HANDLE found = NULL;
    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);
    BYTE detail[1024];

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hdi, NULL, &guid, i, &did); i++) {
        DWORD need = 0;
        if (!SetupDiGetDeviceInterfaceDetailW(hdi, &did, NULL, 0, &need, NULL) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (need == 0 || need > sizeof(detail))
            continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pd =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)detail;
        pd->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hdi, &did, pd, need, &need, NULL))
            continue;

        HANDLE h = CreateFileW(pd->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            DLOG(L"createfile_err", (int)GetLastError());
            continue;
        }

        HIDD_ATTRIBUTES at;
        at.Size = sizeof(at);
        if (!HidD_GetAttributes(h, &at) || at.VendorID != RAPOO_VID) {
            CloseHandle(h);
            continue;
        }

        BOOL ok = FALSE;
        PHIDP_PREPARSED_DATA pp = NULL;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                DLOG(L"caps_page", caps.UsagePage);
                DLOG(L"caps_inlen", caps.InputReportByteLength);
                ok = (caps.UsagePage == 0xFF00 && caps.InputReportByteLength == 7);
            }
            HidD_FreePreparsedData(pp);
        }
        if (ok) {
            DLOG(L"matched", 1);
            found = h;
            break;
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(hdi);
    return found;
}

static void hid_arm(void);

static void hid_lost(void)
{
    hid_close();
    InvalidateRect(g_float, NULL, TRUE);
    PostMessage(g_hwnd, WM_REFRESH_HID, 0, 0);
}

/* ---------------- 图标绘制 ---------------- */

static void put_px(DWORD* px, int x, int y, DWORD c)
{
    if (x < 0 || y < 0 || x >= ICON_SIZE || y >= ICON_SIZE)
        return;
    px[y * ICON_SIZE + x] = c;
}

/* 返回值按 0xAARRGGBB 组织，写入 32bpp DIB 时内存序自动为 B,G,R,A */
static DWORD level_color(int level)
{
    if (level < 0)
        return 0xFF8C8C8C;          /* 灰：未知 / 休眠 */
    if (level <= 20)
        return 0xFFEB3B3B;          /* 红  R=EB G=3B B=3B */
    if (level <= 50)
        return 0xFFF5A51E;          /* 橙  R=F5 G=A5 B=1E */
    return 0xFF3CC83C;              /* 绿  R=3C G=C8 B=3C */
}

/* 按百分比压暗颜色（100 = 不变），用于表示“非实时” */
static DWORD shade(DWORD c, int pct)
{
    int r = (int)((c >> 16) & 0xFF) * pct / 100;
    int g = (int)((c >> 8) & 0xFF) * pct / 100;
    int b = (int)(c & 0xFF) * pct / 100;
    return 0xFF000000u | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
}

static HICON build_icon(int level, BOOL dim)
{
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = ICON_SIZE;
    bmi.bmiHeader.biHeight = -ICON_SIZE;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits)
        return NULL;

    DWORD* px = (DWORD*)bits;
    ZeroMemory(px, ICON_SIZE * ICON_SIZE * 4);

    DWORD c = level_color(level);
    if (dim)
        c = shade(c, 50);

    /* 电池外壳：2px 边框 */
    for (int x = BT_L; x <= BT_R; x++) {
        put_px(px, x, BT_T, c);
        put_px(px, x, BT_T + 1, c);
        put_px(px, x, BT_B, c);
        put_px(px, x, BT_B - 1, c);
    }
    for (int y = BT_T; y <= BT_B; y++) {
        put_px(px, BT_L, y, c);
        put_px(px, BT_L + 1, y, c);
        put_px(px, BT_R, y, c);
        put_px(px, BT_R - 1, y, c);
    }
    /* 正极触点 */
    for (int x = NUB_L; x <= NUB_R; x++)
        for (int y = NUB_T; y <= NUB_B; y++)
            put_px(px, x, y, c);

    /* 电量填充 */
    int l = clamp100(level);
    int il = BT_L + 3, ir = BT_R - 3;
    int it = BT_T + 3, ib = BT_B - 3;
    int w = ir - il + 1;
    int fill = (level < 0) ? 0 : (w * l + 99) / 100;
    for (int x = il; x < il + fill && x <= ir; x++)
        for (int y = it; y <= ib; y++)
            put_px(px, x, y, c);

    /* AND 掩码必须全 0（全部交给 alpha 通道），否则图标会被随机裁掉 */
    BYTE maskBits[ICON_SIZE * 4];
    ZeroMemory(maskBits, sizeof(maskBits));
    HBITMAP hbmMask = CreateBitmap(ICON_SIZE, ICON_SIZE, 1, 1, maskBits);
    ICONINFO ii;
    ii.fIcon = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask = hbmMask;
    ii.hbmColor = hbm;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(hbm);
    if (hbmMask)
        DeleteObject(hbmMask);
    return ic;
}

/* ---------------- 界面刷新 ---------------- */

static void tray_show(BOOL addOrModify)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    if (g_level < 0) {
        lstrcpyW(nid.szTip, g_hid ? L"雷柏无线鼠标  等待鼠标唤醒…"
                                  : L"雷柏无线鼠标  未检测到接收器");
    } else if (g_linked) {
        wsprintfW(nid.szTip, L"雷柏无线鼠标  电量 %d%%", g_level);
    } else if (g_cachedMin >= 120) {
        wsprintfW(nid.szTip, L"雷柏无线鼠标  %d%%（休眠 · %d 小时前读数）",
                  g_level, g_cachedMin / 60);
    } else if (g_cachedMin > 0) {
        wsprintfW(nid.szTip, L"雷柏无线鼠标  %d%%（休眠 · %d 分钟前读数）",
                  g_level, g_cachedMin);
    } else {
        wsprintfW(nid.szTip, L"雷柏无线鼠标  %d%%（休眠）", g_level);
    }
    Shell_NotifyIconW(addOrModify ? NIM_MODIFY : NIM_ADD, &nid);
}

static void tray_update(void)
{
    HICON old = g_icon;
    g_icon = build_icon(g_level, !g_linked);
    if (!g_icon) {
        g_icon = old;
        return;
    }
    tray_show(TRUE);
    if (old)
        DestroyIcon(old);
    if (g_float && IsWindowVisible(g_float))
        InvalidateRect(g_float, NULL, TRUE);
}

static void set_level(int level)
{
    if (level <= 20 && !g_lowNotified && level >= 0 && g_linked) {
        NOTIFYICONDATAW nid;
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_WARNING;
        wsprintfW(nid.szInfoTitle, L"雷柏鼠标电量低");
        wsprintfW(nid.szInfo, L"剩余 %d%%，建议充电。", level);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        g_lowNotified = TRUE;
    }
    if (level > 25)
        g_lowNotified = FALSE;

    if (level != g_level) {
        g_level = level;
        tray_update();
    }
}

/* 解析一帧厂商报告：BB ... XX，末字节为电量百分比 */
static void handle_report(const BYTE* b, DWORD len)
{
    if (len < 2 || b[0] != 0xBB)
        return;
    int lv = b[len - 1];
    if (lv < 0 || lv > 100)
        return;

    BOOL wasLinked = g_linked;
    g_linked = TRUE;
    g_lastTick = GetTickCount64();
    g_rawLen = (int)len;
    if (g_rawLen > (int)sizeof(g_raw))
        g_rawLen = (int)sizeof(g_raw);
    CopyMemory(g_raw, b, (SIZE_T)g_rawLen);
    g_cachedMin = 0;
    reg_set(L"Level", (DWORD)lv);
    reg_set(L"Stamp", stamp_now());
    set_level(lv);
    if (!wasLinked)
        tray_update();
}

static void hid_on_read(void)
{
    DWORD got = 0;
    if (!g_hid) {
        g_pending = FALSE;
        return;
    }
    if (!GetOverlappedResult(g_hid, &g_ov, &got, FALSE)) {
        DLOG(L"overlapped_err", (int)GetLastError());
        hid_lost();
        return;
    }
    DLOG(L"report_len", (int)got);
    handle_report(g_buf, got);
    hid_arm();
}

static void hid_arm(void)
{
    if (!g_hid)
        return;
    ZeroMemory(&g_ov, sizeof(g_ov));
    g_ov.hEvent = g_hev;
    ResetEvent(g_hev);
    g_pending = TRUE;
    if (!ReadFile(g_hid, g_buf, sizeof(g_buf), NULL, &g_ov)) {
        DWORD e = GetLastError();
        DLOG(L"readfile_err", (int)e);
        if (e != ERROR_IO_PENDING) {
            g_pending = FALSE;
            hid_lost();
        }
    }
}

static void try_connect(void)
{
    if (g_hid)
        return;
    HANDLE h = hid_open();
    if (!h) {
        if (g_linked || g_level >= 0) {
            g_linked = FALSE;
            tray_update();
        }
        return;
    }
    g_hid = h;
    g_lastTick = GetTickCount64();
    hid_arm();
    tray_update();
}

/* ---------------- 浮窗 ---------------- */

static void float_place_default(void)
{
    int w = scale(80), h = scale(36);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(g_float, HWND_TOPMOST, sw - w - scale(28), sh - h - scale(96),
                 w, h, SWP_NOACTIVATE);
}

static void float_apply_visible(void)
{
    if (g_showFloat) {
        ShowWindow(g_float, SW_SHOWNOACTIVATE);
        SetWindowPos(g_float, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(g_float, NULL, TRUE);
    } else {
        ShowWindow(g_float, SW_HIDE);
    }
}

static void float_save_pos(void)
{
    RECT r;
    if (GetWindowRect(g_float, &r)) {
        reg_set(L"FloatX", (DWORD)r.left);
        reg_set(L"FloatY", (DWORD)r.top);
    }
}

static void float_restore_pos(void)
{
    DWORD fx = reg_get(L"FloatX", 0xFFFFFFFF), fy = reg_get(L"FloatY", 0xFFFFFFFF);
    int w = scale(80), h = scale(36);
    if (fx == 0xFFFFFFFF || fy == 0xFFFFFFFF) {
        float_place_default();
        return;
    }
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if ((int)fx > sw - scale(20) || (int)fy > sh - scale(10) || (int)fx < -scale(40)) {
        float_place_default();
        return;
    }
    SetWindowPos(g_float, HWND_TOPMOST, (int)fx, (int)fy, w, h, SWP_NOACTIVATE);
}

static void float_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    HBRUSH bg = CreateSolidBrush(RGB(26, 26, 30));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    DWORD c = level_color(g_level);
    if (!g_linked)
        c = shade(c, 62);           /* 缓存/休眠：压暗，避免看起来像实时值 */
    COLORREF accent = RGB((BYTE)((c >> 16) & 0xFF), (BYTE)((c >> 8) & 0xFF), (BYTE)(c & 0xFF));
    HBRUSH bd = CreateSolidBrush(accent);
    FrameRect(dc, &rc, bd);
    DeleteObject(bd);

    WCHAR txt[16];
    if (g_level < 0)
        lstrcpyW(txt, L"--");
    else
        wsprintfW(txt, L"%d%%", g_level);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, accent);
    HGDIOBJ old = SelectObject(dc, g_font);
    RECT tr = rc;
    DrawTextW(dc, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    EndPaint(hwnd, &ps);
}

/* ---------------- 菜单 ---------------- */

static void show_menu(void)
{
    HMENU m = CreatePopupMenu();
    WCHAR t[96];
    if (g_level < 0)
        lstrcpyW(t, g_linked ? L"雷柏无线鼠标  读取中…" : L"雷柏无线鼠标  未连接");
    else
        wsprintfW(t, L"雷柏无线鼠标  电量 %d%%%s", g_level, g_linked ? L"" : L"（休眠）");
    AppendMenuW(m, MF_STRING | MF_GRAYED, MENU_STATE, t);
    if (g_rawLen > 0) {
        WCHAR r[64], *p = r;
        p += wsprintfW(p, L"报文 ");
        for (int i = 0; i < g_rawLen && i < 8; i++)
            p += wsprintfW(p, L"%02X ", g_raw[i]);
        AppendMenuW(m, MF_STRING | MF_GRAYED, MENU_STATE + 100, r);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_showFloat ? MF_CHECKED : 0), MENU_FLOAT, L"桌面浮窗");
    AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), MENU_AUTORUN,
                L"开机自动启动");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, MENU_RECONNECT, L"重新连接");
    AppendMenuW(m, MF_STRING, MENU_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessage(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ---------------- 窗口过程 ---------------- */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (hwnd == g_float) {
        switch (msg) {
        case WM_PAINT:
            float_paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
            g_dragging = TRUE;
            SetCapture(hwnd);
            {
                POINT p;
                GetCursorPos(&p);
                g_dragOfs.x = p.x;
                g_dragOfs.y = p.y;
            }
            return 0;
        case WM_MOUSEMOVE:
            if (g_dragging) {
                POINT p;
                RECT r;
                GetCursorPos(&p);
                GetWindowRect(hwnd, &r);
                SetWindowPos(hwnd, HWND_TOPMOST,
                             r.left + (p.x - g_dragOfs.x),
                             r.top + (p.y - g_dragOfs.y),
                             0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                g_dragOfs = p;
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_dragging) {
                g_dragging = FALSE;
                ReleaseCapture();
                float_save_pos();
            }
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            show_menu();
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
#ifdef RB_SIM
    case WM_TIMER:
        if (wp == 9) {
            static const BYTE kFake[7] = { 0xBB, 0xB0, 0x02, 0x72, 0x06, 0x01, 0x5D };
            handle_report(kFake, 7);
        }
        return 0;
#endif
    case WM_TRAY:
        switch (LOWORD((DWORD)lp)) {
        case WM_LBUTTONUP:
            g_showFloat = !g_showFloat;
            reg_set(L"Float", g_showFloat ? 1 : 0);
            float_apply_visible();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            show_menu();
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD((DWORD)wp)) {
        case MENU_FLOAT:
            g_showFloat = !g_showFloat;
            reg_set(L"Float", g_showFloat ? 1 : 0);
            float_apply_visible();
            break;
        case MENU_AUTORUN:
            autostart_apply(autostart_enabled() ? FALSE : TRUE);
            break;
        case MENU_RECONNECT:
            hid_close();
            try_connect();
            break;
        case MENU_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL)
            PostMessage(hwnd, WM_REFRESH_HID, 0, 0);
        else if (wp == DBT_DEVICEREMOVECOMPLETE)
            hid_close(), tray_update();
        return 0;

    case WM_REFRESH_HID:
        try_connect();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_ENDSESSION:
        return 0;

    default:
        /* 资源管理器重启后重建托盘图标 */
        {
            static UINT s_taskbarCreated;
            if (!s_taskbarCreated)
                s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
            if (msg == s_taskbarCreated) {
                tray_show(FALSE);
                return 0;
            }
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------- 入口 ---------------- */

#ifndef RB_ICON_TEST
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;

    /* 单实例 */
    g_mutex = CreateMutexW(NULL, TRUE, kMutex);
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    SetProcessDPIAware();
    {
        HDC sdc = GetDC(NULL);
        g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
        ReleaseDC(NULL, sdc);
    }
    if (g_dpi < 96)
        g_dpi = 96;

    g_hev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_hev)
        return 1;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, kClass, L"RapooBattery", WS_OVERLAPPED,
                             0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_hwnd)
        return 1;

    /* 浮窗 */
    g_showFloat = reg_get(L"Float", 1) ? TRUE : FALSE;

    /* 上次读数缓存（开机 / 鼠标休眠时也有东西可看） */
    {
        DWORD lv = reg_get(L"Level", 0xFFFFFFFF);
        if (lv != 0xFFFFFFFF && lv <= 100) {
            DWORD now = stamp_now();
            DWORD saved = reg_get(L"Stamp", now);
            g_level = (int)lv;
            g_cachedMin = (now > saved) ? (int)(now - saved) : 0;
        }
    }
    g_float = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kClass, L"RapooBatteryFloat", WS_POPUP,
                              0, 0, scale(80), scale(36), NULL, NULL, inst, NULL);
    g_font = CreateFontW(-scale(20), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!g_font)
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_icon = build_icon(g_level, TRUE);
    tray_show(FALSE);

    float_restore_pos();
    float_apply_visible();

    try_connect();
    if (!g_hid)
        tray_update();
#ifdef RB_SIM
    SetTimer(g_hwnd, 9, 1500, NULL);
#endif

    for (;;) {
        DWORD waitMs = g_hid ? 5000 : 2000;
        DWORD count = (g_hid && g_pending) ? 1 : 0;
        DWORD r = MsgWaitForMultipleObjects(count, count ? &g_hev : NULL, FALSE,
                                           waitMs, QS_ALLINPUT);

        if (count && r == WAIT_OBJECT_0) {
            hid_on_read();
            continue;
        }
        if (r == WAIT_TIMEOUT) {
            if (!g_hid) {
                try_connect();
            } else if (g_linked && GetTickCount64() - g_lastTick > 12000) {
                g_linked = FALSE;
                tray_update();
            }
            continue;
        }
        if (r == WAIT_OBJECT_0 + count || r == WAIT_OBJECT_0) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT)
                    goto done;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            continue;
        }
        if (r == WAIT_FAILED) {
            hid_close();
            Sleep(500);
        }
    }

done:
    hid_close();
    {
        NOTIFYICONDATAW nid;
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_hwnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    if (g_icon)
        DestroyIcon(g_icon);
    if (g_hev)
        CloseHandle(g_hev);
    if (g_mutex)
        CloseHandle(g_mutex);
    return 0;
}
#endif /* RB_ICON_TEST */
