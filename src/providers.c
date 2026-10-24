// providers.c - 按「协议族」实现的无线设备电量读取
//
// 设计说明：
//   * 协议族（而非品牌）驱动，新品牌只要落在同一族就自动覆盖；
//   * 被动型（雷柏）靠设备主动上报，零轮询；
//   * 查询型（罗技/雷蛇/Compx/ROG）按间隔发送「只读」查询；
//   * 全部失败时回退到 Windows 电池类设备（蓝牙鼠标通用）。
//
// 协议来源（均为公开资料，详见 README 的「参考与致谢」）：
//   雷柏  : JD13703202/rapoo-vt3-battery  —— 实测本机设备
//   Compx : Fan4Metal/ATK_tray、libatk-rs —— ATK/VXE/VGN/达尔优/G-Wolves
//   罗技  : Logitech HID++ 2.0 草案规范、GearPower —— 0x1004/0x1000
//   雷蛇  : OpenRazer、GearPower 实测报文       —— 90 字节特性报告
//   ROG   : GearPower 实测报文                  —— CC 12 07

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <winioctl.h>
#include <batclass.h>

#include "providers.h"

#define MAX_DEV   96
#define PATH_CAP  512

typedef struct {
    WCHAR  path[PATH_CAP];
    USHORT vid, pid;
    USHORT usagePage, usage;
    USHORT inLen, outLen, featLen;
} DEV;

enum {
    K_NONE = 0,
    K_RAPOO,
    K_COMPX17,
    K_COMPX64,
    K_HIDPP2,
    K_RAZER,
    K_ROG,
    K_LEOBOG,
    K_REALTEK,
    K_FLYDIGI,
    K_WINBAT
};

/* 品牌识别：协议族负责「怎么读」，这张表只负责「叫对名字」。
   即使某品牌落在通用协议族里，也尽量显示它自己的名字。 */
typedef struct { USHORT vid; const WCHAR *name; } BRAND;

static const BRAND kBrands[] = {
    { 0x24AE, L"雷柏 Rapoo" },
    { 0x046D, L"罗技 Logitech" },
    { 0x1532, L"雷蛇 Razer" },
    { 0x0B05, L"华硕 ROG" },
    { 0x37D7, L"飞智 Flydigi" },
    { 0x3554, L"Compx 方案" },
    { 0x1E7D, L"冰豹 ROCCAT" },
    { 0x1038, L"赛睿 SteelSeries" },
    { 0x1B1C, L"海盗船 Corsair" },
    { 0x1462, L"微星 MSI" },
    { 0x1044, L"技嘉 Gigabyte" },
    { 0x0BDA, L"Realtek 方案" },
    { 0x05AC, L"Apple" },
    { 0x17EF, L"联想 Lenovo" },
    { 0x04F2, L"东芝 Toshiba" },
};

static const WCHAR *brand_name(USHORT vid, USHORT pid)
{
    /* PID 级特例：与 Compx VID 共用但协议不同 */
    if (vid == 0x3554 && pid == 0xFA09)
        return L"LEOBOG";
    if (vid == 0x3554 && pid == 0xFA0A)
        return L"LEOBOG";

    for (int i = 0; i < (int)(sizeof(kBrands) / sizeof(kBrands[0])); i++) {
        if (kBrands[i].vid == vid)
            return kBrands[i].name;
    }
    return L"未知品牌";
}

static int   g_kind = K_NONE;
static HANDLE g_h;                       /* HID 句柄（WINBAT 用 g_bat） */
static HANDLE g_bat;
static WCHAR g_name[96];
static WCHAR g_detail[256];

/* 被动监听状态 */
static OVERLAPPED g_ov;
static HANDLE g_ev;
static BOOL   g_pending;
static BYTE   g_buf[256];
static BYTE   g_lastRaw[16];
static int    g_lastRawLen;

/* 罗技探测到的索引 */
static BYTE g_ppRid, g_ppDev, g_ppBat04, g_ppBat00;

/* 匹配时记录的真实报告长度（轮询要按设备实际情况发送） */
static int  g_outLen, g_inLen, g_featLen;

/* 诊断日志 */
static WCHAR g_log[8192];
static int   g_logLen;
static int   g_lastLevel = -1;

static void logf(const WCHAR *fmt, ...)
{
    if (g_logLen > 7600)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfW(g_log + g_logLen, fmt, ap);
    va_end(ap);
    g_logLen += n;
    if (g_logLen < 8190) {
        g_log[g_logLen++] = L'\r';
        g_log[g_logLen++] = L'\n';
        g_log[g_logLen] = 0;
    }
}

/* ---------------- 设备枚举 ---------------- */

