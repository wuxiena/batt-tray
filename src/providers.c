// providers.c - 按「协议族」实现的无线外设电量读取（鼠标 / 键盘 / 耳机 / 手柄）
//
// 设计说明：
//   * 协议族（而非品牌）驱动，新品牌只要落在同一族就自动覆盖；
//   * 支持多设备并存：每个物理设备（按 VID/PID 分组）只占一个槽位，
//     同一接收器上的多个 HID 集合会被合并成一个设备；
//   * 被动型（雷柏、索尼手柄）：靠设备主动上报，零轮询；
//   * 查询型（罗技/雷蛇/Compx/ROG/Realtek/飞智）：按间隔发送「只读」查询；
//   * 手柄补充：索尼走 HID 输入报文，Xbox / XInput 兼容手柄走 XInput；
//   * 全部失败时回退到 Windows 电池类设备（蓝牙耳机/鼠标通用，可多个）。
//
// 协议来源（均为公开资料，详见 README 的「参考与致谢」）：
//   雷柏  : JD13703202/rapoo-vt3-battery  —— 实测本机设备
//   Compx : Fan4Metal/ATK_tray、libatk-rs —— ATK/VXE/VGN/达尔优/G-Wolves
//   罗技  : Logitech HID++ 2.0 草案规范、GearPower —— 0x1004/0x1000（鼠标与耳机通用）
//   雷蛇  : OpenRazer、GearPower 实测报文       —— 90 字节特性报告 07/80
//   ROG   : GearPower 实测报文                  —— CC 12 07
//   索尼  : hid-playstation 的报文布局说明      —— DualShock 4 / DualSense
//   Xbox  : XInputGetBatteryInformation（系统 API）

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

#define MAX_COLL   128
#define PATH_CAP   512

typedef struct {
    WCHAR  path[PATH_CAP];
    USHORT vid, pid;
    USHORT usagePage, usage;
    USHORT inLen, outLen, featLen;
    BYTE   mask;                    /* 用途位掩码，见 TM_* */
} COLL;

/* 用途位：同一物理设备的多个 HID 集合合并后据此判断类别 */
#define TM_MOUSE   0x01
#define TM_KEY     0x02
#define TM_PAD     0x04
#define TM_AUDIO   0x08

enum {
    K_NONE = 0,
    K_RAPOO,        /* 雷柏 2.4G 被动上报 */
    K_SONY,         /* DualShock 4 / DualSense 输入报文 */
    K_HIDPP2,       /* 罗技 HID++ 2.0（鼠标/键盘/耳机） */
    K_RAZER,        /* 雷蛇 90 字节特性报告 */
    K_ROG,          /* 华硕 ROG */
    K_LEOBOG,
    K_REALTEK,      /* Realtek FF01（迈从等） */
    K_FLYDIGI,
    K_COMPX17,
    K_COMPX64,
    K_XINPUT,       /* Xbox / XInput 兼容手柄 */
    K_WINBAT,       /* Windows 电池类设备 */
    K_VEND90,       /* 0xFF90 厂商通道（耳机 dongle，MCHOSE V9 Pro 同族） */
    K_GAMESIR,      /* 小鸡 GameSir 0x3537 厂商接口（0x12 报文） */
    K_FAKE          /* 仅 -DBATT_FAKE 调试构建使用，正式版不会出现 */
};

typedef struct {
    int    kind;
    HANDLE h;                       /* HID 句柄 */
    HANDLE bat;                     /* 电池类设备句柄（K_WINBAT 用） */
    USHORT vid, pid;

    WCHAR  brand[72];
    WCHAR  type[16];
    WCHAR  detail[144];

    int    outLen, inLen, featLen;
    BYTE   ppRid, ppDev, ppBat04, ppBat00;   /* HID++ 探测结果 */

    /* 被动监听 */
    OVERLAPPED ov;
    HANDLE ev;
    BOOL   pending;
    BYTE   buf[512];
    BYTE   lastRaw[24];
    int    lastRawLen;

    DWORD  xiIndex;                 /* XInput 用户索引 */
    int    sonyKind;                /* 1 = DualShock 4，2 = DualSense */
    int    lastLevel;
} DEVENT;

static DEVENT g_dev[PROV_MAX];
static int    g_ndev;

/* 0xFF90 通道连续不应答计数：不应答的 dongle 不该每轮都去敲一次 */
static int    g_v90fail[PROV_MAX];

/* 小鸡手柄心跳后仍读不到 0x12 报文的计数（同上，退避用） */
static int    g_gsrFail[PROV_MAX];

/* 主动查询间隔（毫秒）。协议层自己不用它，只写进诊断文件，
   方便判断「图标多久没变」是设备睡了还是间隔设得太长。 */
static int    g_pollMsHint;

void prov_set_poll_ms(int ms) { g_pollMsHint = ms; }

/* 诊断日志（跨设备共用一份，按扫描顺序追加） */
static WCHAR g_log[16384];
static int   g_logLen;

/* XInput 动态加载 */
typedef struct { BYTE type; BYTE level; } XIBAT;
typedef DWORD (WINAPI *XI_GETBAT)(DWORD, BYTE, XIBAT *);
typedef DWORD (WINAPI *XI_GETSTATE)(DWORD, void *);
static XI_GETBAT   g_xiBat;
static XI_GETSTATE g_xiState;
static BOOL        g_xiTried;

static void logf(const WCHAR *fmt, ...)
{
    if (g_logLen > 15000)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfW(g_log + g_logLen, fmt, ap);
    va_end(ap);
    g_logLen += n;
    if (g_logLen < 16380) {
        g_log[g_logLen++] = L'\r';
        g_log[g_logLen++] = L'\n';
        g_log[g_logLen] = 0;
    }
}

/* ---------------- 宽字符小工具（不依赖 CRT） ---------------- */

static WCHAR w_upper(WCHAR c)
{
    if (c >= L'a' && c <= L'z')
        return (WCHAR)(c - L'a' + L'A');
    return c;
}

/* 大小写不敏感的子串查找（只处理 ASCII，够用） */
static BOOL w_contains(const WCHAR *hay, const WCHAR *needle)
{
    if (!hay || !needle || !needle[0])
        return FALSE;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && w_upper(hay[i + j]) == w_upper(needle[j]))
            j++;
        if (!needle[j])
            return TRUE;
    }
    return FALSE;
}

static int hex4(const WCHAR *s)
{
    int v = 0;
    for (int i = 0; i < 4 && s[i]; i++) {
        WCHAR c = w_upper(s[i]);
        int d;
        if (c >= L'0' && c <= L'9')      d = c - L'0';
        else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
        else break;
        v = (v << 4) | d;
    }
    return v;
}

/* 从设备实例 ID 里解析 VID_xxxx / PID_xxxx */
static BOOL parse_vidpid(const WCHAR *s, USHORT *vid, USHORT *pid)
{
    BOOL ok = FALSE;
    for (int i = 0; s[i]; i++) {
        if (w_upper(s[i]) == L'V' && w_upper(s[i + 1]) == L'I' &&
            w_upper(s[i + 2]) == L'D' && s[i + 3] == L'_') {
            *vid = (USHORT)hex4(s + i + 4);
            ok = TRUE;
        } else if (w_upper(s[i]) == L'P' && w_upper(s[i + 1]) == L'I' &&
                   w_upper(s[i + 2]) == L'D' && s[i + 3] == L'_') {
            *pid = (USHORT)hex4(s + i + 4);
            ok = TRUE;
        }
    }
    return ok;
}

