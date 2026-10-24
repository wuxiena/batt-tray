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
#define MENU_IV_BASE     200     /* 刷新间隔：200 .. 200+N-1 */
#define MENU_STATS      10       /* 使用统计子菜单 */
#define MENU_HIST_CLEAR 11       /* 清除历史连接记录 */
#define MENU_RESET      12       /* 重置所有信息 */
#define MENU_UNINSTALL  13       /* 卸载并清除所有数据 */
#define MENU_HIST_BASE  300      /* 统计条目（灰色，仅展示）：300 .. 300+HIST_MAX-1 */

/* 离线判定。无线设备关机后接收器还在枚举，所以只能靠「还有没有回应」判断：
   - 被动上报型（雷柏、索尼）：不再推帧就是走了，3 分钟足够排除偶发丢包；
   - 主动查询型：查询失败后进入 20 秒一次的复查，连失 3 次才认定离线，
     避免一次握手失败就把设备从列表里抹掉。 */
#define GONE_PASSIVE_MS  180000ULL
#define GONE_PROBE_MS     20000ULL
#define GONE_PROBE_MAX        3
#define REVIVE_MS         60000ULL   /* 已离线设备的复活探测节奏 */

/* 累计使用时长每 5 分钟落一次盘，免得强杀进程时整段会话都丢掉 */
#define FLUSH_MS         300000ULL

#define HIST_MAX 12      /* 历史设备条数上限 */

/* 主动查询的间隔。每次查询都要把无线设备的射频叫醒一次，间隔越短越费电，
   所以默认 5 分钟——鼠标/键盘的续航比「秒级刷新」重要得多。
   被动上报的机型（雷柏、索尼手柄）不受影响：它们本来就在自己推，我们不发包。 */
static const struct { int ms; const WCHAR *label; } kIntervals[] = {
    {    3000, L"每 3 秒（最费电）" },
    {   60000, L"每 1 分钟" },
    {  300000, L"每 5 分钟" },
    {  600000, L"每 10 分钟" },
    { 1800000, L"每 30 分钟" },
};
#define IV_COUNT ((int)(sizeof(kIntervals) / sizeof(kIntervals[0])))

static int g_pollMs = 300000;    /* 默认 5 分钟，用户可在右键菜单里改 */

#define TRAY_UID_BASE    100        /* 托盘图标 uID 起点 */
#define TRAY_UID_ORPHAN  (TRAY_UID_BASE + PROV_MAX)  /* 一台设备都没有时的入口图标 */

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
    int       fail;        /* 连续查询失败次数：避免一次偶发失败就把图标变暗 */
    BOOL      dead;        /* 判定已断开：不占托盘、不进列表、停止计时 */
    int       probe;       /* 疑似离线后的复查次数 */
    ULONGLONG nextProbe;   /* 下次复查 / 复活探测时刻 */
    ULONGLONG connSince;   /* 本次连接开始的 tick，0 = 当前不在线 */
} SLOT;

/* 历史连接记录：一台设备一条，按「品牌 + 类别」认人 */
typedef struct {
    WCHAR name[64];
    DWORD sec;          /* 累计连接秒数 */
    DWORD stamp;        /* 最后一次确认在线的分钟刻度 */
    BOOL  used;
} HIST;

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
static ULONGLONG  g_lastFlush;         /* 上次把使用时长落盘的时刻 */
static HIST       g_hist[HIST_MAX];
static BOOL       g_orphanOn;          /* 一台设备都没有时留下的入口图标 */
static HICON      g_orphanIcon;
static HFONT      g_font;
static BOOL       g_showFloat;
static BOOL       g_dragging;
static BOOL       g_dragMoved;
static POINT      g_dragOfs;
static int        g_dpi = 96;
static int        g_dia = FLOAT_DIA;

#define STAMP_UNIT 600000000ULL

static void reset_all(void);   /* 前置声明：定义在缓存恢复一节 */
static void uninstall(void);   /* 前置声明：定义在缓存恢复一节 */

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

/* 所有设备都断开时留一个入口图标。否则托盘里一个图标都不剩，
   用户连「退出」「重新扫描」都点不到，只能去任务管理器。 */
