// RapooBattery - 无线鼠标电量托盘工具（多品牌）
// 依赖：纯 Win32 API，无运行时框架
// 结构：providers.c 负责各品牌协议；本文件负责界面与消息循环
// 界面：托盘电池图标 + 圆形浮窗（环形进度 + 中心百分比）

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <math.h>

#include "providers.h"

#define WM_TRAY          (WM_APP + 1)
#define WM_RESCAN        (WM_APP + 2)

#define MENU_STATE       1
#define MENU_FLOAT       2
#define MENU_AUTORUN     3
#define MENU_RESCAN      4
#define MENU_EXIT        5
#define MENU_PROTO       6
#define MENU_DIAG        7

/* 托盘图标：深色圆角徽章 + 彩色电量数字。
   托盘实际只显示 16px，画电池外形再塞数字必然糊成一团，
   所以这里直接让「数字」当主角，用 3x5 点阵放大绘制，笔画干净。 */
#define ICON_MAX         32     /* 支持的最大图标边长（高 DPI） */
#define DIG_W            3      /* 点阵字宽（像素） */
#define DIG_H            5      /* 点阵字高（像素） */

#define FLOAT_DIA        62     /* 圆形浮窗逻辑直径 */
#define SS               4      /* 超采样倍数（抗锯齿） */

static const WCHAR kClass[] = L"RapooBatteryWnd";
static const WCHAR kRegKey[] = L"Software\\RapooBattery";
static const WCHAR kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR kRunVal[] = L"RapooBattery";
static const WCHAR kMutex[] = L"Local\\RapooBattery_SingleInstance";

static HINSTANCE g_inst;
static HWND      g_hwnd;
static HWND      g_float;
static HANDLE    g_mutex;
static int       g_level = -1;
static int       g_charging = -1;
static BOOL      g_linked;
static ULONGLONG g_lastTick;
static ULONGLONG g_lastScan;
static ULONGLONG g_nextPoll;
static HICON     g_icon;
static HFONT     g_font;
static BOOL      g_showFloat;
static BOOL      g_dragging;
static POINT     g_dragOfs;
static int       g_dpi = 96;
static BOOL      g_lowNotified;
static int       g_cachedMin;
static int       g_dia = FLOAT_DIA;

#define STAMP_UNIT 600000000ULL

static DWORD stamp_now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG v = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (DWORD)(v / STAMP_UNIT);
}

static int scale(int v) { return MulDiv(v, g_dpi, 96); }
static int clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

/* ---------------- 注册表 ---------------- */

static DWORD reg_get(const WCHAR *name, DWORD def)
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

static void reg_set(const WCHAR *name, DWORD val)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)
        == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
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
        RegSetValueExW(k, kRunVal, 0, REG_SZ, (const BYTE *)path,
                       (DWORD)((n + 3) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(k, kRunVal);
    }
    RegCloseKey(k);
}

/* ---------------- 托盘图标 ---------------- */

/* 3x5 点阵，每字节低 3 位是 3 个横向像素（bit2 在左） */
static const BYTE kGlyph[11][DIG_H] = {
    { 7, 5, 5, 5, 7 },   /* 0 */
    { 2, 6, 2, 2, 7 },   /* 1 */
    { 7, 1, 7, 4, 7 },   /* 2 */
    { 7, 1, 7, 1, 7 },   /* 3 */
    { 5, 5, 7, 1, 1 },   /* 4 */
    { 7, 4, 7, 1, 7 },   /* 5 */
    { 7, 4, 7, 5, 7 },   /* 6 */
    { 7, 1, 1, 1, 1 },   /* 7 */
    { 7, 5, 7, 5, 7 },   /* 8 */
    { 7, 5, 7, 1, 7 },   /* 9 */
    { 0, 0, 7, 0, 0 }    /* - （未知） */
};

static void put_px(DWORD *px, int s, int x, int y, DWORD c)
{
    if (x < 0 || y < 0 || x >= s || y >= s)
        return;
    px[y * s + x] = c;
}

static DWORD level_color(int level)
{
    if (level < 0)
        return 0xFF8C8C8C;
    if (level <= 20)
        return 0xFFEB3B3B;
    if (level <= 50)
        return 0xFFF5A51E;
    return 0xFF3CC83C;
}