static int dev_enum(DEV *out, int max)
{
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO hdi = SetupDiGetClassDevsW(&guid, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdi == INVALID_HANDLE_VALUE)
        return 0;

    int n = 0;
    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);
    BYTE detail[1024];

    for (DWORD i = 0; n < max && SetupDiEnumDeviceInterfaces(hdi, NULL, &guid, i, &did); i++) {
        DWORD need = 0;
        if (!SetupDiGetDeviceInterfaceDetailW(hdi, &did, NULL, 0, &need, NULL) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (need == 0 || need > sizeof(detail))
            continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pd = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)detail;
        pd->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hdi, &did, pd, need, &need, NULL))
            continue;

        HANDLE h = CreateFileW(pd->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        HIDD_ATTRIBUTES at;
        at.Size = sizeof(at);
        PHIDP_PREPARSED_DATA pp = NULL;
        HIDP_CAPS caps;
        ZeroMemory(&caps, sizeof(caps));

        DEV *d = &out[n];
        ZeroMemory(d, sizeof(*d));
        lstrcpynW(d->path, pd->DevicePath, PATH_CAP);
        if (HidD_GetAttributes(h, &at)) {
            d->vid = at.VendorID;
            d->pid = at.ProductID;
        }
        if (HidD_GetPreparsedData(h, &pp)) {
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                d->usagePage = caps.UsagePage;
                d->usage     = caps.Usage;
                d->inLen     = caps.InputReportByteLength;
                d->outLen    = caps.OutputReportByteLength;
                d->featLen   = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);

        if (d->usagePage) {
            logf(L"HID vid=%04X pid=%04X page=%04X usage=%04X in=%u out=%u feat=%u",
                 d->vid, d->pid, d->usagePage, d->usage, d->inLen, d->outLen, d->featLen);
            n++;
        }
    }
    SetupDiDestroyDeviceInfoList(hdi);
    return n;
}

static HANDLE dev_open_ex(const WCHAR *path, BOOL zeroFirst)
{
    DWORD acc = zeroFirst ? 0 : (GENERIC_READ | GENERIC_WRITE);
    DWORD acc2 = zeroFirst ? (GENERIC_READ | GENERIC_WRITE) : 0;
    HANDLE h = CreateFileW(path, acc, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileW(path, acc2, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    return h;
}

/* ---------------- 重叠 I/O ---------------- */

static BOOL io_read(HANDLE h, void *buf, DWORD len, DWORD *got, DWORD ms)
{
    if (got)
        *got = 0;
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ev)
        return FALSE;
    ov.hEvent = ev;

    BOOL ok = FALSE;
    DWORD g = 0;
    if (ReadFile(h, buf, len, NULL, &ov)) {
        if (GetOverlappedResult(h, &ov, &g, FALSE)) {
            if (got) *got = g;
            ok = TRUE;
        }
    } else if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ev, ms) == WAIT_OBJECT_0) {
            if (GetOverlappedResult(h, &ov, &g, FALSE)) {
                if (got) *got = g;
                ok = TRUE;
            }
        } else {
            CancelIo(h);
            WaitForSingleObject(ev, 200);
        }
    }
    CloseHandle(ev);
    return ok;
}

static BOOL io_write(HANDLE h, const void *buf, DWORD len, DWORD ms)
{
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ev)
        return FALSE;
    ov.hEvent = ev;

    BOOL ok = FALSE;
    DWORD g = 0;
    if (WriteFile(h, buf, len, NULL, &ov)) {
        ok = GetOverlappedResult(h, &ov, &g, FALSE) ? TRUE : FALSE;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ev, ms) == WAIT_OBJECT_0)
            ok = GetOverlappedResult(h, &ov, &g, FALSE) ? TRUE : FALSE;
        else {
            CancelIo(h);
            WaitForSingleObject(ev, 200);
        }
    }
    CloseHandle(ev);
    return ok;
}

/* ---------------- 协议族：Compx（ATK / VXE / VGN / 达尔优 / G-Wolves 等） ---------------- */

static BOOL is_vendor_page(USHORT page) { return page >= 0xFF00; }

static BOOL compx17_query(HANDLE h, int outLen, int inLen, READING *r)
{
    BYTE tx[32];
    int  n = (outLen >= 12 && outLen <= 32) ? outLen : 17;
    ZeroMemory(tx, sizeof(tx));
    tx[0] = 0x08;                       /* Report ID */
    tx[1] = 0x04;                       /* 命令：读电量 */

    BYTE sum = 0;
    for (int i = 0; i < n - 1; i++)
        sum = (BYTE)(sum + tx[i]);
    tx[n - 1] = (BYTE)(0x55 - sum);     /* 校验：0x55 - 其余字节之和 */

    if (!io_write(h, tx, (DWORD)n, 300))
        return FALSE;

    BYTE rx[64];
    DWORD got = 0;
    int   rd = (inLen > 0 && inLen <= (int)sizeof(rx)) ? inLen : 17;
    if (!io_read(h, rx, (DWORD)rd, &got, 400))
        return FALSE;

    /* 响应第 6 字节为电量，第 7 字节为有线标志（含 Report ID 计偏移） */
    if (got < 8)
        return FALSE;
    int lv = rx[6];
    if (lv < 0 || lv > 100)
        return FALSE;

    r->level = lv;
    r->charging = -1;
    r->valid = TRUE;
    return TRUE;
}

