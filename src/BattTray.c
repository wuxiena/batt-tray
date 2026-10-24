// BattTray - 无线设备电量托盘工具（多品牌 · 多设备）
// 依赖：纯 Win32 API，无运行时框架
// 结构：providers.c 负责各协议族；本文件负责界面与消息循环
// 界面：每台设备一个托盘图标（悬停显示品牌）；圆形浮窗显示当前选中设备
//
// 多设备说明：
//   托盘图标 uID = TRAY_UID_BASE + 设备序号，回调 wParam 即 uID。
//   被动型设备（雷柏、索尼手柄）各持有一个事件句柄，消息循环同时等待它们。

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

#define MENU_DEV_BASE    100
#define MENU_PROTO       20
#define MENU_RAW         21
#define MENU_FLOAT       2
#define MENU_AUTORUN     3
#define MENU_RESCAN      4
#define MENU_EXIT        5
#define MENU_DIAG        7

#define TRAY_UID_BASE    100        /* 托盘图标 uID 起点 */

/* 托盘图标：深色圆角徽章 + 彩色电量数字。
   托盘实际只显示 16px，画电池外形再塞数字必然糊成一团，
   所以这里直接让「数字」当主角，用 3x5 点阵放大绘制，笔画干净。 */
#define ICON_MAX         32     /* 支持的最大图标边长（高 DPI） */
#define ICON_SS          4      /* 图标圆角覆盖率子采样倍数 */
#define DIG_W            3      /* 点阵字宽（像素） */
#define DIG_H            5      /* 点阵字高（像素） */

#define FLOAT_DIA        62     /* 圆形浮窗逻辑直径 */
#define SS               4      /* 超采样倍数（抗锯齿） */

static const WCHAR kClass[] = L"BattTrayWnd";
static const WCHAR kRegKey[] = L"Software\\BattTray";
static const WCHAR kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR kRunVal[] = L"BattTray";
static const WCHAR kMutex[] = L"Local\\BattTray_SingleInstance";

/* 每台设备的运行状态 */
typedef struct {
    int       level;
    int       charging;
    BOOL      linked;      /* 本次开机后读到过实时值 */
    BOOL      passive;
    ULONGLONG lastTick;
    int       cachedMin;   /* 启动时从注册表恢复的读数「多少分钟前」 */
    HICON     icon;
    BOOL      lowNotified;
} SLOT;

static HINSTANCE  g_inst;
static HWND       g_hwnd;
static HWND       g_float;
static HANDLE     g_mutex;
static SLOT       g_slot[PROV_MAX];
static int        g_n;                 /* 当前设备数 */
static int        g_trayOn[PROV_MAX];  /* 该设备当前是否占了托盘图标 */
static int        g_sel;               /* 浮窗显示的设备序号 */
static ULONGLONG  g_lastScan;
static ULONGLONG  g_nextPoll;
static HFONT      g_font;
static BOOL       g_showFloat;
static BOOL       g_dragging;
static BOOL       g_dragMoved;
static POINT      g_dragOfs;
static int        g_dpi = 96;
static int        g_dia = FLOAT_DIA;

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

/* 设备读数缓存的键按槽位编号，同时存一份设备名用于校验，
   避免设备顺序变化时把 A 的电量套到 B 上。 */
static void reg_get_str(const WCHAR *name, WCHAR *out, int cap)
{
    HKEY k;
    out[0] = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD sz = (DWORD)(cap * sizeof(WCHAR)), type = 0;
        if (RegQueryValueExW(k, name, NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS)
            out[0] = 0;
        RegCloseKey(k);
    }
}

static void reg_set_str(const WCHAR *name, const WCHAR *val)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)
        == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val,
                       (DWORD)((lstrlenW(val) + 1) * sizeof(WCHAR)));
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
        return 0xFF9AA3AD;   /* 未知：中性灰蓝 */
    if (level <= 20)
        return 0xFFFF5A5A;   /* 低电量：亮红，暗色任务栏上也跳得出来 */
    if (level <= 50)
        return 0xFFFFB020;   /* 中等：琥珀 */
    return 0xFF3FD964;       /* 充足：绿 */
}