static DWORD shade(DWORD c, int pct)
{
    int r = (int)((c >> 16) & 0xFF) * pct / 100;
    int g = (int)((c >> 8) & 0xFF) * pct / 100;
    int b = (int)(c & 0xFF) * pct / 100;
    return 0xFF000000u | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
}

static int icon_size(void)
{
    int s = GetSystemMetrics(SM_CXSMICON);
    if (s < 16)
        s = 16;
    if (s > ICON_MAX)
        s = ICON_MAX;
    return s;
}

/* 圆角矩形的裁剪判断，四角按 1/4 圆处理 */
static BOOL in_round(int x, int y, int s, int r)
{
    int cx, cy;
    if (x < r && y < r)                { cx = r;         cy = r;         }
    else if (x >= s - r && y < r)      { cx = s - r - 1; cy = r;         }
    else if (x < r && y >= s - r)      { cx = r;         cy = s - r - 1; }
    else if (x >= s - r && y >= s - r) { cx = s - r - 1; cy = s - r - 1; }
    else return TRUE;

    int dx = x - cx, dy = y - cy;
    return (dx * dx + dy * dy) <= r * r;
}

static void draw_digits(DWORD *px, int s, const WCHAR *t, int n, int k, DWORD color)
{
    int gw  = DIG_W * k;
    int w   = n * gw + (n - 1);          /* 字间留 1 像素 */
    int h   = DIG_H * k;
    int x0  = (s - w) / 2;
    int y0  = (s - h) / 2;

    for (int i = 0; i < n; i++) {
        int gi = 10;
        if (t[i] >= L'0' && t[i] <= L'9')
            gi = (int)(t[i] - L'0');
        const BYTE *g = kGlyph[gi];
        int bx = x0 + i * (gw + 1);

        for (int row = 0; row < DIG_H; row++) {
            for (int col = 0; col < DIG_W; col++) {
                if (!(g[row] & (1 << (DIG_W - 1 - col))))
                    continue;
                for (int sy = 0; sy < k; sy++)
                    for (int sx = 0; sx < k; sx++)
                        put_px(px, s, bx + col * k + sx, y0 + row * k + sy, color);
            }
        }
    }
}

static HICON build_icon(int level, BOOL dim)
{
    const int S = icon_size();

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = S;
    bmi.bmiHeader.biHeight = -S;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits)
        return NULL;

    DWORD *px = (DWORD *)bits;
    ZeroMemory(px, (SIZE_T)S * S * 4);

    DWORD fg = level_color(level);
    if (dim)
        fg = shade(fg, 55);
    DWORD bg = dim ? 0xFF343434u : 0xFF1C1C1Cu;

    /* 深色徽章底：亮色数字在浅色/深色任务栏上都能读 */
    int r = (S >= 28) ? 4 : (S >= 20 ? 3 : 2);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++)
            if (in_round(x, y, S, r))
                put_px(px, S, x, y, bg);

    /* 文本：两位数字为主，满电三位，未知显示 "--" */
    WCHAR t[4];
    int n;
    if (level < 0) {
        t[0] = L'-'; t[1] = L'-'; n = 2;
    } else {
        int lv = clamp100(level);
        if (lv >= 100) {
            t[0] = L'1'; t[1] = L'0'; t[2] = L'0'; n = 3;
        } else if (lv >= 10) {
            t[0] = (WCHAR)(L'0' + lv / 10);
            t[1] = (WCHAR)(L'0' + lv % 10);
            n = 2;
        } else {
            t[0] = (WCHAR)(L'0' + lv); n = 1;
        }
    }

    /* 取能放下的最大整数倍，保证点阵不失真 */
    int margin = (S <= 20) ? 1 : (S <= 28 ? 2 : 3);
    int avail  = S - 2 * margin;
    int k = 1;
    for (int kk = 8; kk >= 2; kk--) {
        int w = n * DIG_W * kk + (n - 1);
        int h = DIG_H * kk;
        if (w <= avail && h <= avail) {
            k = kk;
            break;
        }
    }
    draw_digits(px, S, t, n, k, fg);

    BYTE maskBits[ICON_MAX * 4];
    ZeroMemory(maskBits, sizeof(maskBits));
    HBITMAP hbmMask = CreateBitmap(S, S, 1, 1, maskBits);

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = hbmMask;
    ii.hbmColor = hbm;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(hbm);
    if (hbmMask)
        DeleteObject(hbmMask);
    return ic;
}