static void orphan_tip(WCHAR *out, int cap)
{
    for (int i = 0; i < g_n; i++) {
        if (!g_slot[i].dead)
            continue;
        DEVDESC d;
        if (dev_info(i, &d)) {
            wsprintfW(out, L"%s %s · 已断开", d.brand, d.type);
            return;
        }
    }
    lstrcpynW(out, L"BattTray · 没有已连接的设备", cap);
}

static void orphan_show(BOOL add)
{
    if (!g_orphanIcon)
        g_orphanIcon = build_icon(-1, TRUE);

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID_ORPHAN;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_orphanIcon;
    orphan_tip(nid.szTip, 127);
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &nid);
}

static void orphan_remove(void)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID_ORPHAN;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

/* 已判定离线的槽位不算「当前设备」：不占托盘、不进菜单、不参与轮换。
   槽位本身保留（序号要和协议层一一对应），设备回来时原地复活。 */
static BOOL slot_alive(int i)
{
    return (i >= 0 && i < g_n && !g_slot[i].dead);
}

static int visible_count(void)
{
    int n = 0;
    for (int i = 0; i < g_n; i++)
        if (!g_slot[i].dead)
            n++;
    return n;
}

/* 托盘只给「拿得到电量」的设备留位置：读不到电量的设备一直挂个 `-`
   图标毫无意义，它只在右键列表里列出来说明情况。
   唯一的例外：一台设备都读不到时留一个图标当入口，否则连菜单都打不开。
   注意：托盘 uID 仍按设备序号算，所以某台设备中途变得可读时会自然补上图标。 */
static int readable_count(void)
{
    int n = 0;
    for (int i = 0; i < g_n; i++)
        if (!g_slot[i].dead && g_slot[i].level >= 0)
            n++;
    return n;
}

static BOOL slot_on_tray(int i)
{
    if (!slot_alive(i))
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
    g_sel = rn ? rl[0] : -1;
}