/* 线性混色，t 为 b 的权重（0..256） */
static DWORD rgb_mix(DWORD a, DWORD b, int t)
{
    int r = (((int)(a >> 16) & 0xFF) * (256 - t) + ((int)(b >> 16) & 0xFF) * t) >> 8;
    int g = (((int)(a >> 8) & 0xFF) * (256 - t) + ((int)(b >> 8) & 0xFF) * t) >> 8;
    int bl = ((int)(a & 0xFF) * (256 - t) + (int)(b & 0xFF) * t) >> 8;
    return 0xFF000000u | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)bl;
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

/* 圆角矩形覆盖率：每像素 4x4 子采样，返回 0..16。
   直接算覆盖率而不是逐像素硬切，圆角才有抗锯齿的过渡。 */
static int rr_cov(int s, int rad, int x, int y)
{
    double half = s * 0.5;
    double lim  = half - rad;
    int n = 0;

    for (int sy = 0; sy < ICON_SS; sy++) {
        double py = y + (sy + 0.5) / ICON_SS;
        double dy = fabs(py - half) - lim;
        if (dy < 0)
            dy = 0;
        for (int sx = 0; sx < ICON_SS; sx++) {
            double px = x + (sx + 0.5) / ICON_SS;
            double dx = fabs(px - half) - lim;
            if (dx < 0)
                dx = 0;
            if (dx * dx + dy * dy <= (double)rad * rad)
                n++;
        }
    }
    return n;
}

static void draw_digits(DWORD *px, int s, const WCHAR *t, int n, int k, DWORD color)
{
    int gw  = DIG_W * k;
    int gap = k;                         /* 字距跟着缩放走，整体宽度才是偶数格，能居中 */
    int w   = n * gw + (n - 1) * gap;
    int h   = DIG_H * k;
    int x0  = (s - w) / 2;
    int y0  = (s - h) / 2;

    for (int i = 0; i < n; i++) {
        int gi = 10;
        if (t[i] >= L'0' && t[i] <= L'9')
            gi = (int)(t[i] - L'0');
        const BYTE *g = kGlyph[gi];
        int bx = x0 + i * (gw + gap);

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
    if (dim) {
        /* 缓存读数：只压掉一部分鲜艳度，仍看得出电量档位 */
        fg = rgb_mix(fg, 0xFF8A8A8A, 96);
    }

    /* 深色徽章底：亮色数字在浅色/深色任务栏上都能读。
       上下做一道很轻的渐变（上浅下深），16px 下才不像一块死黑方块。 */
    DWORD top = dim ? 0xFF454545u : 0xFF3F4855u;
    DWORD bot = dim ? 0xFF232323u : 0xFF14171Bu;
    DWORD hi  = dim ? 0xFF555555u : 0xFF4C5766u;   /* 顶部高光 */

    int rad   = (S >= 28) ? 6 : (S >= 20 ? 5 : 4);
    int span  = (S > 1) ? S - 1 : 1;
    int denom = ICON_SS * ICON_SS;

    for (int y = 0; y < S; y++) {
        DWORD base = rgb_mix(top, bot, y * 256 / span);
        /* 最上面一行再提一点，给平铺的方块一点体积感 */
        if (y == 0)
            base = rgb_mix(base, hi, 56);
        else if (y == 1)
            base = rgb_mix(base, hi, 18);

        for (int x = 0; x < S; x++) {
            int cov = rr_cov(S, rad, x, y);
            if (cov <= 0)
                continue;
            DWORD a = (DWORD)(cov * 255 / denom);
            px[y * S + x] = (a << 24) | (base & 0x00FFFFFFu);
        }
    }

    /* 文本：两位数字为主，满电三位，未知显示一根横杠 */
    WCHAR t[4];
    int n;
    if (level < 0) {
        t[0] = L'-'; n = 1;
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
        int w = n * DIG_W * kk + (n - 1) * kk;
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

/* ---------------- 设备信息与托盘 ---------------- */

static BOOL dev_info(int i, DEVDESC *d)
{
    if (i < 0 || i >= g_n)
        return FALSE;
    return prov_desc(i, d);
}

/* 详情里带「无应答」＝设备在，但厂商通道不回答，别一直骗用户「等待数据」 */
static BOOL text_has(const WCHAR *hay, const WCHAR *needle)
{
    if (!hay || !needle || !needle[0])
        return FALSE;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j])
            j++;
        if (!needle[j])
            return TRUE;
    }
    return FALSE;
}