/* ---------------- 品牌识别 ---------------- */
/* 协议族负责「怎么读」，这张表只负责「叫对名字」。 */

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
    { 0x054C, L"索尼 PlayStation" },
    { 0x045E, L"微软 Xbox" },
    { 0x057E, L"任天堂 Nintendo" },
    { 0x2DC8, L"八位堂 8BitDo" },
    { 0x2563, L"北通 BETOP" },
    { 0x2F24, L"小鸡 GameSir" },      /* 小鸡的 XInput / GIP 身份（USB-IF 登记在深圳汇佳智） */
    { 0x3537, L"小鸡 GameSir" },      /* 小鸡的厂商接口身份，电量走这里 */
    { 0x28DE, L"Valve" },
    { 0x0F0D, L"鸿绪 HORI" },
    { 0x1395, L"森海塞尔 Sennheiser" },
    { 0x0ECB, L"JBL" },
    { 0x047F, L"Plantronics" },
    { 0x1235, L"Focusrite" },
    { 0x0A12, L"Cambridge Audio" },
    { 0x10D6, L"Actions 方案" },      /* 耳机 dongle 芯片厂，真实品牌看产品名 */
    { 0x291D, L"C-Media 方案" },
    { 0x041E, L"创新 Creative" },
};

/* 耳机 dongle 多数用芯片厂的 VID，品牌只能从产品字符串认 */
typedef struct { const WCHAR *key; const WCHAR *name; } PRODBRAND;

static const PRODBRAND kProdBrands[] = {
    { L"GAMESIR",       L"小鸡 GameSir" },
    { L"CYCLONE",       L"小鸡 GameSir" },
    { L"XIBERIA",       L"西伯利亚 XIBERIA" },
    { L"EDIFIER",       L"漫步者 Edifier" },
    { L"MCHOSE",        L"迈从 MCHOSE" },
    { L"HYPERX",        L"HyperX" },
    { L"KINGSTON",      L"HyperX" },
    { L"SOMIC",         L"硕美科 Somic" },
    { L"ONIKUMA",       L"ONIKUMA" },
    { L"KOTION",        L"KOTION EACH" },
    { L"LANGTU",        L"狼途 Langtu" },
    { L"AJAZZ",         L"黑爵 AJAZZ" },
    { L"REDRAGON",      L"红龙 Redragon" },
    { L"STEELSERIES",   L"赛睿 SteelSeries" },
    { L"ARCTIS",        L"赛睿 SteelSeries" },
    { L"LOGITECH",      L"罗技 Logitech" },
    { L"G PRO",         L"罗技 Logitech" },
    { L"RAZER",         L"雷蛇 Razer" },
    { L"BLACKSHARK",    L"雷蛇 Razer" },
    { L"CORSAIR",       L"海盗船 Corsair" },
    { L"ROCCAT",        L"冰豹 ROCCAT" },
    { L"TURTLE BEACH",  L"乌龟海岸 Turtle Beach" },
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
    return NULL;
}

/* 读 HID 产品名（零权限句柄即可）。返回 FALSE 表示拿不到。 */
static BOOL hid_product_string(const WCHAR *path, WCHAR *out, int cap)
{
    if (cap <= 0)
        return FALSE;
    out[0] = 0;
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    WCHAR buf[256];
    BOOL ok = HidD_GetProductString(h, buf, sizeof(buf));
    CloseHandle(h);
    if (!ok || !buf[0])
        return FALSE;
    lstrcpynW(out, buf, cap);
    return TRUE;
}

/* 产品名 → 品牌名：命中关键词表返回 TRUE；认不出来则原样返回 FALSE */
static BOOL brand_from_product(const WCHAR *prod, WCHAR *out, int cap)
{
    if (!out || cap <= 0)
        return FALSE;
    out[0] = 0;
    if (!prod || !prod[0])
        return FALSE;
    for (int i = 0; i < (int)(sizeof(kProdBrands) / sizeof(kProdBrands[0])); i++) {
        if (w_contains(prod, kProdBrands[i].key)) {
            lstrcpynW(out, kProdBrands[i].name, cap);
            return TRUE;
        }
    }
    return FALSE;
}

/* 芯片厂 VID（Actions / C-Media 之类）只说明方案，不代表品牌。
   顺序：产品名关键词 → 非「XX 方案」的 VID 品牌 → 产品名整串 → VID 表 */
static const WCHAR *brand_for_vendor_dongle(const COLL *c, WCHAR *stage, int cap)
{
    WCHAR prod[72];
    prod[0] = 0;
    hid_product_string(c->path, prod, sizeof(prod) / sizeof(prod[0]));

    if (prod[0] && brand_from_product(prod, stage, cap))
        return stage;

    const WCHAR *vb = brand_name(c->vid, c->pid);
    if (vb && vb[0] && !w_contains(vb, L"方案"))
        return vb;

    if (prod[0]) {
        lstrcpynW(stage, prod, cap);
        return stage;
    }
    return vb;
}

/* ---------------- 设备枚举 ---------------- */

static int coll_enum(COLL *out, int max)
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

        COLL *c = &out[n];
        ZeroMemory(c, sizeof(*c));
        lstrcpynW(c->path, pd->DevicePath, PATH_CAP);
        if (HidD_GetAttributes(h, &at)) {
            c->vid = at.VendorID;
            c->pid = at.ProductID;
        }
        if (HidD_GetPreparsedData(h, &pp)) {
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                c->usagePage = caps.UsagePage;
                c->usage     = caps.Usage;
                c->inLen     = caps.InputReportByteLength;
                c->outLen    = caps.OutputReportByteLength;
                c->featLen   = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);

        if (!c->usagePage)
            continue;

        if (c->usagePage == 0x01) {
            if (c->usage == 0x02) c->mask |= TM_MOUSE;
            else if (c->usage == 0x06) c->mask |= TM_KEY;
            else if (c->usage == 0x04 || c->usage == 0x05) c->mask |= TM_PAD;
        } else if (c->usagePage == 0x0B || c->usagePage == 0x0C) {
            c->mask |= TM_AUDIO;         /* 音频控制 / 多媒体键：耳机 */
        }

        logf(L"HID vid=%04X pid=%04X page=%04X usage=%04X in=%u out=%u feat=%u",
             c->vid, c->pid, c->usagePage, c->usage, c->inLen, c->outLen, c->featLen);
        n++;
    }
    SetupDiDestroyDeviceInfoList(hdi);
    return n;
}