/* ---------------- 托盘 ---------------- */

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

    const WCHAR *nm = prov_name();
    if (!nm || !nm[0])
        nm = L"无线设备";

    if (g_level < 0) {
        wsprintfW(nid.szTip, L"%s  等待数据…", nm);
    } else if (g_linked) {
        wsprintfW(nid.szTip, L"%s  电量 %d%%%s", nm, g_level,
                  g_charging == 1 ? L"（充电中）" : L"");
    } else if (g_cachedMin >= 120) {
        wsprintfW(nid.szTip, L"%s  %d%%（%d 小时前读数）", nm, g_level, g_cachedMin / 60);
    } else if (g_cachedMin > 0) {
        wsprintfW(nid.szTip, L"%s  %d%%（%d 分钟前读数）", nm, g_level, g_cachedMin);
    } else {
        wsprintfW(nid.szTip, L"%s  %d%%（缓存）", nm, g_level);
    }
    Shell_NotifyIconW(addOrModify ? NIM_MODIFY : NIM_ADD, &nid);
}

static void float_render(void);

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
        float_render();
}

static void set_level(int level, int charging)
{
    if (level <= 20 && !g_lowNotified && level >= 0 && g_linked) {
        NOTIFYICONDATAW nid;
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_WARNING;
        const WCHAR *nm = prov_name();
        wsprintfW(nid.szInfoTitle, L"%s 电量低", (nm && nm[0]) ? nm : L"无线鼠标");
        wsprintfW(nid.szInfo, L"剩余 %d%%，建议充电。", level);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        g_lowNotified = TRUE;
    }
    if (level > 25)
        g_lowNotified = FALSE;

    if (level != g_level || charging != g_charging) {
        g_level = level;
        g_charging = charging;
        tray_update();
    }
}

/* ---------------- 圆形浮窗 ---------------- */

static void ring_dot(HDC dc, HBRUSH br, int cx, int cy, int rad)
{
    SelectObject(dc, br);
    SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - rad, cy - rad, cx + rad + 1, cy + rad + 1);
}