static void slot_update(int i)
{
    if (!slot_alive(i))
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

    /* 一台设备都没占图标（全断开了）：补一个入口图标，别让程序「消失」 */
    if (tray_list(NULL) == 0) {
        if (!g_orphanOn) {
            orphan_show(TRUE);
            g_orphanOn = TRUE;
        } else {
            orphan_show(FALSE);
        }
    } else if (g_orphanOn) {
        orphan_remove();
        g_orphanOn = FALSE;
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
        if (g_orphanOn) {
            orphan_remove();
            g_orphanOn = FALSE;
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
    /* 没有可显示设备时，气泡挂到「入口图标」上，否则提示会直接丢失 */
    int i = slot_alive(g_sel) ? g_sel : -1;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = (i >= 0) ? (UINT)(TRAY_UID_BASE + i) : TRAY_UID_ORPHAN;
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

static void append_usage(const WCHAR *path);   /* 定义在使用时长那一段 */

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

    if (prov_diag(path)) {
        append_usage(path);
        notify_tip(L"诊断信息已导出", path, NIIF_INFO);
    } else {
        notify_tip(L"导出失败", L"程序所在目录可能不可写。", NIIF_WARNING);
    }
}

static void fill_stats_menu(HMENU s);   /* 定义在使用时长那一段 */

static void show_menu(int i)
{
    /* i 只是「从哪个图标点出来的」，菜单内容按当前状态整体重排，
       所以这里只需要保证拿到的序号是有效设备（或 -1）。 */
    if (!slot_alive(i))
        i = slot_alive(g_sel) ? g_sel : -1;

    HMENU m = CreatePopupMenu();
    WCHAR t[200];

    if (visible_count() == 0) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, MENU_DEV_BASE, L"未检测到已连接的设备");
    } else {
        /* 设备列表：每台一行，勾选的是浮窗正在显示的那台。
           读不到电量的设备也列在这里（不进托盘），并标注原因。
           已经断开的不列——它转到「使用统计」里作为历史记录出现。 */
        for (int k = 0; k < g_n; k++) {
            DEVDESC d;
            if (!slot_alive(k) || !dev_info(k, &d))
                continue;
            const SLOT *s = &g_slot[k];
            UINT flags = MF_STRING;
            if (s->level < 0) {
                flags |= MF_GRAYED;          /* 没电量可选，只做说明 */
                /* 只按「谁不提供」区分：dongle 不回话 ≠ 手柄不报电量。
                   别写「有线连接」——第三方 XInput dongle 无线也报有线。 */
                const WCHAR *why = L"电量不可读";
                if (text_has(d.detail, L"无应答"))
                    why = L"电量不可读（dongle 不提供）";
                else if (text_has(d.type, L"手柄"))
                    why = L"电量不可读（手柄不提供）";
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

    /* 刷新间隔：默认 5 分钟。每次查询都要叫醒无线设备的射频，调太勤会明显吃掉续航。 */
    {
        HMENU iv = CreatePopupMenu();
        for (int k = 0; k < IV_COUNT; k++)
            AppendMenuW(iv, MF_STRING | (g_pollMs == kIntervals[k].ms ? MF_CHECKED : 0),
                        MENU_IV_BASE + k, kIntervals[k].label);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)iv, L"刷新间隔");
    }

    /* 使用统计：累计连接时长 + 历史连接过的设备 */
    {
        HMENU st = CreatePopupMenu();
        fill_stats_menu(st);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)st, L"使用统计");
    }

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, MENU_RESCAN, L"重新扫描设备");
    AppendMenuW(m, MF_STRING, MENU_RESET, L"重置所有信息");
    AppendMenuW(m, MF_STRING, MENU_DIAG, L"导出诊断信息…");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, MENU_UNINSTALL, L"卸载并清除所有数据…");
    AppendMenuW(m, MF_STRING, MENU_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessage(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ---------------- 使用时长与历史设备 ----------------
   历史表同时承担三件事：统计累计连接时长、记录最后在线时间、
   以及给「设备已经断开」留一条可查的记录。存在 HKCU\\Software\\BattTray
   的 H0..H11，格式 "名称|累计秒|最后在线(分钟)"，一条一个 REG_SZ。 */

static DWORD uw_atoi(const WCHAR *s)
{
    DWORD v = 0;
    int n = 0;
    while (s[n] >= L'0' && s[n] <= L'9' && n < 9) {
        v = v * 10 + (DWORD)(s[n] - L'0');
        n++;
    }
    return v;
}

/* "名称|秒|分钟"，用指针往后切片，避免依赖 CRT 的字符串函数 */
static void hist_parse(const WCHAR *s, HIST *h)
{
    int n = 0;
    while (s[n] && s[n] != L'|' && n < 63) {
        h->name[n] = s[n];
        n++;
    }
    h->name[n] = 0;
    if (!h->name[0] || s[n] != L'|')
        return;

    const WCHAR *p = s + n + 1;
    h->sec = uw_atoi(p);
    while (*p && *p != L'|')
        p++;
    h->stamp = (*p == L'|') ? uw_atoi(p + 1) : 0;
    h->used = TRUE;
}

static void hist_load(void)
{
    ZeroMemory(g_hist, sizeof(g_hist));
    for (int i = 0; i < HIST_MAX; i++) {
        WCHAR key[8], buf[160];
        wsprintfW(key, L"H%d", i);
        reg_get_str(key, buf, 160);
        if (buf[0])
            hist_parse(buf, &g_hist[i]);
    }
}

static void hist_save(void)
{
    for (int i = 0; i < HIST_MAX; i++) {
        WCHAR key[8];
        wsprintfW(key, L"H%d", i);
        if (!g_hist[i].used) {
            reg_set_str(key, L"");
            continue;
        }
        WCHAR buf[160];
        wsprintfW(buf, L"%s|%u|%u", g_hist[i].name, g_hist[i].sec, g_hist[i].stamp);
        reg_set_str(key, buf);
    }
}

static int hist_find(const WCHAR *name)
{
    for (int i = 0; i < HIST_MAX; i++)
        if (g_hist[i].used && lstrcmpW(g_hist[i].name, name) == 0)
            return i;
    return -1;
}

/* 找到或新建一条记录；满了就顶掉最久没出现的那台 */
static int hist_slot(const WCHAR *name)
{
    int idx = hist_find(name);
    if (idx >= 0)
        return idx;

    for (int i = 0; i < HIST_MAX; i++) {
        if (!g_hist[i].used) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        int oldest = 0;
        for (int i = 1; i < HIST_MAX; i++)
            if (g_hist[i].stamp < g_hist[oldest].stamp)
                oldest = i;
        idx = oldest;
    }
    ZeroMemory(&g_hist[idx], sizeof(HIST));
    lstrcpynW(g_hist[idx].name, name, 64);
    g_hist[idx].used = TRUE;
    g_hist[idx].stamp = stamp_now();
    return idx;
}

static void hist_touch(const WCHAR *name, DWORD addSec)
{
    int idx = hist_slot(name);
    g_hist[idx].sec += addSec;
    g_hist[idx].stamp = stamp_now();
    hist_save();
}

/* 设备身份：品牌 + 类别。同一品牌同类型的两台设备会被并成一条，
   这种情况很少见，换来的是槽位顺序变化时统计不会错乱。 */
static void dev_key(int i, WCHAR *out, int cap)
{
    DEVDESC d;
    if (dev_info(i, &d))
        wsprintfW(out, L"%s %s", d.brand, d.type);
    else
        lstrcpynW(out, L"未知设备", cap);
}

static void fmt_dur(DWORD sec, WCHAR *out)
{
    if (sec < 60)
        wsprintfW(out, L"%u 秒", sec);
    else if (sec < 3600)
        wsprintfW(out, L"%u 分钟", sec / 60);
    else if (sec < 86400)
        wsprintfW(out, L"%u 小时 %u 分钟", sec / 3600, (sec % 3600) / 60);
    else
        wsprintfW(out, L"%u 天 %u 小时", sec / 86400, (sec % 86400) / 3600);
}

static void fmt_ago(DWORD stamp, WCHAR *out)
{
    DWORD now = stamp_now();
    if (now <= stamp) {
        lstrcpyW(out, L"刚刚");
        return;
    }
    DWORD m = now - stamp;
    if (m < 60)
        wsprintfW(out, L"%u 分钟前", m);
    else if (m < 1440)
        wsprintfW(out, L"%u 小时前", m / 60);
    else
        wsprintfW(out, L"%u 天前", m / 1440);
}

/* 累计连接时长 = 历史已结算 + 本次连接已持续的时间 */
static DWORD slot_total_sec(int i)
{
    if (i < 0 || i >= g_n)
        return 0;

    WCHAR key[64];
    dev_key(i, key, 64);
    int idx = hist_find(key);
    DWORD total = (idx >= 0) ? g_hist[idx].sec : 0;

    if (g_slot[i].connSince) {
        ULONGLONG d = GetTickCount64() - g_slot[i].connSince;
        total += (DWORD)(d / 1000);
    }
    return total;
}

/* 把本次连接这一段结算进历史，并停表 */
static void slot_settle(int i)
{
    if (i < 0 || i >= g_n || !g_slot[i].connSince)
        return;

    ULONGLONG d = GetTickCount64() - g_slot[i].connSince;
    DWORD add = (DWORD)(d / 1000);
    g_slot[i].connSince = 0;

    if (add > 0) {
        WCHAR key[64];
        dev_key(i, key, 64);
        hist_touch(key, add);
    }
}

/* 「使用统计」子菜单：在线的排前面标「使用中」，断开过的按最后在线时间排在下面。
   条目都是灰的——这里只看不改，唯一可点的是最后那条「清除历史记录」。 */
static void fill_stats_menu(HMENU s)
{
    WCHAR t[220], dur[48], ago[48];
    BOOL listed[HIST_MAX];
    ZeroMemory(listed, sizeof(listed));
    int online = 0;

    for (int i = 0; i < g_n; i++) {
        if (!slot_alive(i) || !g_slot[i].connSince)
            continue;

        WCHAR key[64];
        dev_key(i, key, 64);
        fmt_dur(slot_total_sec(i), dur);
        wsprintfW(t, L"%s · 累计 %s（使用中）", key, dur);
        AppendMenuW(s, MF_STRING | MF_GRAYED, MENU_HIST_BASE + i, t);

        int h = hist_find(key);
        if (h >= 0)
            listed[h] = TRUE;
        online++;
    }

    int histShown = 0;
    for (int h = 0; h < HIST_MAX; h++) {
        if (!g_hist[h].used || listed[h])
            continue;
        if (histShown == 0 && online > 0)
            AppendMenuW(s, MF_SEPARATOR, 0, NULL);
        fmt_dur(g_hist[h].sec, dur);
        fmt_ago(g_hist[h].stamp, ago);
        wsprintfW(t, L"%s · 累计 %s · %s", g_hist[h].name, dur, ago);
        AppendMenuW(s, MF_STRING | MF_GRAYED, MENU_HIST_BASE + 30 + h, t);
        histShown++;
    }

    if (online == 0 && histShown == 0) {
        AppendMenuW(s, MF_STRING | MF_GRAYED, MENU_HIST_BASE, L"还没有记录");
        return;
    }

    AppendMenuW(s, MF_SEPARATOR, 0, NULL);
    AppendMenuW(s, MF_STRING, MENU_HIST_CLEAR, L"清除历史记录");
}

/* 清历史：仍在线的设备从零重新计时，但不会因此被判成「断开」 */
static void clear_history(void)
{
    ZeroMemory(g_hist, sizeof(g_hist));
    hist_save();

    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_n; i++) {
        if (g_slot[i].dead)
            continue;
        g_slot[i].connSince = g_slot[i].linked ? now : 0;
    }

    notify_tip(L"历史记录已清除", L"累计使用时长从零开始统计。", NIIF_INFO);
}

/* 诊断文件末尾追加使用统计（prov_diag 用 UTF-8 + BOM 写，这里按同样编码追加） */
static void append_usage(const WCHAR *path)
{
    static WCHAR txt[4096];
    WCHAR line[256], dur[48], ago[48];

    lstrcpyW(txt, L"\r\n使用统计（累计连接时长，只算连接中的时间）\r\n");

    int online = 0;
    for (int i = 0; i < g_n; i++) {
        if (!slot_alive(i))
            continue;
        WCHAR key[64];
        dev_key(i, key, 64);
        if (g_slot[i].connSince) {
            fmt_dur(slot_total_sec(i), dur);
            wsprintfW(line, L"  [在线] %s · 累计 %s\r\n", key, dur);
        } else {
            wsprintfW(line, L"  [在线] %s · 未计时（还没读到实时值）\r\n", key);
        }
        lstrcatW(txt, line);
        online++;
    }
    if (!online)
        lstrcatW(txt, L"  （当前没有已连接的设备）\r\n");

    BOOL any = FALSE;
    for (int h = 0; h < HIST_MAX; h++) {
        if (!g_hist[h].used)
            continue;
        fmt_dur(g_hist[h].sec, dur);
        fmt_ago(g_hist[h].stamp, ago);
        wsprintfW(line, L"  [历史] %s · 累计 %s · 最后在线 %s\r\n", g_hist[h].name, dur, ago);
        lstrcatW(txt, line);
        any = TRUE;
    }
    if (!any)
        lstrcatW(txt, L"  （没有历史记录）\r\n");

    int len = lstrlenW(txt);
    int need = WideCharToMultiByte(CP_UTF8, 0, txt, len, NULL, 0, NULL, NULL);
    if (need <= 0)
        return;

    BYTE *out = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)need + 8);
    if (!out)
        return;
    WideCharToMultiByte(CP_UTF8, 0, txt, len, (char *)out, need, NULL, NULL);

    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, out, (DWORD)need, &w, NULL);
        CloseHandle(f);
    }
    HeapFree(GetProcessHeap(), 0, out);
}