/* 同组判定：优先按 VID/PID；无 VID 的通用设备按实例前缀，避免合并不同设备 */
static void group_key(const COLL *c, WCHAR *out)
{
    if (c->vid == 0 || c->vid == 0xFFFF) {
        lstrcpynW(out, c->path, PATH_CAP);
        for (int i = 0; out[i]; i++) {
            if (out[i] == L'#') {
                out[i] = 0;
                break;
            }
        }
    } else {
        wsprintfW(out, L"%04X:%04X", c->vid, c->pid);
    }
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

/* ---------------- 协议族：罗技 HID++ 2.0（鼠标 / 键盘 / 耳机通用） ---------------- */

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

/* 探测：返回 TRUE 表示找到电量特性；结果写入 d 的 pp* 字段。
   doQuery = FALSE 时只探测不读电量（用于识别阶段）。 */
static BOOL hidpp_probe(HANDLE h, int outLen, int inLen, DEVENT *d, READING *r, BOOL doQuery)
{
    BYTE rid = (outLen >= 20) ? 0x11 : 0x10;

    static const BYTE devs[] = { 0xFF, 1, 2, 3, 4, 5, 6 };
    for (int di = 0; di < (int)sizeof(devs); di++) {
        BYTE dev = devs[di];
        BYTE f04 = hidpp_root_get_feature(h, rid, outLen, inLen, dev, 0x1004);
        BYTE f00 = f04 ? 0 : hidpp_root_get_feature(h, rid, outLen, inLen, dev, 0x1000);
        if (!f04 && !f00)
            continue;

        d->ppRid = rid;
        d->ppDev = dev;
        d->ppBat04 = f04;
        d->ppBat00 = f00;
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

static BOOL hidpp_read(DEVENT *d, READING *r)
{
    if (!d->h)
        return FALSE;
    if (!d->ppBat04 && !d->ppBat00)
        return hidpp_probe(d->h, d->outLen, d->inLen, d, r, TRUE);

    BYTE rx[32];
    BYTE feat = d->ppBat04 ? d->ppBat04 : d->ppBat00;
    if (!hidpp_call(d->h, d->ppRid, d->outLen, d->inLen, d->ppDev, feat, 0x00, NULL, 0, rx))
        return FALSE;
    int lv = rx[4];
    if (lv < 0 || lv > 100)
        return FALSE;
    r->level = lv;
    r->charging = d->ppBat04 ? (rx[6] != 0 ? 1 : 0)
                             : ((rx[6] >= 1 && rx[6] <= 4) ? 1 : 0);
    r->valid = TRUE;
    return TRUE;
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

/* ---------------- 协议族：0xFF90 厂商通道（耳机 dongle） ---------------- */
//
// 大量国产 2.4G 耳机接收器（Actions、C-Media 等芯片方案）都会额外挂一个
// Usage Page 0xFF90 / Usage 0x0001、IN/OUT 各 64 字节的厂商集合，用来
// 转发配置与状态。已知可用的读法是：
//     发送  55 65 01 00 ...（余下补 0）
//     回复  55 65 <电量 0..100> <状态字节> ...（状态含义未公开，忽略）
// 该帧头在 MCHOSE V9 Pro 上有公开逆向记录；同族 dongle 若不认这个帧，
// 会直接拒绝写入或干脆不回，两种都算「不支持」，不影响其它协议族。
// 注意：不要把这里改成「盲扫 opcode」——某些 dongle 收到非法帧会自行重连。

static BOOL vend90_query_co(HANDLE h, int outLen, int inLen, READING *r)
{
    BYTE tx[64];
    int  n = (outLen >= 8 && outLen <= (int)sizeof(tx)) ? outLen : 64;

    ZeroMemory(tx, sizeof(tx));
    tx[0] = 0x55;
    tx[1] = 0x65;
    tx[2] = 0x01;                 /* 子命令：读电量 */
    if (!io_write(h, tx, (DWORD)n, 300))
        return FALSE;

    int rd = (inLen >= 4 && inLen <= 64) ? inLen : 64;
    for (int tries = 0; tries < 3; tries++) {
        BYTE  rx[64];
        DWORD got = 0;
        ZeroMemory(rx, sizeof(rx));
        if (!io_read(h, rx, (DWORD)rd, &got, 250))
            return FALSE;         /* 超时：这个 dongle 不说话 */
        if (got < 4 || rx[0] != 0x55 || rx[1] != 0x65)
            continue;             /* 是别的报文，继续等 */
        int lv = rx[2];
        if (lv < 0 || lv > 100)
            continue;
        r->level = lv;
        r->charging = -1;
        r->valid = TRUE;
        return TRUE;
    }
    return FALSE;
}

/* ---------------- 协议族：小鸡 GameSir（0x3537 厂商接口） ---------------- */
//
// 小鸡手柄在 PC 上有两套身份：
//     0x2F24 —— XInput / GIP 身份，Windows 里就是一支普通 Xbox 手柄；
//     0x3537 —— 厂商接口，电量只走这里。
// 厂商接口的读法来自开源逆向项目 broroeror/gamesir-linux-tools（Cyclone 2 /
// G7 Pro / G7 SE / Tarantula 实测）：先发一条 0x0F 报文做心跳，手柄才会持续
// 推送 0x12 状态报文；报文第 36 字节就是电量百分比，第 35 字节 bit0 是充电。
// 心跳不发，通道完全是哑的——这一点和「先听再说」的耳机 dongle 不同。
//
// ⚠️ 本分支尚未在实机上验证（写这段时手上没有小鸡手柄）。解析只认 0x12 报文
// 头且电量限定 0..100，认不出来就当作「不可读」，不会编数字。

static BOOL gamesir_read_co(HANDLE h, int outLen, int inLen, READING *r)
{
    BYTE tx[64];
    int  n = (outLen >= 4 && outLen <= (int)sizeof(tx)) ? outLen : 64;

    ZeroMemory(tx, sizeof(tx));
    tx[0] = 0x0F;                 /* 输出报文 id */
    tx[1] = 0xF2;                 /* 心跳 */
    if (!io_write(h, tx, (DWORD)n, 200))
        return FALSE;

    int rd = (inLen >= 37 && inLen <= (int)sizeof(tx)) ? inLen : 64;
    for (int tries = 0; tries < 3; tries++) {
        BYTE  rx[64];
        DWORD got = 0;
        ZeroMemory(rx, sizeof(rx));
        if (!io_read(h, rx, (DWORD)rd, &got, 90))
            return FALSE;         /* 超时：报文没来（手柄没连 / 不在 Xbox 模式） */
        if (got < 37 || rx[0] != 0x12)
            continue;             /* 别的报文，继续等 */
        int lv = rx[36];
        if (lv < 0 || lv > 100)
            continue;
        r->level = lv;
        r->charging = (rx[35] & 0x01) ? 1 : 0;
        r->valid = TRUE;
        return TRUE;
    }
    return FALSE;
}

/* ---------------- 协议族：索尼手柄（DualShock 4 / DualSense） ---------------- */
//
// 手柄按固定频率推送输入报文，因此走被动路径：读完再挂起下一次读。
// 电量字段：DS4 在 0x1E（高半字节 0..10＝档位，低半字节＝是否充电）；
//           DualSense 在 0x36（低半字节 0..10＝档位，高半字节＝充电状态）。
// 两个偏移附近都接受，并做取值校验，避免固件差异导致读数乱跳。

static BOOL sony_parse(DEVENT *d, const BYTE *buf, DWORD got, READING *r)
{
    if (got < 32)
        return FALSE;

    BYTE rid = buf[0];

    if (d->sonyKind == 2) {                       /* DualSense */
        if (rid != 0x01 && rid != 0x31)
            return FALSE;

        /* 先试 0x36，不行再试 0x35（两种固件都有出现） */
        for (int k = 0; k < 2; k++) {
            int off = k ? 0x35 : 0x36;
            if ((int)got <= off)
                continue;
            int b  = buf[off];
            int cap = b & 0x0F;
            int st  = (b >> 4) & 0x0F;
            if (cap > 10 || st > 2)
                continue;
            r->level = cap * 10;
            r->charging = (st == 1) ? 1 : 0;
            r->valid = TRUE;
            return TRUE;
        }
        return FALSE;
    }

    /* DualShock 4：0x01 报文的电量在 0x1E，0x11 报文在 0x1F */
    if (rid != 0x01 && rid != 0x11)
        return FALSE;
    {
        int off = (rid == 0x11) ? 0x1F : 0x1E;
        if ((int)got <= off)
            return FALSE;
        int b   = buf[off];
        int cap = (b >> 4) & 0x0F;
        int st  = b & 0x0F;
        if (cap > 10 || st > 2)
            return FALSE;
        r->level = cap * 10;
        r->charging = (st == 1) ? 1 : 0;
        r->valid = TRUE;
        return TRUE;
    }
}

/* ---------------- 协议族：Xbox / XInput 兼容手柄 ---------------- */

static BOOL xi_init(void)
{
    if (g_xiTried)
        return g_xiBat != NULL;
    g_xiTried = TRUE;

    static const WCHAR *dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll" };
    for (int i = 0; i < 2 && !g_xiBat; i++) {
        HMODULE m = LoadLibraryW(dlls[i]);
        if (!m)
            continue;
        g_xiBat = (XI_GETBAT)GetProcAddress(m, "XInputGetBatteryInformation");
        if (!g_xiState)
            g_xiState = (XI_GETSTATE)GetProcAddress(m, "XInputGetState");
        if (!g_xiBat) {
            /* 该版本没有电量接口，保持模块已加载无妨，继续下一个 */
        }
    }
    return g_xiBat != NULL;
}

/* 返回 TRUE 并填入 coarse 等级（0..100），仅接受无线连接的手柄 */
static BOOL xi_read(DWORD idx, READING *r)
{
    if (!g_xiBat)
        return FALSE;
    XIBAT b;
    ZeroMemory(&b, sizeof(b));
    if (g_xiBat(idx, 0 /* BATTERY_DEVTYPE_GAMEPAD */, &b) != ERROR_SUCCESS)
        return FALSE;

    /* 0 断开 / 1 有线 / 2 碱性电池 / 3 镍氢电池 / 0xFF 未知 */
    if (b.type == 0 || b.type == 1)
        return FALSE;

    switch (b.level) {
    case 0:  r->level = 5;   break;   /* EMPTY  */
    case 1:  r->level = 25;  break;   /* LOW    */
    case 2:  r->level = 60;  break;   /* MEDIUM */
    case 3:  r->level = 100; break;   /* FULL   */
    default: return FALSE;
    }
    r->charging = 0;
    r->valid = TRUE;
    return TRUE;
}

/* 手柄确实连着、但电量读不出来。
   大量第三方 2.4G dongle 会直接用 XINPUT_BATTERY_INFORMATION 谎报
   type=WIRED / level=FULL（天剑PLUS 实测如此），所以只能靠有无手柄在线来区分，
   不能把 type==WIRED 当作「没插手柄」。 */
static BOOL xi_connected(DWORD idx, BOOL *wired)
{
    if (wired)
        *wired = FALSE;
    if (!g_xiState)
        return FALSE;

    BYTE state[32];               /* XINPUT_STATE 只有 16 字节，留足余量 */
    ZeroMemory(state, sizeof(state));
    if (g_xiState(idx, state) != ERROR_SUCCESS)
        return FALSE;

    if (wired && g_xiBat) {
        XIBAT b;
        ZeroMemory(&b, sizeof(b));
        if (g_xiBat(idx, 0, &b) == ERROR_SUCCESS && b.type == 1)
            *wired = TRUE;
    }
    return TRUE;
}

/* ---------------- 通用回退：Windows 电池类设备（蓝牙耳机 / 鼠标 / 手柄） ---------------- */

static const GUID kBatClass =
    { 0x72631E54, 0x78A4, 0x11D0, { 0xBC, 0xF7, 0x00, 0xAA, 0x00, 0xB7, 0xB3, 0x2A } };

static BOOL winbat_query_handle(HANDLE h, READING *r)
{
    if (!h || h == INVALID_HANDLE_VALUE)
        return FALSE;

    DWORD bytes = 0, wait = 0;
    ULONG tag = 0;
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait),
                         &tag, sizeof(tag), &bytes, NULL) || tag == 0)
        return FALSE;

    BATTERY_QUERY_INFORMATION bqi;
    ZeroMemory(&bqi, sizeof(bqi));
    bqi.BatteryTag = tag;
    bqi.InformationLevel = BatteryInformation;

    BATTERY_INFORMATION bi;
    ZeroMemory(&bi, sizeof(bi));
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &bqi, sizeof(bqi),
                         &bi, sizeof(bi), &bytes, NULL))
        return FALSE;
    if (bi.FullChargedCapacity == 0)
        return FALSE;

    BATTERY_WAIT_STATUS bws;
    ZeroMemory(&bws, sizeof(bws));
    bws.BatteryTag = tag;

    BATTERY_STATUS bs;
    ZeroMemory(&bs, sizeof(bs));
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_STATUS, &bws, sizeof(bws),
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