static void float_render(void)
{
    if (!g_float)
        return;

    int d = g_dia;
    int big = d * SS;
    int c = big / 2;

    HDC screen = GetDC(NULL);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = big;
    bi.bmiHeader.biHeight = -big;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *srcBits = NULL;
    HBITMAP srcBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &srcBits, NULL, 0);
    if (!srcBmp || !srcBits) {
        if (srcBmp)
            DeleteObject(srcBmp);
        ReleaseDC(NULL, screen);
        return;
    }
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ oldBmp = SelectObject(mem, srcBmp);

    /* 底色 */
    RECT full = { 0, 0, big, big };
    HBRUSH bgb = CreateSolidBrush(RGB(0x1A, 0x1C, 0x21));
    FillRect(mem, &full, bgb);
    DeleteObject(bgb);

    DWORD acc = level_color(g_level);
    if (!g_linked)
        acc = shade(acc, 62);
    COLORREF accent = RGB((BYTE)(acc & 0xFF), (BYTE)((acc >> 8) & 0xFF), (BYTE)((acc >> 16) & 0xFF));

    int thickness = scale(5) * SS;
    int dotRad = thickness / 2;
    int ro = c - scale(2) * SS - dotRad;
    int sweep = (g_level < 0) ? 0 : clamp100(g_level) * 360 / 100;

    /* 轨道 */
    HBRUSH track = CreateSolidBrush(g_linked ? RGB(0x2E, 0x31, 0x38) : RGB(0x26, 0x28, 0x2D));
    for (int a = 0; a < 360; a += 3) {
        double th = (a - 90.0) * 3.14159265358979 / 180.0;
        ring_dot(mem, track, c + (int)(ro * cos(th)), c + (int)(ro * sin(th)), dotRad);
    }
    DeleteObject(track);

    /* 进度弧：12 点起顺时针 */
    if (sweep > 0) {
        HBRUSH prog = CreateSolidBrush(accent);
        for (int a = 0; a <= sweep; a += 3) {
            double th = (a - 90.0) * 3.14159265358979 / 180.0;
            ring_dot(mem, prog, c + (int)(ro * cos(th)), c + (int)(ro * sin(th)), dotRad);
        }
        /* 末端补一个点，保证小数值也能看到起笔 */
        double th = (sweep - 90.0) * 3.14159265358979 / 180.0;
        ring_dot(mem, prog, c + (int)(ro * cos(th)), c + (int)(ro * sin(th)), dotRad);
        DeleteObject(prog);
    }

    /* 中心数字 */
    WCHAR txt[16];
    if (g_level < 0)
        lstrcpyW(txt, L"--");
    else
        wsprintfW(txt, L"%d", g_level);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, accent);
    HGDIOBJ oldFont = SelectObject(mem, g_font);
    RECT tr = { 0, 0, big, big };
    DrawTextW(mem, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, oldFont);

    /* ---- 降采样 + 圆形 alpha + 预乘 ---- */
    BITMAPINFO bd;
    ZeroMemory(&bd, sizeof(bd));
    bd.bmiHeader = bi.bmiHeader;
    bd.bmiHeader.biWidth = d;
    bd.bmiHeader.biHeight = -d;

    void *dstBits = NULL;
    HBITMAP dstBmp = CreateDIBSection(screen, &bd, DIB_RGB_COLORS, &dstBits, NULL, 0);
    if (dstBmp && dstBits) {
        DWORD *s = (DWORD *)srcBits;
        DWORD *o = (DWORD *)dstBits;
        double rad = big / 2.0 - scale(2) * SS;    /* 圆盘半径（环外缘） */
        double cx0 = big / 2.0, cy0 = big / 2.0;

        for (int y = 0; y < d; y++) {
            for (int x = 0; x < d; x++) {
                int sr = 0, sg = 0, sb = 0;
                double cov = 0.0;

                for (int sy = 0; sy < SS; sy++) {
                    for (int sx = 0; sx < SS; sx++) {
                        int px = x * SS + sx, py = y * SS + sy;
                        DWORD v = s[py * big + px];
                        sb += (int)(v & 0xFF);
                        sg += (int)((v >> 8) & 0xFF);
                        sr += (int)((v >> 16) & 0xFF);

                        double ddx = (px + 0.5) - cx0;
                        double ddy = (py + 0.5) - cy0;
                        double dist = sqrt(ddx * ddx + ddy * ddy);
                        double t = rad - dist + 0.5;      /* 边缘软化 1 像素 */
                        if (t < 0.0) t = 0.0;
                        if (t > 1.0) t = 1.0;
                        cov += t;
                    }
                }

                int n = SS * SS;
                int a = (int)(cov / n * 255.0 + 0.5);
                if (a <= 0) {
                    o[y * d + x] = 0;
                    continue;
                }
                int r = sr / n, g = sg / n, b = sb / n;
                r = r * a / 255;
                g = g * a / 255;
                b = b * a / 255;
                o[y * d + x] = ((DWORD)a << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
            }
        }

        HDC dst = CreateCompatibleDC(screen);
        HGDIOBJ od = SelectObject(dst, dstBmp);
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        SIZE sz;
        sz.cx = d;
        sz.cy = d;
        POINT po = { 0, 0 };
        UpdateLayeredWindow(g_float, screen, NULL, &sz, dst, &po, 0, &bf, ULW_ALPHA);
        SelectObject(dst, od);
        DeleteDC(dst);
    }

    if (dstBmp)
        DeleteObject(dstBmp);
    SelectObject(mem, oldBmp);
    DeleteDC(mem);
    DeleteObject(srcBmp);
    ReleaseDC(NULL, screen);
}

static void float_place_default(void)
{
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(g_float, HWND_TOPMOST, sw - g_dia - scale(30), sh - g_dia - scale(110),
                 g_dia, g_dia, SWP_NOACTIVATE);
}