/* ---------------- 读数处理 ---------------- */

static void slot_reg_name(int i, const WCHAR *prefix, WCHAR *out)
{
    wsprintfW(out, L"%s%d", prefix, i);
}

/* 判定设备已经断开：停表、撤图标、从列表里去掉。
   槽位本身留着（序号要和协议层一一对应），设备回来时原地复活，
   这样托盘 uID 不变，Windows 记着的图标位置也不会丢。 */
static void slot_mark_dead(int i)
{
    if (i < 0 || i >= g_n || g_slot[i].dead)
        return;

    SLOT *s = &g_slot[i];
    slot_settle(i);              /* 先把这一段连接时长结算进历史 */

    s->dead = TRUE;
    s->linked = FALSE;
    s->probe = 0;
    s->fail = 0;
    s->level = -1;
    s->charging = -1;
    s->cachedMin = 0;
    s->nextProbe = GetTickCount64() + REVIVE_MS;

    /* 让下次启动别把离线设备的旧读数当缓存又显示出来 */
    WCHAR key[16];
    slot_reg_name(i, L"L", key);
    reg_set(key, 0xFFFFFFFFu);

    if (s->icon) {
        DestroyIcon(s->icon);
        s->icon = NULL;
    }

    ensure_sel();
    tray_sync();
    if (g_float && IsWindowVisible(g_float))
        float_render();
}