/* 按设备名猜类别，蓝牙设备拿不到 HID 用途，只能靠名字 */
static const WCHAR *type_from_name(const WCHAR *name)
{
    if (!name || !name[0])
        return L"蓝牙设备";
    if (w_contains(name, L"耳机") || w_contains(name, L"耳機") ||
        w_contains(name, L"headset") || w_contains(name, L"headphone") ||
        w_contains(name, L"earbud") || w_contains(name, L"buds") ||
        w_contains(name, L"airpods") || w_contains(name, L"speaker") ||
        w_contains(name, L"audio") ||
        w_contains(name, L"WH-") || w_contains(name, L"WF-") ||
        w_contains(name, L"CH-") || w_contains(name, L"QC"))
        return L"耳机";
    if (w_contains(name, L"手柄") || w_contains(name, L"controller") ||
        w_contains(name, L"gamepad") || w_contains(name, L"joy"))
        return L"手柄";
    if (w_contains(name, L"鼠标") || w_contains(name, L"mouse"))
        return L"鼠标";
    if (w_contains(name, L"键盘") || w_contains(name, L"keyboard"))
        return L"键盘";
    return L"蓝牙设备";
}

/* ---------------- 组内匹配 ---------------- */

static void fill_common(DEVENT *d, const COLL *c, int kind,
                        const WCHAR *brand, const WCHAR *type, const WCHAR *detail)
{
    d->kind = kind;
    d->vid = c->vid;
    d->pid = c->pid;
    lstrcpynW(d->brand, brand ? brand : L"未知品牌", 72);
    lstrcpynW(d->type, type ? type : L"设备", 16);
    lstrcpynW(d->detail, detail ? detail : L"", 144);
}

static const WCHAR *type_from_mask(BYTE mask)
{
    if (mask & TM_PAD)   return L"手柄";
    if (mask & TM_MOUSE) return L"鼠标";
    if (mask & TM_KEY)   return L"键盘";
    if (mask & TM_AUDIO) return L"耳机";
    return L"设备";
}