/* 悬停提示：品牌在最前面，一眼能分辨是哪台设备 */
static void slot_tip(int i, WCHAR *out, int cap)
{
    DEVDESC d;
    if (!dev_info(i, &d)) {
        lstrcpynW(out, L"未检测到设备", cap);
        return;
    }
    const SLOT *s = &g_slot[i];

    if (s->level < 0) {
        if (text_has(d.detail, L"无应答"))
            wsprintfW(out, L"%s %s · dongle 不提供电量", d.brand, d.type);
        else
            wsprintfW(out, L"%s %s · 等待数据…", d.brand, d.type);
        return;
    }
    if (s->linked) {
        wsprintfW(out, L"%s %s · %d%%%s", d.brand, d.type, s->level,
                  s->charging == 1 ? L" 充电中" : L"");
        return;
    }
    if (s->cachedMin >= 120)
        wsprintfW(out, L"%s %s · %d%%（%d 小时前读数）", d.brand, d.type, s->level,
                  s->cachedMin / 60);
    else if (s->cachedMin > 0)
        wsprintfW(out, L"%s %s · %d%%（%d 分钟前读数）", d.brand, d.type, s->level,
                  s->cachedMin);
    else
        wsprintfW(out, L"%s %s · %d%%（缓存）", d.brand, d.type, s->level);
}

static void tray_show(int i, BOOL add)
{
    if (i < 0 || i >= PROV_MAX)
        return;

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = (UINT)(TRAY_UID_BASE + i);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_slot[i].icon;
    slot_tip(i, nid.szTip, 127);

    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &nid);
}

static void tray_remove(int i)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = (UINT)(TRAY_UID_BASE + i);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void float_render(void);

/* 托盘只给「拿得到电量」的设备留位置：读不到电量的设备一直挂个 `-`
   图标毫无意义，它只在右键列表里列出来说明情况。
   唯一的例外：一台设备都读不到时留一个图标当入口，否则连菜单都打不开。
   注意：托盘 uID 仍按设备序号算，所以某台设备中途变得可读时会自然补上图标。 */
static int readable_count(void)
{
    int n = 0;
    for (int i = 0; i < g_n; i++)
        if (g_slot[i].level >= 0)
            n++;
    return n;
}

static BOOL slot_on_tray(int i)
{
    if (i < 0 || i >= g_n)
        return FALSE;
    if (g_slot[i].level >= 0)
        return TRUE;
    return (i == 0 && readable_count() == 0);
}

/* 占托盘的设备列表；返回个数，out 里是设备序号 */
static int tray_list(int *out)
{
    int n = 0;
    for (int i = 0; i < g_n; i++) {
        if (slot_on_tray(i)) {
            if (out)
                out[n] = i;
            n++;
        }
    }
    return n;
}

/* 浮窗显示目标必须落在能显示电量的设备上 */
static void ensure_sel(void)
{
    if (slot_on_tray(g_sel))
        return;
    int rl[PROV_MAX];
    int rn = tray_list(rl);
    g_sel = rn ? rl[0] : 0;
}

static void slot_update(int i)
{
    if (i < 0 || i >= g_n)
        return;

    HICON old = g_slot[i].icon;
    HICON ic = build_icon(g_slot[i].level, !g_slot[i].linked);
    if (!ic)
        return;
    g_slot[i].icon = ic;
    if (g_trayOn[i])
        tray_show(i, FALSE);
    if (old)
        DestroyIcon(old);
    if (i == g_sel && g_float && IsWindowVisible(g_float))
        float_render();
}