static void apply_reading(int i, const READING *r)
{
    if (i < 0 || i >= g_n || !r->valid || r->level < 0)
        return;

    SLOT *s = &g_slot[i];
    BOOL wasLinked = s->linked;
    BOOL wasDead = s->dead;

    if (wasDead) {
        /* 设备回来了：清掉离线状态，托盘图标与菜单项重新出现。
           电量先归 -1，保证 set_level 认得出「变了」，图标会重画。 */
        s->dead = FALSE;
        s->probe = 0;
        s->fail = 0;
        s->level = -1;
        s->charging = -1;
        s->cachedMin = 0;
        s->lowNotified = FALSE;
    }

    /* 从「离线」变成「在线」才开始计时；一段连接只开一次表 */
    if (!s->connSince) {
        WCHAR k2[64];
        dev_key(i, k2, 64);
        int h = hist_slot(k2);
        g_hist[h].stamp = stamp_now();
        hist_save();
        s->connSince = GetTickCount64();
    }

    s->linked = TRUE;
    s->fail = 0;
    s->probe = 0;
    s->lastTick = GetTickCount64();
    s->cachedMin = 0;

    WCHAR key[16];
    slot_reg_name(i, L"L", key);
    if ((int)reg_get(key, 0xFFFFFFFF) != r->level) {
        reg_set(key, (DWORD)r->level);
        slot_reg_name(i, L"S", key);
        reg_set(key, stamp_now());
    }

    if (wasDead || !wasLinked)
        slot_update(i);          /* 缓存态 → 实时态，颜色要提亮 */
    set_level(i, r->level, r->charging);

    if (wasDead)
        tray_sync();             /* 离线时图标被撤掉了，得补回来 */
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
            g_slot[i].lastTick = GetTickCount64();
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
    g_nextPoll = g_lastScan + (ULONGLONG)g_pollMs;

    /* 刚扫完给所有设备一段宽限：扫描本身花掉的时间不该算进离线判定 */
    for (int i = 0; i < g_n; i++)
        if (!g_slot[i].linked)
            g_slot[i].lastTick = g_lastScan;

    if (g_float && IsWindowVisible(g_float))
        float_render();
}