/* 在组内尝试各协议族；成功则填好 *d（句柄已打开，由调用方接管）。 */
static BOOL match_group(COLL *col, int *idx, int gn, BYTE mask, DEVENT *d)
{
    ZeroMemory(d, sizeof(*d));
    const WCHAR *type = type_from_mask(mask);

    /* 1) 雷柏：被动上报，无法即时验证，优先锁定。
       同一接收器会暴露多个 FF00 集合（0002 是电量所在的那个），
       所以先挑 usage=0002，找不到再退回其它长度合理的集合。 */
    {
        int best = -1;
        for (int k = 0; k < gn; k++) {
            const COLL *c = &col[idx[k]];
            if (c->vid != 0x24AE || c->usagePage != 0xFF00)
                continue;
            if (c->inLen < 4 || c->inLen > 32)
                continue;
            if (c->usage == 0x0002) {
                best = k;
                break;
            }
            if (best < 0)
                best = k;
        }
        if (best >= 0) {
            const COLL *c = &col[idx[best]];
            HANDLE h = dev_open_ex(c->path, FALSE);
            if (h != INVALID_HANDLE_VALUE) {
                WCHAR det[144];
                wsprintfW(det, L"雷柏被动上报 · FF00/%04X · %u 字节", c->usage, c->inLen);
                fill_common(d, c, K_RAPOO, brand_name(c->vid, c->pid), type, det);
                d->h = h;
                d->outLen = c->outLen;
                d->inLen = c->inLen;
                d->featLen = c->featLen;
                d->ev = CreateEventW(NULL, TRUE, FALSE, NULL);
                logf(L"match RAPOO page=%04X usage=%04X in=%u",
                     c->usagePage, c->usage, c->inLen);
                return TRUE;
            }
        }
    }

    /* 2) 索尼手柄：输入报文被动上报 */
    {
        int best = -1;
        for (int k = 0; k < gn; k++) {
            const COLL *c = &col[idx[k]];
            if (c->vid != 0x054C || c->usagePage != 0x01)
                continue;
            if (c->usage != 0x05 && c->usage != 0x04)
                continue;
            if (c->inLen < 32)
                continue;
            best = k;
            break;
        }
        if (best >= 0) {
            const COLL *c = &col[idx[best]];
            HANDLE h = dev_open_ex(c->path, FALSE);
            if (h != INVALID_HANDLE_VALUE) {
                int ds5 = (c->pid == 0x0CE6 || c->pid == 0x0DF2);
                WCHAR det[144];
                wsprintfW(det, L"%s 输入报文 · 被动上报",
                          ds5 ? L"DualSense" : L"DualShock 4");
                fill_common(d, c, K_SONY, brand_name(c->vid, c->pid), L"手柄", det);
                d->h = h;
                d->outLen = c->outLen;
                d->inLen = c->inLen;
                d->featLen = c->featLen;
                d->sonyKind = ds5 ? 2 : 1;
                d->ev = CreateEventW(NULL, TRUE, FALSE, NULL);
                logf(L"match SONY pid=%04X kind=%d", c->pid, d->sonyKind);
                return TRUE;
            }
        }
    }

    /* 3) 罗技 HID++ 2.0（鼠标 / 键盘 / 耳机） */
    for (int k = 0; k < gn; k++) {
        const COLL *c = &col[idx[k]];
        if (c->vid != 0x046D || c->usagePage != 0xFF00)
            continue;
        if (c->outLen != 7 && c->outLen != 20)
            continue;
        HANDLE h = dev_open_ex(c->path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (hidpp_probe(h, c->outLen, c->inLen, d, &r, FALSE)) {
            WCHAR det[144];
            wsprintfW(det, L"HID++ 2.0 · 特性 %s · dev %02X",
                      d->ppBat04 ? L"0x1004" : L"0x1000", d->ppDev);
            d->kind = K_HIDPP2;
            d->vid = c->vid;
            d->pid = c->pid;
            lstrcpynW(d->brand, brand_name(c->vid, c->pid), 72);
            lstrcpynW(d->type, type, 16);
            lstrcpynW(d->detail, det, 144);
            d->h = h;
            d->outLen = c->outLen;
            d->inLen = c->inLen;
            d->featLen = c->featLen;
            logf(L"match HIDPP2");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 4) 雷蛇：90 字节特性报告，鼠标集合常被独占，需 0 权限打开 */
    for (int k = 0; k < gn; k++) {
        const COLL *c = &col[idx[k]];
        if (c->vid != 0x1532 || c->featLen < 90)
            continue;
        HANDLE h = dev_open_ex(c->path, TRUE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (razer_query(h, c->featLen, &r)) {
            fill_common(d, c, K_RAZER, brand_name(c->vid, c->pid), type,
                        L"雷蛇 · 90 字节特性报告 07/80");
            d->h = h;
            d->outLen = c->outLen;
            d->inLen = c->inLen;
            d->featLen = c->featLen;
            logf(L"match RAZER");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 5) ASUS ROG */
    for (int k = 0; k < gn; k++) {
        const COLL *c = &col[idx[k]];
        if (c->vid != 0x0B05 || !is_vendor_page(c->usagePage))
            continue;
        if (c->outLen < 64 || c->inLen < 64)
            continue;
        HANDLE h = dev_open_ex(c->path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (rog_query(h, c->outLen, c->inLen, &r)) {
            fill_common(d, c, K_ROG, brand_name(c->vid, c->pid), type,
                        L"ROG · CC 12 07 · 64 字节");
            d->h = h;
            d->outLen = c->outLen;
            d->inLen = c->inLen;
            d->featLen = c->featLen;
            logf(L"match ROG");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 6) LEOBOG 特例：与 Compx 共用 VID，必须抢在通用 Compx 之前判 */
    for (int k = 0; k < gn; k++) {
        const COLL *c = &col[idx[k]];
        if (c->vid != 0x3554 || !is_vendor_page(c->usagePage))
            continue;
        if (c->pid != 0xFA09 && c->pid != 0xFA0A)
            continue;
        HANDLE h = dev_open_ex(c->path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (leobog_query(h, c->outLen, c->inLen, &r)) {
            fill_common(d, c, K_LEOBOG, brand_name(c->vid, c->pid), type,
                        L"LEOBOG 专用 · 13 CA · 20 字节");
            d->h = h;
            d->outLen = c->outLen;
            d->inLen = c->inLen;
            d->featLen = c->featLen;
            logf(L"match LEOBOG");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 7) Realtek FF01（迈从 MCHOSE 等） */
    for (int k = 0; k < gn; k++) {
        const COLL *c = &col[idx[k]];
        if (c->usagePage != 0xFF01)
            continue;
        if (c->outLen < 8 || c->inLen < 8)
            continue;
        HANDLE h = dev_open_ex(c->path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (realtek_query(h, c->outLen, c->inLen, &r)) {
            WCHAR det[144];
            wsprintfW(det, L"Realtek · FF01/%04X · 11-13 · E2 事件", c->usage);
            fill_common(d, c, K_REALTEK, brand_name(c->vid, c->pid), type, det);
            d->h = h;
            d->outLen = c->outLen;
            d->inLen = c->inLen;
            d->featLen = c->featLen;
            logf(L"match REALTEK");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 8) 飞智 Flydigi（37D7，FFA0，00 5A A5） */
    for (int k = 0; k < gn; k++) {
        const COLL *c = &col[idx[k]];
        if (c->vid != 0x37D7 || !is_vendor_page(c->usagePage))
            continue;
        if (c->outLen < 33 || c->inLen < 33)
            continue;
        HANDLE h = dev_open_ex(c->path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (flydigi_query(h, c->outLen, c->inLen, &r)) {
            fill_common(d, c, K_FLYDIGI, brand_name(c->vid, c->pid), type,
                        L"Flydigi · 00 5A A5 01 02 03 · 33 字节");
            d->h = h;
            d->outLen = c->outLen;
            d->inLen = c->inLen;
            d->featLen = c->featLen;
            logf(L"match FLYDIGI");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 8.2) 小鸡 GameSir：0x3537 厂商接口。手柄在 Xbox 模式下才推 0x12 报文，
       没连手柄的 dongle 也会枚举但不说话——两种情况都登记，菜单里能看出区别。 */
    {
        int cand = -1;
        for (int k = 0; k < gn; k++) {
            const COLL *c = &col[idx[k]];
            if (c->vid != 0x3537 || !is_vendor_page(c->usagePage))
                continue;
            if (c->inLen < 37 || c->outLen < 4)
                continue;
            if (cand < 0)
                cand = k;

            HANDLE h = dev_open_ex(c->path, FALSE);
            if (h == INVALID_HANDLE_VALUE)
                continue;
            READING r;
            ZeroMemory(&r, sizeof(r));
            if (gamesir_read_co(h, c->outLen, c->inLen, &r) && r.valid) {
                WCHAR det[144];
                const WCHAR *tp = (lstrcmpW(type, L"设备") == 0) ? L"手柄" : type;
                wsprintfW(det, L"GameSir · 0x12 状态报文 · %u 字节", c->inLen);
                fill_common(d, c, K_GAMESIR, brand_name(c->vid, c->pid), tp, det);
                d->h = h;
                d->outLen = c->outLen;
                d->inLen = c->inLen;
                d->featLen = c->featLen;
                d->lastLevel = r.level;
                logf(L"match GAMESIR level=%d charging=%d", r.level, r.charging);
                return TRUE;
            }
            CloseHandle(h);
        }
        if (cand >= 0) {
            const COLL *c = &col[idx[cand]];
            HANDLE h = dev_open_ex(c->path, FALSE);
            if (h != INVALID_HANDLE_VALUE) {
                WCHAR det[144];
                const WCHAR *tp = (lstrcmpW(type, L"设备") == 0) ? L"手柄" : type;
                wsprintfW(det, L"GameSir · 0x12 状态报文 · %u 字节 · 无应答", c->inLen);
                fill_common(d, c, K_GAMESIR, brand_name(c->vid, c->pid), tp, det);
                d->h = h;
                d->outLen = c->outLen;
                d->inLen = c->inLen;
                d->featLen = c->featLen;
                d->lastLevel = -1;
                logf(L"match GAMESIR 无应答");
                return TRUE;
            }
        }
    }

    /* 8.5) 0xFF90 厂商通道：耳机 dongle。
       必须要求整组里出现音频类集合（TM_AUDIO），否则会和鼠标的其它
       厂商页面混淆。这里不做「必须先应答」的门槛——耳机可能正处于
       关机/休眠，先把它列出来、显示未知电量，比凭空消失更好。 */
    if (mask & TM_AUDIO) {
        for (int k = 0; k < gn; k++) {
            const COLL *c = &col[idx[k]];
            if (c->usagePage != 0xFF90 || c->usage != 0x0001)
                continue;
            if (c->inLen != 64 || c->outLen != 64)
                continue;
            HANDLE h = dev_open_ex(c->path, FALSE);
            if (h == INVALID_HANDLE_VALUE)
                continue;

            READING r;
            ZeroMemory(&r, sizeof(r));
            BOOL answered = vend90_query_co(h, c->outLen, c->inLen, &r);

            WCHAR bstage[72];
            const WCHAR *brand = brand_for_vendor_dongle(c, bstage, 72);
            fill_common(d, c, K_VEND90, brand, type, L"");

            wsprintfW(d->detail, L"0xFF90 厂商通道 · 64 字节 · %s",
                      answered ? L"已应答" : L"无应答");
            d->h = h;
            d->outLen = c->outLen;
            d->inLen = c->inLen;
            d->featLen = c->featLen;
            if (answered)
                d->lastLevel = r.level;
            else
                d->lastLevel = -1;   /* 不然 ZeroMemory 留下的 0 会被当成「电量 0%」 */
            logf(L"match VEND90 answered=%d level=%d", answered, d->lastLevel);
            return TRUE;
        }
    }

    /* 9) Compx 短报告：ATK / VXE / VGN / 达尔优 / G-Wolves / 兰族 / Pulsar 等 */
    for (int k = 0; k < gn; k++) {
        const COLL *c = &col[idx[k]];
        if (!is_vendor_page(c->usagePage))
            continue;
        if (c->outLen < 12 || c->outLen > 32)
            continue;
        HANDLE h = dev_open_ex(c->path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (compx17_query(h, c->outLen, c->inLen, &r)) {
            WCHAR det[144];
            wsprintfW(det, L"Compx Nordic52 · %u 字节 08-04", c->outLen);
            fill_common(d, c, K_COMPX17, brand_name(c->vid, c->pid), type, det);
            d->h = h;
            d->outLen = c->outLen;
            d->inLen = c->inLen;
            d->featLen = c->featLen;
            logf(L"match COMPX17");
            return TRUE;
        }
        CloseHandle(h);
    }

    /* 10) Compx 长报告（Nordic54，ATK Zero / 部分 VXE 等） */
    for (int k = 0; k < gn; k++) {
        const COLL *c = &col[idx[k]];
        if (!is_vendor_page(c->usagePage))
            continue;
        if (c->usagePage == 0xFF90)   /* 耳机 dongle 的地盘，已在上一步处理 */
            continue;
        if (c->outLen < 64 || c->outLen > 80)
            continue;
        HANDLE h = dev_open_ex(c->path, FALSE);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (compx64_query(h, c->outLen, c->inLen, &r)) {
            WCHAR det[144];
            wsprintfW(det, L"Compx Nordic54 · %u 字节 08-7D", c->outLen);
            fill_common(d, c, K_COMPX64, brand_name(c->vid, c->pid), type, det);
            d->h = h;
            d->outLen = c->outLen;
            d->inLen = c->inLen;
            d->featLen = c->featLen;
            logf(L"match COMPX64");
            return TRUE;
        }
        CloseHandle(h);
    }

    return FALSE;
}

/* ---------------- 扫描 ---------------- */

static void dev_free(DEVENT *d)
{
    if (d->h) {
        CancelIo(d->h);
        CloseHandle(d->h);
    }
    if (d->bat)
        CloseHandle(d->bat);
    if (d->ev)
        CloseHandle(d->ev);
    ZeroMemory(d, sizeof(*d));
}

void prov_close(void)
{
    for (int i = 0; i < PROV_MAX; i++)
        dev_free(&g_dev[i]);
    g_ndev = 0;
}

/* Windows 电池类设备兜底：可以同时有多台（蓝牙耳机 + 蓝牙鼠标） */
static void winbat_scan(USHORT *claimedVid, USHORT *claimedPid, int nClaimed, BOOL haveXi)
{
    HDEVINFO hdi = SetupDiGetClassDevsW(&kBatClass, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdi == INVALID_HANDLE_VALUE)
        return;

    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);
    BYTE detail[1024];

    for (DWORD i = 0; g_ndev < PROV_MAX; i++) {
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(hdi, NULL, &kBatClass, i, &did))
            break;

        DWORD need = 0;
        if (!SetupDiGetDeviceInterfaceDetailW(hdi, &did, NULL, 0, &need, NULL) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (need == 0 || need > sizeof(detail))
            continue;

        SP_DEVINFO_DATA dinfo;
        ZeroMemory(&dinfo, sizeof(dinfo));
        dinfo.cbSize = sizeof(dinfo);

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pd = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)detail;
        pd->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hdi, &did, pd, need, &need, &dinfo))
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

        /* 设备名：蓝牙设备通常就是产品名，用它当「品牌」显示 */
        WCHAR name[128];
        name[0] = 0;
        {
            DWORD type = 0, sz = 0;
            if (!SetupDiGetDeviceRegistryPropertyW(hdi, &dinfo, SPDRP_FRIENDLYNAME,
                                                   &type, (PBYTE)name, sizeof(name), &sz) ||
                !name[0]) {
                if (!SetupDiGetDeviceRegistryPropertyW(hdi, &dinfo, SPDRP_DEVICEDESC,
                                                       &type, (PBYTE)name, sizeof(name), &sz))
                    name[0] = 0;
            }
        }

        /* 去重：同一个物理设备已经被协议族认领时不要再单列一个 */
        {
            WCHAR inst[512];
            inst[0] = 0;
            if (SetupDiGetDeviceInstanceIdW(hdi, &dinfo, inst, 512, NULL)) {
                USHORT v = 0, p = 0;
                if (parse_vidpid(inst, &v, &p)) {
                    BOOL dup = FALSE;
                    for (int k = 0; k < nClaimed; k++) {
                        if (claimedVid[k] == v && claimedPid[k] == p)
                            dup = TRUE;
                    }
                    if (dup) {
                        logf(L"skip battery (claimed by protocol): %s", name);
                        continue;
                    }
                }
            }
        }

        /* XInput 已经报过的 Xbox 手柄不再重复统计 */
        if (haveXi && w_contains(name, L"xbox")) {
            logf(L"skip battery (xinput): %s", name);
            continue;
        }

        HANDLE h = CreateFileW(pd->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        READING r;
        ZeroMemory(&r, sizeof(r));
        if (!winbat_query_handle(h, &r)) {
            CloseHandle(h);
            continue;
        }

        DEVENT *d = &g_dev[g_ndev];
        ZeroMemory(d, sizeof(*d));
        d->kind = K_WINBAT;
        d->bat = h;
        lstrcpynW(d->brand, name[0] ? name : L"蓝牙设备", 72);
        lstrcpynW(d->type, type_from_name(name), 16);
        lstrcpynW(d->detail, L"Windows 电池类设备", 144);
        d->lastLevel = r.level;
        logf(L"match WINBAT name=%s level=%d", d->brand, r.level);
        g_ndev++;
    }
    SetupDiDestroyDeviceInfoList(hdi);
}

int prov_scan(void)
{
    prov_close();
    g_logLen = 0;
    g_log[0] = 0;
    ZeroMemory(g_v90fail, sizeof(g_v90fail));
    ZeroMemory(g_gsrFail, sizeof(g_gsrFail));

    COLL col[MAX_COLL];
    int n = coll_enum(col, MAX_COLL);
    logf(L"---- enum %d hid interfaces ----", n);

    BOOL used[MAX_COLL];
    ZeroMemory(used, sizeof(used));

    USHORT claimedVid[PROV_MAX], claimedPid[PROV_MAX];
    int nClaimed = 0;

    /* 按物理设备分组：同一接收器的多个集合合并成一个设备 */
    for (int i = 0; i < n && g_ndev < PROV_MAX; i++) {
        if (used[i])
            continue;

        WCHAR key[PATH_CAP];
        group_key(&col[i], key);

        int idx[MAX_COLL], gn = 0;
        BYTE mask = 0;
        for (int j = i; j < n; j++) {
            if (used[j])
                continue;
            WCHAR k2[PATH_CAP];
            group_key(&col[j], k2);
            if (lstrcmpW(key, k2) != 0)
                continue;
            used[j] = TRUE;
            idx[gn++] = j;
            mask |= col[j].mask;
        }

        DEVENT tmp;
        if (match_group(col, idx, gn, mask, &tmp)) {
            if (g_ndev < PROV_MAX) {
                g_dev[g_ndev] = tmp;
                g_ndev++;
                if (nClaimed < PROV_MAX) {
                    claimedVid[nClaimed] = tmp.vid;
                    claimedPid[nClaimed] = tmp.pid;
                    nClaimed++;
                }
            } else {
                dev_free(&tmp);
            }
        }
    }

    /* XInput 手柄（Xbox / 北通 / 飞智等 XInput 模式） */
    BOOL xiGood = FALSE;

    /* 小鸡手柄同时暴露两个身份：0x2F24 的 XInput 和 0x3537 的厂商接口，
       电量只有后者有。已经认出小鸡，就不再重复列一台「读不到电量」的手柄。 */
    BOOL haveGsr = FALSE;
    for (int i = 0; i < g_ndev; i++) {
        if (g_dev[i].kind == K_GAMESIR) {
            haveGsr = TRUE;
            break;
        }
    }

    if (!haveGsr && xi_init()) {
        for (DWORD i = 0; i < 4 && g_ndev < PROV_MAX; i++) {
            READING r;
            ZeroMemory(&r, sizeof(r));
            if (!xi_read(i, &r)) {
                /* 读不到电量不等于没插手柄：登记一台「不可读」的，
                   让右键菜单能说明原因，而不是让它凭空消失。 */
                BOOL wired = FALSE;
                if (!xi_connected(i, &wired))
                    continue;
                DEVENT *d = &g_dev[g_ndev];
                ZeroMemory(d, sizeof(*d));
                d->kind = K_XINPUT;
                d->xiIndex = i;
                lstrcpyW(d->brand, L"Xbox / XInput");
                lstrcpyW(d->type, L"手柄");
                lstrcpyW(d->detail, wired ? L"XInput · 系统报「有线」，读不到电量"
                                          : L"XInput · 手柄未上报电量");
                d->lastLevel = -1;
                logf(L"match XINPUT idx=%lu 无电量 (wired=%d)", i, (int)wired);
                g_ndev++;
                continue;
            }
            DEVENT *d = &g_dev[g_ndev];
            ZeroMemory(d, sizeof(*d));
            d->kind = K_XINPUT;
            d->xiIndex = i;
            lstrcpyW(d->brand, L"Xbox / XInput");
            lstrcpyW(d->type, L"手柄");
            lstrcpyW(d->detail, L"XInput · 系统电量接口（粗粒度 4 档）");
            d->lastLevel = r.level;
            xiGood = TRUE;
            logf(L"match XINPUT idx=%lu level=%d", i, r.level);
            g_ndev++;
        }
    }

    /* 通用兜底：Windows 电池类设备 */
    winbat_scan(claimedVid, claimedPid, nClaimed, xiGood);

#ifdef BATT_FAKE
    /* 调试构建专用：伪造几台设备，用来检查多图标布局与悬停提示。
       正式构建不定义 BATT_FAKE，这段不会编进去。 */
    {
        static const struct { const WCHAR *brand, *type; int lv; } fake[] = {
            { L"罗技 Logitech",        L"耳机", 78 },
            { L"Xbox / XInput",        L"手柄", 60 },
            { L"索尼 PlayStation",     L"手柄", 35 },
            { L"罗技 Logitech",        L"鼠标", 100 },
        };
        for (int i = 0; i < (int)(sizeof(fake) / sizeof(fake[0])) && g_ndev < PROV_MAX; i++) {
            DEVENT *d = &g_dev[g_ndev];
            ZeroMemory(d, sizeof(*d));
            d->kind = K_FAKE;
            lstrcpynW(d->brand, fake[i].brand, 72);
            lstrcpynW(d->type, fake[i].type, 16);
            lstrcpyW(d->detail, L"调试用假设备");
            d->lastLevel = fake[i].lv;
            g_ndev++;
        }
    }
#endif

    logf(L"---- %d device(s) ----", g_ndev);
    return g_ndev;
}

/* ---------------- 对外接口 ---------------- */

int prov_count(void) { return g_ndev; }

BOOL prov_desc(int i, DEVDESC *d)
{
    if (!d || i < 0 || i >= g_ndev)
        return FALSE;
    d->brand   = g_dev[i].brand;
    d->type    = g_dev[i].type;
    d->detail  = g_dev[i].detail;
    d->passive = (g_dev[i].kind == K_RAPOO || g_dev[i].kind == K_SONY);
    d->xinput  = (g_dev[i].kind == K_XINPUT);
    return TRUE;
}

BOOL prov_is_passive(int i)
{
    if (i < 0 || i >= g_ndev)
        return FALSE;
    return (g_dev[i].kind == K_RAPOO || g_dev[i].kind == K_SONY);
}

HANDLE prov_event(int i)
{
    if (i < 0 || i >= g_ndev || !prov_is_passive(i))
        return NULL;
    return g_dev[i].ev;
}

/* 关掉句柄但保留品牌/类别，界面还能继续显示「等待数据」而不是凭空消失 */
static void dev_drop_handle(DEVENT *d)
{
    if (d->h) {
        CancelIo(d->h);
        CloseHandle(d->h);
        d->h = NULL;
    }
    d->pending = FALSE;
}

void prov_arm(int i)
{
    if (i < 0 || i >= g_ndev)
        return;
    DEVENT *d = &g_dev[i];
    if (!prov_is_passive(i) || !d->h || !d->ev)
        return;

    ZeroMemory(&d->ov, sizeof(d->ov));
    d->ov.hEvent = d->ev;
    ResetEvent(d->ev);
    d->pending = TRUE;

    DWORD len = (d->inLen > 0 && d->inLen <= (DWORD)sizeof(d->buf)) ? (DWORD)d->inLen : 64;
    if (!ReadFile(d->h, d->buf, len, NULL, &d->ov)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            logf(L"arm failed kind=%d err=%lu", d->kind, GetLastError());
            dev_drop_handle(d);
        }
    }
}

static void keep_raw(DEVENT *d, DWORD got)
{
    d->lastRawLen = (int)got;
    if (d->lastRawLen > (int)sizeof(d->lastRaw))
        d->lastRawLen = (int)sizeof(d->lastRaw);
    CopyMemory(d->lastRaw, d->buf, (SIZE_T)d->lastRawLen);
}

BOOL prov_read_ready(int i, READING *r)
{
    r->valid = FALSE;
    r->level = -1;
    r->charging = -1;
    if (i < 0 || i >= g_ndev)
        return FALSE;

    DEVENT *d = &g_dev[i];
    if (!d->h || !d->pending)
        return FALSE;

    DWORD got = 0;
    if (!GetOverlappedResult(d->h, &d->ov, &got, FALSE)) {
        if (GetLastError() == ERROR_IO_INCOMPLETE)
            return FALSE;
        dev_drop_handle(d);
        return FALSE;
    }
    d->pending = FALSE;

    if (d->kind == K_RAPOO) {
        if (got >= 2 && d->buf[0] == 0xBB) {
            int lv = d->buf[got - 1];
            if (lv >= 0 && lv <= 100) {
                r->level = lv;
                r->charging = -1;
                r->valid = TRUE;
                keep_raw(d, got);
            }
        } else if (got > 0) {
            logf(L"raboo unknown frame: %02X %02X", d->buf[0], got > 1 ? d->buf[1] : 0);
        }
    } else if (d->kind == K_SONY) {
        if (sony_parse(d, d->buf, got, r))
            keep_raw(d, got);
    }

    if (r->valid)
        d->lastLevel = r->level;
    return r->valid;
}

/* 离线设备的复活探测要能真的发一帧：不清掉退避的话，那些「连续失败就降频」
   的协议族会直接把请求吞掉，界面层永远等不到回应。 */
void prov_reset_backoff(int i)
{
    if (i < 0 || i >= PROV_MAX)
        return;
    g_v90fail[i] = 0;
    g_gsrFail[i] = 0;
}

BOOL prov_poll(int i, READING *r)
{
    r->valid = FALSE;
    r->level = -1;
    r->charging = -1;
    if (i < 0 || i >= g_ndev)
        return FALSE;

    DEVENT *d = &g_dev[i];
    BOOL ok = FALSE;

    switch (d->kind) {
    case K_HIDPP2:
        ok = hidpp_read(d, r);
        break;
    case K_RAZER:
        ok = d->h ? razer_query(d->h, d->featLen, r) : FALSE;
        break;
    case K_ROG:
        ok = d->h ? rog_query(d->h, d->outLen, d->inLen, r) : FALSE;
        break;
    case K_LEOBOG:
        ok = d->h ? leobog_query(d->h, d->outLen, d->inLen, r) : FALSE;
        break;
    case K_REALTEK:
        ok = d->h ? realtek_query(d->h, d->outLen, d->inLen, r) : FALSE;
        break;
    case K_FLYDIGI:
        ok = d->h ? flydigi_query(d->h, d->outLen, d->inLen, r) : FALSE;
        break;
    case K_COMPX17:
        ok = d->h ? compx17_query(d->h, d->outLen, d->inLen, r) : FALSE;
        break;
    case K_COMPX64:
        ok = d->h ? compx64_query(d->h, d->outLen, d->inLen, r) : FALSE;
        break;
    case K_XINPUT:
        ok = xi_read(d->xiIndex, r);
        break;
    case K_WINBAT:
        ok = winbat_query_handle(d->bat, r);
        break;
    case K_VEND90:
        /* 连续失败后降到约 3 分钟试一次：这类 dongle 一旦不应答，
           每 20 秒去敲一次只会白等 250ms，还会让菜单卡顿。 */
        if (g_v90fail[i] >= 3 && (g_v90fail[i] % 9) != 0) {
            g_v90fail[i]++;
            break;
        }
        ok = d->h ? vend90_query_co(d->h, d->outLen, d->inLen, r) : FALSE;
        if (ok && r->valid) {
            g_v90fail[i] = 0;
            if (w_contains(d->detail, L"无应答")) {
                wsprintfW(d->detail, L"0xFF90 厂商通道 · 64 字节 · 已应答");
                logf(L"VEND90 starts answering, level=%d", r->level);
            }
        } else {
            g_v90fail[i]++;
        }
        break;
    case K_GAMESIR:
        /* 读不到 0x12 报文时降频：这类设备不该每轮都白等三次读超时。 */
        if (g_gsrFail[i] >= 3 && (g_gsrFail[i] % 9) != 0) {
            g_gsrFail[i]++;
            break;
        }
        ok = d->h ? gamesir_read_co(d->h, d->outLen, d->inLen, r) : FALSE;
        if (ok && r->valid) {
            g_gsrFail[i] = 0;
            if (w_contains(d->detail, L"无应答")) {
                wsprintfW(d->detail, L"GameSir · 0x12 状态报文 · %d 字节", d->inLen);
                logf(L"GAMESIR starts answering, level=%d", r->level);
            }
        } else {
            g_gsrFail[i]++;
        }
        break;
    case K_FAKE:
        r->level = d->lastLevel;
        r->charging = -1;
        r->valid = TRUE;
        ok = TRUE;
        break;
    default:
        ok = FALSE;
        break;
    }

    if (ok && r->valid)
        d->lastLevel = r->level;
    return ok;
}

int prov_last_raw(int i, BYTE *out, int cap)
{
    if (i < 0 || i >= g_ndev)
        return 0;
    DEVENT *d = &g_dev[i];
    int n = d->lastRawLen;
    if (n > cap)
        n = cap;
    if (n <= 0)
        return 0;
    CopyMemory(out, d->lastRaw, (SIZE_T)n);
    return n;
}

BOOL prov_diag(const WCHAR *path)
{
    /* 整段先拼成宽字符，再一次性转 UTF-8：方便直接粘贴到 issue */
    WCHAR *txt = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 65536 * sizeof(WCHAR));
    if (!txt)
        return FALSE;

    int len = 0;
    len += wsprintfW(txt + len,
                     L"BattTray diagnostics\r\n"
                     L"version  : %s\r\n"
                     L"devices  : %d\r\n"
                     L"interval : %d s\r\n\r\n",
                     BATT_VERSION, g_ndev, g_pollMsHint / 1000);

    for (int i = 0; i < g_ndev; i++) {
        DEVENT *d = &g_dev[i];
        len += wsprintfW(txt + len,
                         L"[%d] %s / %s\r\n"
                         L"    protocol: %s\r\n"
                         L"    vid=%04X pid=%04X  reports: in=%d out=%d feature=%d\r\n"
                         L"    level=%d  passive=%d  xinput=%d\r\n\r\n",
                         i, d->brand, d->type, d->detail,
                         d->vid, d->pid, d->inLen, d->outLen, d->featLen,
                         d->lastLevel, (int)prov_is_passive(i),
                         (int)(d->kind == K_XINPUT));
    }

    if (g_logLen > 0 && len + g_logLen < 64000) {
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