static BOOL compx64_query(HANDLE h, int outLen, int inLen, READING *r)
{
    BYTE tx[80];
    int  n = (outLen >= 64 && outLen <= 80) ? outLen : 64;
    int  inl = (inLen >= 64 && inLen <= (int)sizeof(tx)) ? inLen : 64;

    for (int wired = 0; wired < 2; wired++) {
        ZeroMemory(tx, sizeof(tx));
        tx[0] = 0x08;
        tx[1] = (BYTE)(wired ? 0x7C : 0x7D);
        tx[2] = 0x72;
        tx[3] = 0x02;
        tx[4] = 0x00;
        tx[5] = (BYTE)(wired ? 0x00 : 0x01);
        tx[6] = 0x07;
        tx[7] = 0x01;

        if (!io_write(h, tx, (DWORD)n, 300))
            continue;

        BYTE rx[96];
        DWORD got = 0;
        if (!io_read(h, rx, (DWORD)inl, &got, 400))
            continue;
        if (got < 8 || rx[1] != 0x72 || rx[5] != 0x07)
            continue;
        if (wired)
            return FALSE;               /* 有线响应不含可信电量 */

        int lv = rx[7];
        if (lv < 0 || lv > 100)
            return FALSE;
        r->level = lv;
        r->charging = -1;
        r->valid = TRUE;
        return TRUE;
    }
    return FALSE;
}

/* ---------------- 协议族：罗技 HID++ 2.0 ---------------- */

static BOOL hidpp_call(HANDLE h, BYTE rid, int outLen, int inLen, BYTE dev, BYTE feat,
                       BYTE func, const BYTE *params, int np, BYTE *resp)
{
    BYTE tx[32];
    int  n = (outLen > 4 && outLen <= (int)sizeof(tx)) ? outLen : 7;
    ZeroMemory(tx, sizeof(tx));
    tx[0] = rid;
    tx[1] = dev;
    tx[2] = feat;
    tx[3] = (BYTE)((func << 4) | 0x01);   /* software id 固定 1 */
    for (int i = 0; i < np && i < 16; i++)
        tx[4 + i] = params[i];

    if (!io_write(h, tx, (DWORD)n, 250))
        return FALSE;

    int inl = (inLen >= 7 && inLen <= 32) ? inLen : 7;
    DWORD got = 0;
    BYTE  rx[32];
    for (int tries = 0; tries < 3; tries++) {
        ZeroMemory(rx, sizeof(rx));
        if (!io_read(h, rx, (DWORD)inl, &got, 250))
            return FALSE;
        if (got >= 4 && rx[0] == rid && rx[1] == dev && (rx[3] & 0x0F) == 0x01) {
            CopyMemory(resp, rx, got);
            return TRUE;
        }
    }
    return FALSE;
}

static BYTE hidpp_root_get_feature(HANDLE h, BYTE rid, int outLen, int inLen,
                                   BYTE dev, USHORT featureId)
{
    BYTE p[2];
    p[0] = (BYTE)(featureId >> 8);
    p[1] = (BYTE)(featureId & 0xFF);
    BYTE rx[32];
    if (!hidpp_call(h, rid, outLen, inLen, dev, 0x00, 0x00, p, 2, rx))
        return 0;
    return rx[4];               /* params[0] = 特性索引 */
}