static void poll_all(void)
{
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_n; i++) {
        if (g_slot[i].dead || prov_is_passive(i))
            continue;
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (prov_poll(i, &r)) {
            g_slot[i].fail = 0;
            apply_reading(i, &r);
        } else {
            g_slot[i].fail++;
            /* 连读两次失败、且距上次成功已超过 90 秒，才认为设备睡了/走了。
               间隔调长以后，一次偶发失败不该让图标立刻变暗。 */
            if (g_slot[i].linked && g_slot[i].fail >= 2 &&
                now - g_slot[i].lastTick > 90000) {
                g_slot[i].linked = FALSE;
                slot_update(i);
            }
        }
    }
}

/* 主动查询型设备两次成功读数之间的兜底时限：轮询间隔拉长时跟着放宽，
   否则调成「每 30 分钟」后，设备会在两次轮询之间被误判成离线。 */
static ULONGLONG active_timeout(void)
{
    ULONGLONG v = (ULONGLONG)g_pollMs * 3;
    return v < 90000ULL ? 90000ULL : v;
}

/* 断连判定。
   只对「读到过电量」的设备判定——读不到电量的设备在协议层扫描时就已经被
   过滤掉、根本不进列表，所以这里不会遇到 level<0 的槽位。
   设备真的关机/断开才走这里：停表、撤图标、从菜单里去掉。 */