static void float_apply_visible(void)
{
    if (g_showFloat) {
        ShowWindow(g_float, SW_SHOWNOACTIVATE);
        SetWindowPos(g_float, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        float_render();
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
    if (fx == 0xFFFFFFFF || fy == 0xFFFFFFFF) {
        float_place_default();
        return;
    }
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if ((int)fx > sw - scale(20) || (int)fy > sh - scale(10) || (int)fx < -scale(40)) {
        float_place_default();
        return;
    }
    SetWindowPos(g_float, HWND_TOPMOST, (int)fx, (int)fy, g_dia, g_dia, SWP_NOACTIVATE);
}

/* ---------------- 菜单 ---------------- */

static void export_diag(void)
{
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    WCHAR *slash = NULL;
    for (DWORD i = 0; i < n; i++)
        if (path[i] == L'\\')
            slash = path + i;
    if (slash)
        slash[1] = 0;
    else
        path[0] = 0;
    lstrcatW(path, L"RapooBattery-diag.txt");

    /* 托盘气泡提示，不弹模态对话框 */
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;

    if (prov_diag(path)) {
        nid.dwInfoFlags = NIIF_INFO;
        lstrcpyW(nid.szInfoTitle, L"诊断信息已导出");
        lstrcpynW(nid.szInfo, path, 250);
    } else {
        nid.dwInfoFlags = NIIF_WARNING;
        lstrcpyW(nid.szInfoTitle, L"导出失败");
        lstrcpyW(nid.szInfo, L"程序所在目录可能不可写。");
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void show_menu(void)
{
    HMENU m = CreatePopupMenu();
    WCHAR t[200];
    const WCHAR *nm = (prov_name() && prov_name()[0]) ? prov_name() : L"未检测到设备";

    if (g_level < 0)
        lstrcpyW(t, nm);
    else if (g_charging == 1)
        wsprintfW(t, L"%s  电量 %d%%（充电中）", nm, g_level);
    else
        wsprintfW(t, L"%s  电量 %d%%%s", nm, g_level, g_linked ? L"" : L"（缓存）");
    AppendMenuW(m, MF_STRING | MF_GRAYED, MENU_STATE, t);

    if (prov_detail()[0])
        AppendMenuW(m, MF_STRING | MF_GRAYED, MENU_PROTO, prov_detail());

    BYTE raw[16];
    int rawLen = prov_last_raw(raw, sizeof(raw));
    if (rawLen > 0 && rawLen <= (int)sizeof(raw)) {
        WCHAR r[64], *p = r;
        p += wsprintfW(p, L"报文 ");
        for (int i = 0; i < rawLen && i < 8; i++)
            p += wsprintfW(p, L"%02X ", raw[i]);
        AppendMenuW(m, MF_STRING | MF_GRAYED, MENU_STATE + 100, r);
    }

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_showFloat ? MF_CHECKED : 0), MENU_FLOAT, L"桌面浮窗");
    AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), MENU_AUTORUN,
                L"开机自动启动");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, MENU_RESCAN, L"重新扫描设备");
    AppendMenuW(m, MF_STRING, MENU_DIAG, L"导出诊断信息…");
    AppendMenuW(m, MF_STRING, MENU_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessage(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ---------------- 读数处理 ---------------- */

static void apply_reading(const READING *r)
{
    if (!r->valid || r->level < 0)
        return;
    BOOL wasLinked = g_linked;
    g_linked = TRUE;
    g_lastTick = GetTickCount64();
    g_cachedMin = 0;
    reg_set(L"Level", (DWORD)r->level);
    reg_set(L"Stamp", stamp_now());
    set_level(r->level, r->charging);
    if (!wasLinked)
        tray_update();
}

static void after_scan(void)
{
    g_lastScan = GetTickCount64();
    g_nextPoll = g_lastScan + 3000;
    tray_update();
    if (prov_passive()) {
        prov_arm();
    } else {
        READING r;
        if (prov_poll(&r))
            apply_reading(&r);
    }
    if (g_float && IsWindowVisible(g_float))
        float_render();
}

static void do_rescan(void)
{
    g_linked = FALSE;
    prov_close();
    prov_scan();
    after_scan();
}

static void tick(void)
{
    ULONGLONG now = GetTickCount64();

    const WCHAR *nm = prov_name();
    if (!nm || !nm[0]) {
        if (now - g_lastScan > 4000) {
            prov_scan();
            after_scan();
        }
        return;
    }

    if (prov_passive()) {
        if (g_linked && now - g_lastTick > 12000) {
            g_linked = FALSE;
            tray_update();
        }
        return;
    }

    if (now >= g_nextPoll) {
        g_nextPoll = now + 20000;
        READING r;
        if (prov_poll(&r))
            apply_reading(&r);
        else if (g_linked && now - g_lastTick > 90000) {
            g_linked = FALSE;
            tray_update();
        }
    }
}

/* ---------------- 窗口过程 ---------------- */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (hwnd == g_float) {
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST: {
            /* 圆形以外的区域让点击穿透 */
            POINT p;
            p.x = (short)LOWORD(lp);
            p.y = (short)HIWORD(lp);
            ScreenToClient(hwnd, &p);
            int r = g_dia / 2;
            int dx = p.x - r, dy = p.y - r;
            if (dx * dx + dy * dy > r * r)
                return HTTRANSPARENT;
            return HTCLIENT;
        }
        case WM_LBUTTONDOWN:
            g_dragging = TRUE;
            SetCapture(hwnd);
            {
                POINT p;
                GetCursorPos(&p);
                g_dragOfs = p;
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
            READING r;
            r.valid = TRUE;
            r.level = 93;
            r.charging = -1;
            apply_reading(&r);
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
        case MENU_RESCAN:
            do_rescan();
            break;
        case MENU_DIAG:
            export_diag();
            break;
        case MENU_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL)
            PostMessage(hwnd, WM_RESCAN, 0, 0);
        else if (wp == DBT_DEVICEREMOVECOMPLETE) {
            prov_close();
            g_linked = FALSE;
            tray_update();
            PostMessage(hwnd, WM_RESCAN, 0, 0);
        }
        return 0;

    case WM_RESCAN:
        do_rescan();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_ENDSESSION:
        return 0;

    default: {
        static UINT s_taskbarCreated;
        if (!s_taskbarCreated)
            s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        if (msg == s_taskbarCreated) {
            tray_show(FALSE);
            return 0;
        }
    } break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------- 入口 ---------------- */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;

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
    g_dia = scale(FLOAT_DIA);

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

    g_showFloat = reg_get(L"Float", 1) ? TRUE : FALSE;

    {
        DWORD lv = reg_get(L"Level", 0xFFFFFFFF);
        if (lv != 0xFFFFFFFF && lv <= 100) {
            DWORD now = stamp_now();
            DWORD saved = reg_get(L"Stamp", now);
            g_level = (int)lv;
            g_cachedMin = (now > saved) ? (int)(now - saved) : 0;
        }
    }

    g_float = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kClass, L"RapooBatteryFloat", WS_POPUP,
                              0, 0, g_dia, g_dia, NULL, NULL, inst, NULL);

    g_font = CreateFontW(-(int)(scale(23) * SS), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!g_font)
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_icon = build_icon(g_level, TRUE);
    tray_show(FALSE);

    float_restore_pos();
    float_apply_visible();

    prov_scan();
    after_scan();

#ifdef RB_SIM
    SetTimer(g_hwnd, 9, 1500, NULL);
#endif

    for (;;) {
        HANDLE ev = prov_passive() ? prov_event() : NULL;
        DWORD  cnt = ev ? 1 : 0;
        DWORD  r = MsgWaitForMultipleObjects(cnt, cnt ? &ev : NULL, FALSE, 1000, QS_ALLINPUT);

        if (cnt && r == WAIT_OBJECT_0) {
            READING rd;
            if (prov_read_ready(&rd))
                apply_reading(&rd);
            prov_arm();
            continue;
        }
        if (r == WAIT_TIMEOUT) {
            tick();
            continue;
        }
        if (r == WAIT_OBJECT_0 + cnt) {
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
            prov_close();
            Sleep(500);
        }
    }

done:
    prov_close();
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
    if (g_font)
        DeleteObject(g_font);
    if (g_mutex)
        CloseHandle(g_mutex);
    return 0;
}
