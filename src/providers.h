// providers.h - 多品牌无线设备电量读取（协议族驱动，支持多设备并存）
#ifndef PROVIDERS_H
#define PROVIDERS_H

#include <windows.h>

#ifndef BATT_VERSION
#define BATT_VERSION L"1.1.3"
#endif

#define PROV_MAX 6          /* 同时跟踪的设备上限 */

typedef struct {
    BOOL valid;
    int  level;      /* 0..100，-1 表示未知 */
    int  charging;   /* -1 未知 / 0 未充电 / 1 充电中 */
} READING;

/* 设备静态描述（指针指向内部存储，下次 prov_scan 前有效） */
typedef struct {
    const WCHAR *brand;   /* 品牌名，如 "雷柏 Rapoo"、"罗技 Logitech" */
    const WCHAR *type;    /* 类别，如 "鼠标" / "耳机" / "手柄" */
    const WCHAR *detail;  /* 协议说明，用于菜单第二行 */
    BOOL passive;         /* TRUE = 设备主动上报，无需轮询 */
    BOOL xinput;          /* TRUE = 由 XInput 读取，等级只有粗粒度 */
} DEVDESC;

/* 扫描全部可识别设备，返回设备数量（0..PROV_MAX） */
int  prov_scan(void);
void prov_close(void);
int  prov_count(void);
BOOL prov_desc(int i, DEVDESC *d);

/* 被动监听型：由消息循环等待事件，收到上报后解析 */
BOOL   prov_is_passive(int i);
HANDLE prov_event(int i);
void   prov_arm(int i);
BOOL   prov_read_ready(int i, READING *r);

/* 主动查询型：按间隔调用 */
BOOL prov_poll(int i, READING *r);

int  prov_last_raw(int i, BYTE *out, int cap); /* 最近一帧原始报文，返回字节数 */
BOOL prov_diag(const WCHAR *path);             /* 导出诊断信息 */

void prov_set_poll_ms(int ms);                 /* 仅用于诊断文件里注明查询间隔 */
void prov_reset_backoff(int i);                /* 清掉该设备的失败退避，用于离线后的复活探测 */

#endif