static void check_offline(ULONGLONG now)
{
    for (int i = 0; i < g_n; i++) {
        SLOT *s = &g_slot[i];
        if (s->dead || s->level < 0)
            continue;

        /* 复查阶段：加密探测，连失 GONE_PROBE_MAX 次才认定离线 */
        if (s->probe > 0) {
            if (now < s->nextProbe)
                continue;
            s->nextProbe = now + GONE_PROBE_MS;
            READING r;
            ZeroMemory(&r, sizeof(r));
            if (prov_poll(i, &r)) {
                s->probe = 0;
                apply_reading(i, &r);
            } else if (++s->probe > GONE_PROBE_MAX) {
                slot_mark_dead(i);
            }
            continue;
        }

        if (s->passive) {
            /* 被动型没法主动询问，只能靠「还有没有帧」判断 */
            if (now - s->lastTick > GONE_PASSIVE_MS)
                slot_mark_dead(i);
            continue;
        }

        /* 主动型：一次查询失败就先别急着下结论，进入复查 */
        if (s->fail >= 1 || now - s->lastTick > active_timeout()) {
            s->probe = 1;
            s->nextProbe = now + GONE_PROBE_MS;
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

    /* 已离线的设备定期试一次，能应答就复活。
       被动上报型不用管：消息循环还挂着它的读取事件，收到帧就会自己回来。 */
    for (int i = 0; i < g_n; i++) {
        SLOT *s = &g_slot[i];
        if (!s->dead || prov_is_passive(i))
            continue;
        if (now < s->nextProbe)
            continue;
        s->nextProbe = now + REVIVE_MS;
        prov_reset_backoff(i);
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (prov_poll(i, &r))
            apply_reading(i, &r);
    }

    /* 被动型：太久没上报就转成「缓存」显示 */
    for (int i = 0; i < g_n; i++) {
        if (g_slot[i].dead || !prov_is_passive(i) || !g_slot[i].linked)
            continue;
        if (now - g_slot[i].lastTick > 12000) {
            g_slot[i].linked = FALSE;
            slot_update(i);
        }
    }

    if (now >= g_nextPoll) {
        g_nextPoll = now + (ULONGLONG)g_pollMs;
        poll_all();
    }

    check_offline(now);

    /* 使用时长定期落盘：强杀进程时最多丢 5 分钟，而不是整段会话 */
    if (now - g_lastFlush > FLUSH_MS) {
        g_lastFlush = now;
        for (int i = 0; i < g_n; i++) {
            if (g_slot[i].dead || !g_slot[i].connSince)
                continue;
            DWORD add = (DWORD)((now - g_slot[i].connSince) / 1000);
            if (add > 0) {
                g_slot[i].connSince = now;
                WCHAR key[64];
                dev_key(i, key, 64);
                hist_touch(key, add);
            }
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
                } else {
                    /* 没拖动 = 点一下：在占托盘的设备之间轮换 */
                    int rl[PROV_MAX];
                    int rn = tray_list(rl);
                    if (rn > 1) {
                        int pos = 0;
                        for (int k = 0; k < rn; k++)
                            if (rl[k] == g_sel)
                                pos = k;
                        g_sel = rl[(pos + 1) % rn];
                        float_render();
                    }
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
        /* 「没有已连接设备」的入口图标：左右键都直接给菜单 */
        if ((UINT)wp == TRAY_UID_ORPHAN) {
            show_menu(-1);
            return 0;
        }
        int i = (int)wp - TRAY_UID_BASE;
        if (!slot_alive(i))
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
        if (id >= MENU_IV_BASE && id < MENU_IV_BASE + IV_COUNT) {
            g_pollMs = kIntervals[id - MENU_IV_BASE].ms;
            reg_set(L"IV", (DWORD)g_pollMs);
            prov_set_poll_ms(g_pollMs);
            g_nextPoll = GetTickCount64();      /* 换完立刻按新间隔读一次 */
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
        case MENU_RESET:
            reset_all();
            break;
        case MENU_UNINSTALL:
            uninstall();
            break;
        case MENU_HIST_CLEAR:
            clear_history();
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
            /* 设备（含接收器）被拔掉：先把这一段连接时长记下来再重扫。
               不要把 g_n 提前清零——rebuild_slots 还要靠旧槽位回收图标句柄。 */
            for (int i = 0; i < g_n; i++)
                if (!g_slot[i].dead)
                    slot_settle(i);
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
        /* 关机 / 注销：把还在计时的那几段结算掉，别让最后一段白算 */
        if (wp)
            for (int i = 0; i < g_n; i++)
                if (!g_slot[i].dead)
                    slot_settle(i);
        return 0;

    default: {
        static UINT s_taskbarCreated;
        if (!s_taskbarCreated)
            s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        if (msg == s_taskbarCreated) {
            /* explorer 重启过：旧图标句柄全失效，按当前状态重新注册 */
            ZeroMemory(g_trayOn, sizeof(g_trayOn));
            g_orphanOn = FALSE;
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

/* 重置所有信息：清空历史/使用时长、电量缓存，并把当前所有设备按离线处理
   后重新扫描。设置类（浮窗开关、刷新间隔、开机自启）保留不动。 */
static void reset_all(void)
{
    ZeroMemory(g_hist, sizeof(g_hist));
    hist_save();

    for (int i = 0; i < PROV_MAX; i++) {
        WCHAR key[16];
        wsprintfW(key, L"N%d", i); reg_set_str(key, L"");
        wsprintfW(key, L"L%d", i); reg_set(key, 0xFFFFFFFFu);
        wsprintfW(key, L"S%d", i); reg_set(key, 0);
    }
    for (int i = 0; i < g_n; i++) {
        if (!g_slot[i].dead)
            slot_mark_dead(i);
    }
    ensure_sel();
    tray_sync();
    do_scan();
    notify_tip(L"已重置所有信息", L"历史记录与电量缓存已清空。", NIIF_INFO);
}

/* 卸载：清空注册表数据、移除开机自启，并把 exe 标记为下次重启删除。
   程序运行中删不掉自己（exe 被文件锁占用），所以用 MoveFileEx 的延迟删除；
   注册表残留当时就清干净了，exe 在下次重启时被 Windows 自动删除。 */
static void uninstall(void)
{
    int r = MessageBoxW(g_hwnd,
        L"将删除本程序的全部数据（使用统计、设置、缓存），\n"
        L"并移除开机自启。程序文件会在下次重启时自动删除。\n\n"
        L"确定要卸载吗？",
        L"BattTray 卸载", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    if (r != IDYES)
        return;

    /* 1. 删掉整个数据键（含子键，RegDeleteTree 连值一起清） */
    RegDeleteTreeW(HKEY_CURRENT_USER, kRegKey);

    /* 2. 移除开机自启 */
    autostart_apply(FALSE);

    /* 3. 标记 exe 下次重启删除 */
    WCHAR path[MAX_PATH];
    if (GetModuleFileNameW(NULL, path, MAX_PATH))
        MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);

    /* 4. 退出。销毁窗口 → 消息循环结束，退出路径不会再写回注册表。 */
    DestroyWindow(g_hwnd);
}

/* ---------------- 入口 ---------------- */

/* 应用图标：编译后由 tools/embedres.c 写进 exe 的资源段
   （RT_GROUP_ICON id 1 + 若干 RT_ICON），运行时直接按 ID 加载。
   这样 exe 在资源管理器 / 任务栏 / Alt+Tab 里都是电池图标。 */
static HICON app_icon(HINSTANCE inst, int cx, int cy)
{
    HICON ic = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, cx, cy,
                                 LR_DEFAULTCOLOR);
    if (ic)
        return ic;
    return LoadIconW(NULL, IDI_APPLICATION);
}

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
    wc.hIcon   = app_icon(inst, 32, 32);   /* 任务栏 / Alt+Tab / 资源管理器 */
    wc.hIconSm = app_icon(inst, 16, 16);   /* 标题栏小图标 */
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

    /* 刷新间隔：读注册表；不是菜单里的合法值就回落到默认 5 分钟 */
    {
        int ms = (int)reg_get(L"IV", (DWORD)g_pollMs);
        int ok = 0;
        for (int k = 0; k < IV_COUNT; k++) {
            if (kIntervals[k].ms == ms) {
                ok = 1;
                break;
            }
        }
        g_pollMs = ok ? ms : 300000;
    }
    prov_set_poll_ms(g_pollMs);

    /* 先扫描：托盘图标的数量取决于设备数 */
    hist_load();
    g_n = prov_scan();
    load_cache();
    save_cache_names();

    for (int i = 0; i < g_n; i++) {
        g_slot[i].passive = prov_is_passive(i);
        g_slot[i].icon = build_icon(g_slot[i].level, !g_slot[i].linked);
        /* 启动瞬间不能算「很久没上报」，否则第一次 tick 就会把设备判成离线 */
        g_slot[i].lastTick = GetTickCount64();
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
    g_nextPoll = g_lastScan + (ULONGLONG)g_pollMs;
    g_lastFlush = g_lastScan;

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
            if (prov_read_ready(i, &rd)) {
                /* 收到帧就说明设备在线，哪怕这一帧没解析出电量 */
                g_slot[i].lastTick = GetTickCount64();
                apply_reading(i, &rd);
            }
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
    /* 退出前把最后一段连接时长记下来（必须在 prov_close 之前，还得能取到设备名） */
    for (int i = 0; i < g_n; i++)
        if (!g_slot[i].dead)
            slot_settle(i);

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