static BOOL hidpp_probe(HANDLE h, int outLen, int inLen, READING *r, BOOL doQuery)
{
    BYTE rid = (outLen >= 20) ? 0x11 : 0x10;
    if (outLen < 20 && outLen >= 7)
        rid = 0x10;

    static const BYTE devs[] = { 0xFF, 1, 2, 3, 4, 5, 6 };
    for (int di = 0; di < (int)sizeof(devs); di++) {
        BYTE dev = devs[di];
        BYTE f04 = hidpp_root_get_feature(h, rid, outLen, inLen, dev, 0x1004);
        BYTE f00 = f04 ? 0 : hidpp_root_get_feature(h, rid, outLen, inLen, dev, 0x1000);
        if (!f04 && !f00)
            continue;

        g_ppRid = rid;
        g_ppDev = dev;
        g_ppBat04 = f04;
        g_ppBat00 = f00;
        logf(L"HID++ rid=%02X dev=%02X f1004=%02X f1000=%02X", rid, dev, f04, f00);

        if (!doQuery) {
            r->valid = TRUE;
            r->level = -1;
            r->charging = -1;
            return TRUE;
        }

        BYTE rx[32];
        if (f04 && hidpp_call(h, rid, outLen, inLen, dev, f04, 0x00, NULL, 0, rx)) {
            int lv = rx[4];
            if (lv >= 0 && lv <= 100) {
                r->level = lv;                       /* stateOfCharge */
                r->charging = (rx[6] != 0) ? 1 : 0;   /* chargingStatus */
                r->valid = TRUE;
                return TRUE;
            }
        }
        if (f00 && hidpp_call(h, rid, outLen, inLen, dev, f00, 0x00, NULL, 0, rx)) {
            int lv = rx[4];
            int st = rx[6];
            if (lv >= 0 && lv <= 100) {
                r->level = lv;
                r->charging = (st >= 1 && st <= 4) ? 1 : 0;
                r->valid = TRUE;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* ---------------- 协议族：雷蛇（90 字节特性报告） ---------------- */

static BYTE razer_crc(const BYTE *r, int len)
{
    BYTE c = 0;
    for (int i = 3; i < 89 && i < len; i++)
        c ^= r[i];
    return c;
}

static BOOL razer_query(HANDLE h, int featLen, READING *r)
{
    BYTE tx[96], rx[96];
    int  n = (featLen >= 91 && featLen <= (int)sizeof(tx)) ? 91 : 90;

    ZeroMemory(tx, sizeof(tx));
    tx[1] = 0x00;                 /* status：新命令 */
    tx[2] = 0x1F;                 /* transaction id */
    tx[6] = 0x02;                 /* data size */
    tx[7] = 0x07;                 /* command class：电源 */
    tx[8] = 0x80;                 /* command id：读电量 */
    tx[89] = razer_crc(tx, n);

    if (!HidD_SetFeature(h, tx, (ULONG)n))
        io_write(h, tx, (DWORD)n, 300);

    for (int tries = 0; tries < 8; tries++) {
        Sleep(25);
        ZeroMemory(rx, sizeof(rx));
        if (!HidD_GetFeature(h, rx, (ULONG)n)) {
            DWORD got = 0;
            if (!io_read(h, rx, (DWORD)n, &got, 250))
                continue;
        }
        if (rx[1] == 0x02 && rx[2] == 0x1F) {         /* 成功且事务号匹配 */
            int raw = rx[10];
            if (raw < 0 || raw > 255)
                return FALSE;
            r->level = (raw * 100 + 127) / 255;
            r->charging = -1;
            r->valid = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}

/* ---------------- 协议族：ASUS ROG（CC 12 07，64 字节 OUT/IN） ---------------- */

static BOOL rog_query(HANDLE h, int outLen, int inLen, READING *r)
{
    BYTE tx[80];
    int  n = (outLen >= 64 && outLen <= (int)sizeof(tx)) ? 64 : outLen;
    if (n <= 0)
        return FALSE;

    ZeroMemory(tx, sizeof(tx));
    tx[0] = 0xCC;
    tx[1] = 0x12;
    tx[2] = 0x07;
    if (!io_write(h, tx, (DWORD)n, 300))
        return FALSE;

    BYTE rx[80];
    DWORD got = 0;
    int   inl = (inLen >= 64 && inLen <= (int)sizeof(rx)) ? 64 : inLen;
    if (inl <= 0 || !io_read(h, rx, (DWORD)inl, &got, 400))
        return FALSE;
    if (got < 7 || rx[0] != 0xCC || rx[1] != 0x12 || rx[2] != 0x07)
        return FALSE;

    int lv = rx[6];
    if (lv < 0 || lv > 100)
        return FALSE;
    r->level = lv;
    r->charging = -1;
    r->valid = TRUE;
    return TRUE;
}

/* ---------------- 协议族：Realtek 0xFF01（迈从 MCHOSE 等） ---------------- */
//
// 特点：命令走 OUT 报告 0x11，状态从 IN 报告 0x13 回来，
// 电量放在 0xE2 事件标记之后一个字节。这里不写死偏移，
// 而是整帧找 0xE2 标记，兼容不同固件的字段位置微调。

static BOOL realtek_query(HANDLE h, int outLen, int inLen, READING *r)
{
    BYTE tx[64];
    int  n = (outLen >= 8 && outLen <= (int)sizeof(tx)) ? outLen : 8;

    ZeroMemory(tx, sizeof(tx));
    tx[0] = 0x11;                 /* Report ID：命令通道 */
    tx[1] = 0x01;                 /* 子命令：读状态 */
    if (!io_write(h, tx, (DWORD)n, 300))
        return FALSE;

    int rd = (inLen >= 8 && inLen <= 64) ? inLen : 8;
    for (int tries = 0; tries < 6; tries++) {
        BYTE  rx[64];
        DWORD got = 0;
        ZeroMemory(rx, sizeof(rx));
        if (!io_read(h, rx, (DWORD)rd, &got, 300))
            return FALSE;
        if (got < 4 || rx[0] != 0x13)
            continue;

        for (DWORD i = 1; i + 1 < got; i++) {
            if (rx[i] != 0xE2)
                continue;
            int lv = rx[i + 1];
            if (lv < 0 || lv > 100)
                break;
            r->level = lv;
            r->charging = -1;
            r->valid = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}

/* ---------------- 协议族：LEOBOG（3554:FA09，20 字节 13 CA） ---------------- */

static BOOL leobog_query(HANDLE h, int outLen, int inLen, READING *r)
{
    BYTE tx[32];
    int  n = (outLen >= 20 && outLen <= (int)sizeof(tx)) ? outLen : 20;

    ZeroMemory(tx, sizeof(tx));
    tx[0] = 0x13;
    tx[1] = 0xCA;
    tx[2] = 0x01;
    if (!io_write(h, tx, (DWORD)n, 300))
        return FALSE;

    BYTE  rx[32];
    DWORD got = 0;
    int   rd = (inLen >= 20 && inLen <= (int)sizeof(rx)) ? inLen : 20;
    ZeroMemory(rx, sizeof(rx));
    if (!io_read(h, rx, (DWORD)rd, &got, 350))
        return FALSE;
    if (got < 8 || rx[0] != 0x13 || rx[1] != 0xCA)
        return FALSE;

    /* 电量紧跟帧头；同时要求「充电标志」字段取值为 0/1，避免误判 */
    int lv = rx[6];
    if (lv < 0 || lv > 100)
        return FALSE;
    if (got > 7 && rx[7] > 1)
        return FALSE;

    r->level = lv;
    r->charging = (got > 7) ? (rx[7] ? 1 : 0) : -1;
    r->valid = TRUE;
    return TRUE;
}

/* ---------------- 协议族：飞智 Flydigi（37D7，FFA0，33 字节） ---------------- */

static BOOL flydigi_query(HANDLE h, int outLen, int inLen, READING *r)
{
    BYTE tx[48];
    int  n = (outLen >= 33 && outLen <= (int)sizeof(tx)) ? outLen : 33;

    ZeroMemory(tx, sizeof(tx));
    tx[1] = 0x5A;                 /* 帧头 00 5A A5，与雷柏/Compx 区分明显 */
    tx[2] = 0xA5;
    tx[3] = 0x01;
    tx[4] = 0x02;
    tx[5] = 0x03;
    if (!io_write(h, tx, (DWORD)n, 300))
        return FALSE;

    BYTE  rx[48];
    DWORD got = 0;
    int   rd = (inLen >= 33 && inLen <= (int)sizeof(rx)) ? inLen : 33;
    ZeroMemory(rx, sizeof(rx));
    if (!io_read(h, rx, (DWORD)rd, &got, 350))
        return FALSE;
    if (got < 8 || rx[1] != 0x5A || rx[2] != 0xA5)
        return FALSE;

    /* 回应镜像请求头 01 02 03，紧接着一个字节的低半字节是电量档位 */
    for (DWORD i = 3; i + 1 < got; i++) {
        if (rx[i] != 0x01 || rx[i + 1] != 0x02 || i + 2 >= got)
            continue;
        if (rx[i + 2] != 0x03)
            continue;
        DWORD p = i + 3;
        if (p >= got)
            return FALSE;
        int lv = rx[p] & 0x0F;
        if (rx[p] & 0xF0)          /* 高半字节应为状态位，非 0 则不信 */
            return FALSE;
        if (lv < 0 || lv > 15)
            return FALSE;
        r->level = lv * 100 / 15;  /* 0..15 档 → 百分比 */
        r->charging = -1;
        r->valid = TRUE;
        return TRUE;
    }
    return FALSE;
}

/* ---------------- 通用回退：Windows 电池类设备 ---------------- */

static const GUID kBatClass =
    { 0x72631E54, 0x78A4, 0x11D0, { 0xBC, 0xF7, 0x00, 0xAA, 0x00, 0xB7, 0xB3, 0x2A } };

static BOOL winbat_open(void)
{
    HDEVINFO hdi = SetupDiGetClassDevsW(&kBatClass, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdi == INVALID_HANDLE_VALUE)
        return FALSE;

    BOOL found = FALSE;
    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);
    BYTE detail[1024];

    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(hdi, NULL, &kBatClass, i, &did); i++) {
        DWORD need = 0;
        if (!SetupDiGetDeviceInterfaceDetailW(hdi, &did, NULL, 0, &need, NULL) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (need == 0 || need > sizeof(detail))
            continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pd = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)detail;
        pd->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hdi, &did, pd, need, &need, NULL))
            continue;

        /* 排除本机主电池（ACPI 枚举），只接受 HID / 蓝牙挂载的电池设备 */
        {
            const WCHAR *p = pd->DevicePath;
            while (*p == L'\\' || *p == L'?' || *p == L'.')
                p++;
            BOOL hid = (p[0] == L'H' && p[1] == L'I' && p[2] == L'D');
            BOOL bth = (p[0] == L'B' && p[1] == L'T' && p[2] == L'H');
            if (!hid && !bth) {
                logf(L"skip battery: %s", p);
                continue;
            }
        }

        HANDLE h = CreateFileW(pd->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        g_bat = h;
        found = TRUE;
    }
    SetupDiDestroyDeviceInfoList(hdi);
    return found;
}

static BOOL winbat_query(READING *r)
{
    if (!g_bat)
        return FALSE;

    DWORD bytes = 0, wait = 0;
    ULONG tag = 0;
    if (!DeviceIoControl(g_bat, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait),
                         &tag, sizeof(tag), &bytes, NULL) || tag == 0)
        return FALSE;

    BATTERY_QUERY_INFORMATION bqi;
    ZeroMemory(&bqi, sizeof(bqi));
    bqi.BatteryTag = tag;
    bqi.InformationLevel = BatteryInformation;

    BATTERY_INFORMATION bi;
    ZeroMemory(&bi, sizeof(bi));
    if (!DeviceIoControl(g_bat, IOCTL_BATTERY_QUERY_INFORMATION, &bqi, sizeof(bqi),
                         &bi, sizeof(bi), &bytes, NULL))
        return FALSE;
    if (bi.FullChargedCapacity == 0)
        return FALSE;

    BATTERY_WAIT_STATUS bws;
    ZeroMemory(&bws, sizeof(bws));
    bws.BatteryTag = tag;

    BATTERY_STATUS bs;
    ZeroMemory(&bs, sizeof(bs));
    if (!DeviceIoControl(g_bat, IOCTL_BATTERY_QUERY_STATUS, &bws, sizeof(bws),
                         &bs, sizeof(bs), &bytes, NULL))
        return FALSE;

    int lv = (int)((ULONGLONG)bs.Capacity * 100 / bi.FullChargedCapacity);
    if (lv < 0 || lv > 100)
        return FALSE;

    r->level = lv;
    r->charging = (bs.PowerState & BATTERY_CHARGING) ? 1 : 0;
    r->valid = TRUE;
    return TRUE;
}

/* ---------------- 扫描 ---------------- */

void prov_close(void)
{
    if (g_h) {
        CancelIo(g_h);
        CloseHandle(g_h);
        g_h = NULL;
    }
    if (g_bat) {
        CloseHandle(g_bat);
        g_bat = NULL;
    }
    g_pending = FALSE;
    g_kind = K_NONE;
    g_lastRawLen = 0;
    g_outLen = g_inLen = g_featLen = 0;
    g_name[0] = 0;
    g_detail[0] = 0;
    g_ppRid = g_ppDev = g_ppBat04 = g_ppBat00 = 0;
}

BOOL prov_scan(void)
{
    prov_close();
    g_logLen = 0;
    g_log[0] = 0;

    DEV dev[MAX_DEV];
    int n = dev_enum(dev, MAX_DEV);
    logf(L"---- enum %d hid interfaces ----", n);

    READING r;
    int i;

    /* 1) 雷柏：被动上报，无法即时验证，优先锁定。
       同一接收器会暴露多个 FF00 集合（0002 是电量所在的那个），
       所以先挑 usage=0002，找不到再退回其它长度合理的集合。 */
    {
        int rap = -1;
        for (i = 0; i < n; i++) {
            if (dev[i].vid != 0x24AE || dev[i].usagePage != 0xFF00)
                continue;
            if (dev[i].inLen < 4 || dev[i].inLen > 32)
                continue;
            if (dev[i].usage == 0x0002) {
                rap = i;
                break;
            }
            if (rap < 0)
                rap = i;
        }
        if (rap >= 0) {
            HANDLE h = dev_open_ex(dev[rap].path, FALSE);
            if (h != INVALID_HANDLE_VALUE) {
                g_h = h;
                g_kind = K_RAPOO;
                lstrcpyW(g_name, L"雷柏 Rapoo");
                wsprintfW(g_detail, L"vid 24AE · FF00/%04X · %u 字节被动上报",
                          dev[rap].usage, dev[rap].inLen);
                if (!g_ev)
                    g_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
                g_outLen = dev[rap].outLen;
                g_inLen = dev[rap].inLen;
                g_featLen = dev[rap].featLen;
                logf(L"match RAPOO passive page=%04X usage=%04X in=%u",
                     dev[rap].usagePage, dev[rap].usage, dev[rap].inLen);
                return TRUE;
            }
        }
    }

    /* 2) 罗技 HID++ 2.0 */
    for (i = 0; i < n; i++) {
        if (dev[i].vid != 0x046D || dev[i].usagePage != 0xFF00)
            continue;
        if (dev[i].outLen != 7 && dev[i].outLen != 20)
            continue;
        HANDLE h = dev_open_ex(dev[i].path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        ZeroMemory(&r, sizeof(r));
        if (hidpp_probe(h, dev[i].outLen, dev[i].inLen, &r, TRUE)) {
            g_h = h;
            g_kind = K_HIDPP2;
            lstrcpyW(g_name, L"罗技 Logitech");
            wsprintfW(g_detail, L"HID++ 2.0 · 特性 %s · dev %02X",
                      g_ppBat04 ? L"0x1004" : L"0x1000", g_ppDev);
            g_outLen = dev[i].outLen; g_inLen = dev[i].inLen; g_featLen = dev[i].featLen;
            logf(L"match HIDPP2");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 3) 雷蛇：90 字节特性报告，鼠标集合常被独占，需 0 权限打开 */
    for (i = 0; i < n; i++) {
        if (dev[i].vid != 0x1532 || dev[i].featLen < 90)
            continue;
        HANDLE h = dev_open_ex(dev[i].path, TRUE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        ZeroMemory(&r, sizeof(r));
        if (razer_query(h, dev[i].featLen, &r)) {
            g_h = h;
            g_kind = K_RAZER;
            lstrcpyW(g_name, L"雷蛇 Razer");
            wsprintfW(g_detail, L"90 字节特性报告 · 07/80");
            g_outLen = dev[i].outLen; g_inLen = dev[i].inLen; g_featLen = dev[i].featLen;
            logf(L"match RAZER");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 4) ASUS ROG */
    for (i = 0; i < n; i++) {
        if (dev[i].vid != 0x0B05 || !is_vendor_page(dev[i].usagePage))
            continue;
        if (dev[i].outLen < 64 || dev[i].inLen < 64)
            continue;
        HANDLE h = dev_open_ex(dev[i].path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        ZeroMemory(&r, sizeof(r));
        if (rog_query(h, dev[i].outLen, dev[i].inLen, &r)) {
            g_h = h;
            g_kind = K_ROG;
            lstrcpyW(g_name, L"华硕 ROG");
            wsprintfW(g_detail, L"CC 12 07 · 64 字节");
            g_outLen = dev[i].outLen; g_inLen = dev[i].inLen; g_featLen = dev[i].featLen;
            logf(L"match ROG");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 5) LEOBOG 特例：与 Compx 共用 VID，必须抢在通用 Compx 之前判 */
    for (i = 0; i < n; i++) {
        if (dev[i].vid != 0x3554 || !is_vendor_page(dev[i].usagePage))
            continue;
        if (dev[i].pid != 0xFA09 && dev[i].pid != 0xFA0A)
            continue;
        HANDLE h = dev_open_ex(dev[i].path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        ZeroMemory(&r, sizeof(r));
        if (leobog_query(h, dev[i].outLen, dev[i].inLen, &r)) {
            g_h = h;
            g_kind = K_LEOBOG;
            lstrcpyW(g_name, brand_name(dev[i].vid, dev[i].pid));
            wsprintfW(g_detail, L"LEOBOG 专用 · 13 CA · 20 字节");
            g_outLen = dev[i].outLen; g_inLen = dev[i].inLen; g_featLen = dev[i].featLen;
            logf(L"match LEOBOG");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 6) Compx 短报告：ATK / VXE / VGN / 达尔优 / G-Wolves / 兰族 / Pulsar 等 */
    for (i = 0; i < n; i++) {
        if (!is_vendor_page(dev[i].usagePage))
            continue;
        if (dev[i].outLen < 12 || dev[i].outLen > 32)
            continue;
        HANDLE h = dev_open_ex(dev[i].path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        ZeroMemory(&r, sizeof(r));
        if (compx17_query(h, dev[i].outLen, dev[i].inLen, &r)) {
            g_h = h;
            g_kind = K_COMPX17;
            lstrcpyW(g_name, brand_name(dev[i].vid, dev[i].pid));
            wsprintfW(g_detail, L"Compx Nordic52 · %u 字节 08-04", dev[i].outLen);
            g_outLen = dev[i].outLen; g_inLen = dev[i].inLen; g_featLen = dev[i].featLen;
            logf(L"match COMPX17");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 7) Compx 长报告（Nordic54，ATK Zero / 部分 VXE 等） */
    for (i = 0; i < n; i++) {
        if (!is_vendor_page(dev[i].usagePage))
            continue;
        if (dev[i].outLen < 64 || dev[i].outLen > 80)
            continue;
        HANDLE h = dev_open_ex(dev[i].path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        ZeroMemory(&r, sizeof(r));
        if (compx64_query(h, dev[i].outLen, dev[i].inLen, &r)) {
            g_h = h;
            g_kind = K_COMPX64;
            lstrcpyW(g_name, brand_name(dev[i].vid, dev[i].pid));
            wsprintfW(g_detail, L"Compx Nordic54 · %u 字节 08-%02X",
                      dev[i].outLen, dev[i].outLen);
            g_outLen = dev[i].outLen; g_inLen = dev[i].inLen; g_featLen = dev[i].featLen;
            logf(L"match COMPX64");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 8) Realtek 0xFF01（迈从 MCHOSE 等） */
    for (i = 0; i < n; i++) {
        if (dev[i].usagePage != 0xFF01)
            continue;
        if (dev[i].outLen < 8 || dev[i].inLen < 8)
            continue;
        HANDLE h = dev_open_ex(dev[i].path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        ZeroMemory(&r, sizeof(r));
        if (realtek_query(h, dev[i].outLen, dev[i].inLen, &r)) {
            g_h = h;
            g_kind = K_REALTEK;
            lstrcpyW(g_name, brand_name(dev[i].vid, dev[i].pid));
            wsprintfW(g_detail, L"Realtek · FF01/%04X · 11-13 · E2 事件",
                      dev[i].usage);
            g_outLen = dev[i].outLen; g_inLen = dev[i].inLen; g_featLen = dev[i].featLen;
            logf(L"match REALTEK");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 9) 飞智 Flydigi（37D7，FFA0，00 5A A5） */
    for (i = 0; i < n; i++) {
        if (dev[i].vid != 0x37D7 || !is_vendor_page(dev[i].usagePage))
            continue;
        if (dev[i].outLen < 33 || dev[i].inLen < 33)
            continue;
        HANDLE h = dev_open_ex(dev[i].path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        ZeroMemory(&r, sizeof(r));
        if (flydigi_query(h, dev[i].outLen, dev[i].inLen, &r)) {
            g_h = h;
            g_kind = K_FLYDIGI;
            lstrcpyW(g_name, brand_name(dev[i].vid, dev[i].pid));
            wsprintfW(g_detail, L"Flydigi · 00 5A A5 01 02 03 · 33 字节");
            g_outLen = dev[i].outLen; g_inLen = dev[i].inLen; g_featLen = dev[i].featLen;
            logf(L"match FLYDIGI");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 10) 通用兜底：Windows 电池类设备（蓝牙鼠标通用） */
    if (winbat_open()) {
        ZeroMemory(&r, sizeof(r));
        if (winbat_query(&r)) {
            g_kind = K_WINBAT;
            lstrcpyW(g_name, L"蓝牙设备（系统电池）");
            lstrcpyW(g_detail, L"Windows 电池类设备");
            logf(L"match WINBAT");
            return TRUE;
        }
        CloseHandle(g_bat);
        g_bat = NULL;
    }

    logf(L"no provider matched");
    return FALSE;
}

/* ---------------- 对外接口 ---------------- */

BOOL prov_passive(void) { return g_kind == K_RAPOO; }
HANDLE prov_event(void) { return g_ev; }

void prov_arm(void)
{
    if (!g_h || g_kind != K_RAPOO)
        return;
    ZeroMemory(&g_ov, sizeof(g_ov));
    g_ov.hEvent = g_ev;
    ResetEvent(g_ev);
    g_pending = TRUE;
    if (!ReadFile(g_h, g_buf, sizeof(g_buf), NULL, &g_ov)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            g_pending = FALSE;
            prov_close();
        }
    }
}

BOOL prov_read_ready(READING *r)
{
    r->valid = FALSE;
    r->level = -1;
    r->charging = -1;

    if (!g_h || !g_pending)
        return FALSE;

    DWORD got = 0;
    if (!GetOverlappedResult(g_h, &g_ov, &got, FALSE)) {
        prov_close();
        return FALSE;
    }
    g_pending = FALSE;

    if (g_kind == K_RAPOO) {
        if (got >= 2 && g_buf[0] == 0xBB) {
            int lv = g_buf[got - 1];
            if (lv >= 0 && lv <= 100) {
                r->level = lv;
                r->charging = -1;
                r->valid = TRUE;
                g_lastLevel = lv;

                g_lastRawLen = (int)got;
                if (g_lastRawLen > (int)sizeof(g_lastRaw))
                    g_lastRawLen = (int)sizeof(g_lastRaw);
                CopyMemory(g_lastRaw, g_buf, (SIZE_T)g_lastRawLen);
            }
        }
    }
    return r->valid;
}

int prov_last_raw(BYTE *out, int cap)
{
    int n = g_lastRawLen;
    if (n > cap)
        n = cap;
    if (n <= 0)
        return 0;
    CopyMemory(out, g_lastRaw, (SIZE_T)n);
    return n;
}

BOOL prov_poll(READING *r)
{
    r->valid = FALSE;
    r->level = -1;
    r->charging = -1;

    BOOL ok;
    switch (g_kind) {
    case K_COMPX17:
        ok = g_h ? compx17_query(g_h, g_outLen, g_inLen, r) : FALSE;
        break;
    case K_COMPX64:
        ok = g_h ? compx64_query(g_h, g_outLen, g_inLen, r) : FALSE;
        break;
    case K_LEOBOG:
        ok = g_h ? leobog_query(g_h, g_outLen, g_inLen, r) : FALSE;
        break;
    case K_REALTEK:
        ok = g_h ? realtek_query(g_h, g_outLen, g_inLen, r) : FALSE;
        break;
    case K_FLYDIGI:
        ok = g_h ? flydigi_query(g_h, g_outLen, g_inLen, r) : FALSE;
        break;
    case K_HIDPP2:
        if (!g_h) {
            ok = FALSE;
            break;
        }
        if (g_ppBat04 || g_ppBat00) {
            BYTE rx[32];
            BYTE feat = g_ppBat04 ? g_ppBat04 : g_ppBat00;
            ok = FALSE;
            if (hidpp_call(g_h, g_ppRid, g_outLen, g_inLen, g_ppDev, feat, 0x00, NULL, 0, rx)) {
                int lv = rx[4];
                if (lv >= 0 && lv <= 100) {
                    r->level = lv;
                    r->charging = g_ppBat04 ? (rx[6] != 0 ? 1 : 0)
                                            : ((rx[6] >= 1 && rx[6] <= 4) ? 1 : 0);
                    r->valid = TRUE;
                    ok = TRUE;
                }
            }
            break;
        }
        ok = hidpp_probe(g_h, g_outLen, g_inLen, r, TRUE);
        break;
    case K_RAZER:
        ok = g_h ? razer_query(g_h, g_featLen, r) : FALSE;
        break;
    case K_ROG:
        ok = g_h ? rog_query(g_h, g_outLen, g_inLen, r) : FALSE;
        break;
    case K_WINBAT:
        ok = winbat_query(r);
        break;
    default:
        ok = FALSE;
        break;
    }

    if (ok && r->valid)
        g_lastLevel = r->level;
    return ok;
}

const WCHAR *prov_name(void)   { return g_name; }
const WCHAR *prov_detail(void) { return g_detail; }

BOOL prov_diag(const WCHAR *path)
{
    /* 整段先拼成宽字符，再一次性转 UTF-8：方便直接粘贴到 issue */
    WCHAR *txt = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 32768 * sizeof(WCHAR));
    if (!txt)
        return FALSE;

    int len = 0;
    len += wsprintfW(txt + len,
                     L"BattTray diagnostics\r\n"
                     L"version : %s\r\n"
                     L"provider: %s\r\n"
                     L"protocol: %s\r\n"
                     L"reading : %d\r\n"
                     L"reports : in=%d out=%d feature=%d\r\n\r\n",
                     BATT_VERSION,
                     g_name[0] ? g_name : L"(none)",
                     g_detail[0] ? g_detail : L"(unknown)",
                     g_lastLevel, g_inLen, g_outLen, g_featLen);

    if (g_logLen > 0 && len + g_logLen < 32000) {
        CopyMemory(txt + len, g_log, (SIZE_T)g_logLen * sizeof(WCHAR));
        len += g_logLen;
    }

    int need = WideCharToMultiByte(CP_UTF8, 0, txt, len, NULL, 0, NULL, NULL);
    if (need <= 0) {
        HeapFree(GetProcessHeap(), 0, txt);
        return FALSE;
    }

    BYTE *out = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)need + 3);
    if (!out) {
        HeapFree(GetProcessHeap(), 0, txt);
        return FALSE;
    }
    out[0] = 0xEF; out[1] = 0xBB; out[2] = 0xBF;   /* UTF-8 BOM */
    WideCharToMultiByte(CP_UTF8, 0, txt, len, (char *)out + 3, need, NULL, NULL);
    HeapFree(GetProcessHeap(), 0, txt);

    BOOL ok = FALSE;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, out, (DWORD)need + 3, &w, NULL);
        ok = TRUE;
        CloseHandle(f);
    }
    HeapFree(GetProcessHeap(), 0, out);
    return ok;
}