static void tray_sync(void)
{
    for (int i = 0; i < g_n; i++) {
        BOOL want = slot_on_tray(i);
        if (want && !g_slot[i].icon)
            g_slot[i].icon = build_icon(g_slot[i].level, !g_slot[i].linked);
        if (want) {
            tray_show(i, !g_trayOn[i]);
            g_trayOn[i] = TRUE;
        } else if (g_trayOn[i]) {
            tray_remove(i);
            g_trayOn[i] = FALSE;
        }
    }
    for (int i = g_n; i < PROV_MAX; i++) {
        if (g_trayOn[i]) {
            tray_remove(i);
            g_trayOn[i] = FALSE;
        }
    }
}

/* Windows 11 会把每个新图标塞进「隐藏的图标」区，而这是按 (exe 路径, uID)
   记在 NotifyIconSettings 里的。多设备时每台设备都是一个新 uID，
   默认全都藏着，等于白做。这里给「从未设置过」的条目补一个 IsPromoted=1，
   用户之后手动拖回去/拖出来（值变成 0）就不再多管。 */
static int promote_tray_icons(void)
{
    WCHAR me[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, me, MAX_PATH);
    me[n] = 0;

    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0,
                      KEY_READ | KEY_WRITE, &root) != ERROR_SUCCESS)
        return 0;

    int changed = 0;
    WCHAR name[128];
    for (DWORD i = 0;; i++) {
        DWORD nl = 128;
        if (RegEnumKeyExW(root, i, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        HKEY sub;
        if (RegOpenKeyExW(root, name, 0, KEY_READ | KEY_WRITE, &sub) != ERROR_SUCCESS)
            continue;

        WCHAR path[MAX_PATH];
        DWORD sz = sizeof(path), type = 0;
        if (RegQueryValueExW(sub, L"ExecutablePath", NULL, &type, (LPBYTE)path, &sz)
            == ERROR_SUCCESS && lstrcmpiW(path, me) == 0) {
            DWORD v = 0, vs = sizeof(v);
            if (RegQueryValueExW(sub, L"IsPromoted", NULL, &type, (LPBYTE)&v, &vs) != ERROR_SUCCESS) {
                DWORD one = 1;
                if (RegSetValueExW(sub, L"IsPromoted", 0, REG_DWORD,
                                   (const BYTE *)&one, sizeof(one)) == ERROR_SUCCESS)
                    changed++;
            }
        }
        RegCloseKey(sub);
    }
    RegCloseKey(root);
    return changed;
}

/* 同步图标并处理「新图标默认被折叠」的问题：设置只在注册时生效，
   所以补完 IsPromoted 后要重新注册一遍。 */
static void tray_sync_promote(void)
{
    tray_sync();
    if (promote_tray_icons() > 0) {
        for (int i = 0; i < PROV_MAX; i++) {
            if (g_trayOn[i]) {
                tray_remove(i);
                g_trayOn[i] = FALSE;
            }
        }
        tray_sync();
    }
}

static void notify_low(int i)
{
    DEVDESC d;
    if (!dev_info(i, &d))
        return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = (UINT)(TRAY_UID_BASE + i);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    wsprintfW(nid.szInfoTitle, L"%s %s 电量低", d.brand, d.type);
    wsprintfW(nid.szInfo, L"剩余 %d%%，建议充电。", g_slot[i].level);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void notify_tip(const WCHAR *title, const WCHAR *body, DWORD flags)
{
    int i = (g_sel >= 0 && g_sel < g_n) ? g_sel : 0;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = (UINT)(TRAY_UID_BASE + i);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = flags;
    lstrcpynW(nid.szInfoTitle, title, 63);
    lstrcpynW(nid.szInfo, body, 255);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void set_level(int i, int level, int charging)
{
    SLOT *s = &g_slot[i];
    BOOL wasOn = slot_on_tray(i);

    if (level >= 0 && level <= 20 && !s->lowNotified && s->linked) {
        notify_low(i);
        s->lowNotified = TRUE;
    }
    if (level > 25)
        s->lowNotified = FALSE;

    if (level != s->level || charging != s->charging) {
        s->level = level;
        s->charging = charging;
        slot_update(i);
    }
    /* 从「读不到」变成「读到了」（或反过来）：补上 / 撤掉托盘图标 */
    if (wasOn != slot_on_tray(i))
        tray_sync();
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

    int level = -1, charging = -1;
    BOOL linked = FALSE;
    ensure_sel();
    if (g_sel >= 0 && g_sel < g_n) {
        level = g_slot[g_sel].level;
        charging = g_slot[g_sel].charging;
        linked = g_slot[g_sel].linked;
    }

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

    DWORD acc = level_color(level);
    if (!linked)
        acc = shade(acc, 62);
    COLORREF accent = RGB((BYTE)(acc & 0xFF), (BYTE)((acc >> 8) & 0xFF), (BYTE)((acc >> 16) & 0xFF));

    int thickness = scale(5) * SS;
    int dotRad = thickness / 2;
    int ro = c - scale(2) * SS - dotRad;
    int sweep = (level < 0) ? 0 : clamp100(level) * 360 / 100;

    /* 轨道 */
    HBRUSH track = CreateSolidBrush(linked ? RGB(0x2E, 0x31, 0x38) : RGB(0x26, 0x28, 0x2D));
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

    /* 中心数字：多设备时上移一点，给底部的设备指示点腾位置 */
    WCHAR txt[16];
    if (level < 0)
        lstrcpyW(txt, L"--");
    else
        wsprintfW(txt, L"%d", level);

    /* 指示点只按「能显示电量」的设备画：读不到的那台没有托盘图标，
       在浮窗里也没有轮换的意义，只在右键列表里说明。 */
    int rl[PROV_MAX];
    int rn = tray_list(rl);

    int shift = (rn > 1) ? scale(7) * SS : 0;   /* 多设备时数字上移，留出指示点 */
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, accent);
    HGDIOBJ oldFont = SelectObject(mem, g_font);
    RECT tr = { 0, -shift, big, big - shift };
    DrawTextW(mem, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, oldFont);

    /* 多设备时在数字下方画一排指示点：亮的是当前显示的那台 */
    if (rn > 1) {
        int step = scale(7) * SS;
        int rr   = scale(2) * SS;
        int y    = c + shift + scale(6) * SS;
        int x0   = c - (rn - 1) * step / 2;
        for (int k = 0; k < rn; k++) {
            HBRUSH b = CreateSolidBrush(rl[k] == g_sel ? accent : RGB(0x4A, 0x50, 0x5A));
            SelectObject(mem, b);
            SelectObject(mem, GetStockObject(NULL_PEN));
            int cx = x0 + k * step;
            Ellipse(mem, cx - rr, y - rr, cx + rr + 1, y + rr + 1);
            DeleteObject(b);
        }
    }

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
    lstrcatW(path, L"BattTray-diag.txt");

    if (prov_diag(path))
        notify_tip(L"诊断信息已导出", path, NIIF_INFO);
    else
        notify_tip(L"导出失败", L"程序所在目录可能不可写。", NIIF_WARNING);
}

static void show_menu(int i)
{
    if (i < 0 || i >= g_n)
        i = (g_sel >= 0 && g_sel < g_n) ? g_sel : 0;

    HMENU m = CreatePopupMenu();
    WCHAR t[200];

    if (g_n == 0) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, MENU_DEV_BASE, L"未检测到设备");
    } else {
        /* 设备列表：每台一行，勾选的是浮窗正在显示的那台。
           读不到电量的设备也列在这里（不进托盘），并标注原因。 */
        for (int k = 0; k < g_n; k++) {
            DEVDESC d;
            if (!dev_info(k, &d))
                continue;
            const SLOT *s = &g_slot[k];
            UINT flags = MF_STRING;
            if (s->level < 0) {
                flags |= MF_GRAYED;          /* 没电量可选，只做说明 */
                const WCHAR *why = L"电量不可读";
                if (text_has(d.detail, L"无应答"))
                    why = L"电量不可读（dongle 不提供）";
                else if (text_has(d.detail, L"未上报"))
                    why = L"电量不可读（手柄不提供）";
                else if (text_has(d.detail, L"有线"))
                    why = L"电量不可读（有线连接）";
                wsprintfW(t, L"%s %s · %s", d.brand, d.type, why);
            } else {
                if (k == g_sel)
                    flags |= MF_CHECKED;
                if (s->charging == 1)
                    wsprintfW(t, L"%s %s · %d%%（充电中）", d.brand, d.type, s->level);
                else
                    wsprintfW(t, L"%s %s · %d%%%s", d.brand, d.type, s->level,
                              s->linked ? L"" : L"（缓存）");
            }
            AppendMenuW(m, flags, MENU_DEV_BASE + k, t);
        }
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

static void slot_reg_name(int i, const WCHAR *prefix, WCHAR *out)
{
    wsprintfW(out, L"%s%d", prefix, i);
}

static void apply_reading(int i, const READING *r)
{
    if (i < 0 || i >= g_n || !r->valid || r->level < 0)
        return;

    SLOT *s = &g_slot[i];
    BOOL wasLinked = s->linked;
    s->linked = TRUE;
    s->lastTick = GetTickCount64();
    s->cachedMin = 0;

    WCHAR key[16];
    slot_reg_name(i, L"L", key);
    if ((int)reg_get(key, 0xFFFFFFFF) != r->level) {
        reg_set(key, (DWORD)r->level);
        slot_reg_name(i, L"S", key);
        reg_set(key, stamp_now());
    }

    if (!wasLinked)
        slot_update(i);          /* 缓存态 → 实时态，颜色要提亮 */
    set_level(i, r->level, r->charging);
}

/* 扫描后重建槽位；同一品牌的旧读数尽量保留，避免刷新时数字闪一下 */
static void rebuild_slots(int prevN, SLOT *prev, WCHAR prevKey[][96])
{
    BOOL used[PROV_MAX];
    ZeroMemory(used, sizeof(used));

    for (int i = 0; i < g_n; i++) {
        DEVDESC d;
        WCHAR key[96] = L"";
        if (dev_info(i, &d))
            wsprintfW(key, L"%s|%s", d.brand, d.type);

        BOOL reused = FALSE;
        for (int j = 0; j < prevN; j++) {
            if (used[j] || !prevKey[j][0])
                continue;
            if (lstrcmpW(key, prevKey[j]) == 0) {
                g_slot[i] = prev[j];
                used[j] = TRUE;
                reused = TRUE;
                break;
            }
        }
        if (!reused) {
            ZeroMemory(&g_slot[i], sizeof(SLOT));
            g_slot[i].level = -1;
            g_slot[i].charging = -1;
        }
    }

    for (int j = 0; j < prevN; j++) {
        if (!used[j] && prev[j].icon)
            DestroyIcon(prev[j].icon);
    }
    for (int i = g_n; i < PROV_MAX; i++)
        ZeroMemory(&g_slot[i], sizeof(SLOT));
}

static void do_scan(void)
{
    SLOT prev[PROV_MAX];
    WCHAR prevKey[PROV_MAX][96];
    int prevN = g_n;

    CopyMemory(prev, g_slot, sizeof(prev));
    for (int i = 0; i < prevN; i++) {
        DEVDESC d;
        prevKey[i][0] = 0;
        if (dev_info(i, &d))
            wsprintfW(prevKey[i], L"%s|%s", d.brand, d.type);
    }

    prov_close();
    g_n = prov_scan();

    rebuild_slots(prevN, prev, prevKey);

    if (g_n > 0 && g_sel >= g_n)
        g_sel = 0;
    if (g_n == 0)
        g_sel = 0;

    tray_sync_promote();

    /* 首次读数：被动型挂起监听，其余立即查一次 */
    for (int i = 0; i < g_n; i++) {
        if (prov_is_passive(i)) {
            prov_arm(i);
        } else {
            READING r;
            ZeroMemory(&r, sizeof(r));
            if (prov_poll(i, &r))
                apply_reading(i, &r);
        }
    }

    g_lastScan = GetTickCount64();
    g_nextPoll = g_lastScan + 3000;

    if (g_float && IsWindowVisible(g_float))
        float_render();
}

static void poll_all(void)
{
    for (int i = 0; i < g_n; i++) {
        if (prov_is_passive(i))
            continue;
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (prov_poll(i, &r))
            apply_reading(i, &r);
        else if (g_slot[i].linked && GetTickCount64() - g_slot[i].lastTick > 90000) {
            g_slot[i].linked = FALSE;
            slot_update(i);
        }
    }
}

static void tick(void)
{
    ULONGLONG now = GetTickCount64();

    if (g_n == 0) {
        if (now - g_lastScan > 4000)
            do_scan();
        return;
    }

    /* 被动型：太久没上报就转成「缓存」显示 */
    for (int i = 0; i < g_n; i++) {
        if (!prov_is_passive(i) || !g_slot[i].linked)
            continue;
        if (now - g_slot[i].lastTick > 12000) {
            g_slot[i].linked = FALSE;
            slot_update(i);
        }
    }

    if (now >= g_nextPoll) {
        g_nextPoll = now + 20000;
        poll_all();
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
            g_dragMoved = FALSE;
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
                if (p.x != g_dragOfs.x || p.y != g_dragOfs.y)
                    g_dragMoved = TRUE;
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
                if (g_dragMoved) {
                    float_save_pos();
                } else if (g_n > 1) {
                    /* 没拖动 = 点一下：在多台设备之间轮换 */
                    g_sel = (g_sel + 1) % g_n;
                    float_render();
                }
            }
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            show_menu(-1);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
#ifdef BATT_SIM
    case WM_TIMER:
        if (wp == 9 && g_n > 0) {
            READING r;
            r.valid = TRUE;
            r.level = 93;
            r.charging = -1;
            apply_reading(0, &r);
        }
        return 0;
#endif
    case WM_TRAY: {
        int i = (int)wp - TRAY_UID_BASE;
        if (i < 0 || i >= g_n)
            return 0;
        switch (LOWORD((DWORD)lp)) {
        case WM_LBUTTONUP:
            g_sel = i;
            g_showFloat = !g_showFloat;
            reg_set(L"Float", g_showFloat ? 1 : 0);
            float_apply_visible();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            g_sel = i;
            show_menu(i);
            break;
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD((DWORD)wp);
        if (id >= MENU_DEV_BASE && id < MENU_DEV_BASE + PROV_MAX) {
            int k = id - MENU_DEV_BASE;
            if (k < g_n) {
                g_sel = k;
                if (!g_showFloat) {
                    g_showFloat = TRUE;
                    reg_set(L"Float", 1);
                }
                float_apply_visible();
            }
            return 0;
        }
        switch (id) {
        case MENU_FLOAT:
            g_showFloat = !g_showFloat;
            reg_set(L"Float", g_showFloat ? 1 : 0);
            float_apply_visible();
            break;
        case MENU_AUTORUN:
            autostart_apply(autostart_enabled() ? FALSE : TRUE);
            break;
        case MENU_RESCAN:
            do_scan();
            break;
        case MENU_DIAG:
            export_diag();
            break;
        case MENU_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    }

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL)
            PostMessage(hwnd, WM_RESCAN, 0, 0);
        else if (wp == DBT_DEVICEREMOVECOMPLETE) {
            prov_close();
            g_n = 0;
            tray_sync();
            PostMessage(hwnd, WM_RESCAN, 0, 0);
        }
        return 0;

    case WM_RESCAN:
        do_scan();
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
            /* explorer 重启过：旧图标句柄全失效，按当前状态重新注册 */
            ZeroMemory(g_trayOn, sizeof(g_trayOn));
            tray_sync_promote();
            return 0;
        }
    } break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------- 缓存恢复 ---------------- */

static void load_cache(void)
{
    DWORD now = stamp_now();
    for (int i = 0; i < g_n; i++) {
        DEVDESC d;
        if (!dev_info(i, &d))
            continue;

        WCHAR key[16], keyN[16], keyS[16];
        slot_reg_name(i, L"N", keyN);
        slot_reg_name(i, L"L", key);
        slot_reg_name(i, L"S", keyS);

        WCHAR name[96];
        reg_get_str(keyN, name, 96);

        DWORD lv = reg_get(key, 0xFFFFFFFF);
        if (lv == 0xFFFFFFFF || lv > 100)
            continue;
        /* 槽位顺序可能变过，名字不一致就当没有缓存 */
        if (name[0] && lstrcmpW(name, d.brand) != 0) {
            WCHAR name2[96];
            wsprintfW(name2, L"%s %s", d.brand, d.type);
            if (lstrcmpW(name2, name) != 0)
                continue;
        }

        DWORD saved = reg_get(keyS, now);
        g_slot[i].level = (int)lv;
        g_slot[i].charging = -1;
        g_slot[i].linked = FALSE;
        g_slot[i].cachedMin = (now > saved) ? (int)(now - saved) : 0;
    }
}

static void save_cache_names(void)
{
    for (int i = 0; i < g_n; i++) {
        DEVDESC d;
        if (!dev_info(i, &d))
            continue;
        WCHAR key[16];
        slot_reg_name(i, L"N", key);

        /* 槽位换过设备时，旧读数必须一起作废。
           否则下一个进程启动时名字已经对上，会把别人的电量当成自己的缓存显示。 */
        WCHAR old[96];
        reg_get_str(key, old, 96);
        if (old[0] && lstrcmpW(old, d.brand) != 0) {
            WCHAR k2[16];
            slot_reg_name(i, L"L", k2);
            reg_set(k2, 0xFFFFFFFFu);       /* load_cache 见到它就跳过 */
            slot_reg_name(i, L"S", k2);
            reg_set(k2, 0);
        }
        reg_set_str(key, d.brand);
    }
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

    g_hwnd = CreateWindowExW(0, kClass, L"BattTray", WS_OVERLAPPED,
                             0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_hwnd)
        return 1;

    g_showFloat = reg_get(L"Float", 1) ? TRUE : FALSE;
    for (int i = 0; i < PROV_MAX; i++) {
        g_slot[i].level = -1;
        g_slot[i].charging = -1;
    }

    g_float = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kClass, L"BattTrayFloat", WS_POPUP,
                              0, 0, g_dia, g_dia, NULL, NULL, inst, NULL);

    g_font = CreateFontW(-(int)(scale(23) * SS), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!g_font)
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    /* 先扫描：托盘图标的数量取决于设备数 */
    g_n = prov_scan();
    load_cache();
    save_cache_names();

    for (int i = 0; i < g_n; i++) {
        g_slot[i].passive = prov_is_passive(i);
        g_slot[i].icon = build_icon(g_slot[i].level, !g_slot[i].linked);
    }
    tray_sync_promote();

    float_restore_pos();
    float_apply_visible();

    for (int i = 0; i < g_n; i++) {
        if (prov_is_passive(i)) {
            prov_arm(i);
        } else {
            READING r;
            ZeroMemory(&r, sizeof(r));
            if (prov_poll(i, &r))
                apply_reading(i, &r);
        }
    }
    g_lastScan = GetTickCount64();
    g_nextPoll = g_lastScan + 3000;

#ifdef BATT_SIM
    SetTimer(g_hwnd, 9, 1500, NULL);
#endif

    for (;;) {
        HANDLE ev[PROV_MAX];
        int    map[PROV_MAX];
        int    cnt = 0;

        for (int i = 0; i < g_n; i++) {
            if (!prov_is_passive(i))
                continue;
            HANDLE e = prov_event(i);
            if (!e)
                continue;
            ev[cnt] = e;
            map[cnt] = i;
            cnt++;
        }

        DWORD r = MsgWaitForMultipleObjects((DWORD)cnt, cnt ? ev : NULL, FALSE, 1000,
                                           QS_ALLINPUT);

        if (cnt && r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + (DWORD)cnt) {
            int i = map[r - WAIT_OBJECT_0];
            READING rd;
            if (prov_read_ready(i, &rd))
                apply_reading(i, &rd);
            prov_arm(i);
            continue;
        }
        if (r == WAIT_TIMEOUT) {
            tick();
            continue;
        }
        if (cnt && r == WAIT_OBJECT_0 + (DWORD)cnt) {
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
            Sleep(200);
        }
    }

done:
    prov_close();
    for (int i = 0; i < PROV_MAX; i++) {
        if (g_trayOn[i]) {
            tray_remove(i);
            g_trayOn[i] = FALSE;
        }
    }
    for (int i = 0; i < PROV_MAX; i++) {
        if (g_slot[i].icon)
            DestroyIcon(g_slot[i].icon);
    }
    if (g_font)
        DeleteObject(g_font);
    if (g_mutex)
        CloseHandle(g_mutex);
    return 0;
}
