// providers.h - 多品牌无线设备电量读取（协议族驱动）
#ifndef PROVIDERS_H
#define PROVIDERS_H

#include <windows.h>

#ifndef RB_VERSION
#define RB_VERSION L"1.1.0"
#endif

typedef struct {
    BOOL valid;
    int  level;      /* 0..100，-1 表示未知 */
    int  charging;   /* -1 未知 / 0 未充电 / 1 充电中 */
} READING;

/* 扫描设备并选定一个可用提供者。返回 TRUE 表示已锁定。 */
BOOL prov_scan(void);
void prov_close(void);

/* 被动监听型（雷柏）：由消息循环等事件，收到上报后解析 */
BOOL   prov_passive(void);
HANDLE prov_event(void);
void   prov_arm(void);
BOOL   prov_read_ready(READING *r);

/* 主动查询型：按间隔调用 */
BOOL prov_poll(READING *r);

const WCHAR *prov_name(void);      /* 品牌 / 方案名，如 "雷柏 (Rapoo)" */
const WCHAR *prov_detail(void);    /* 匹配到的接口与协议说明 */
int  prov_last_raw(BYTE *out, int cap); /* 最近一帧原始报文（被动型才有），返回字节数 */
BOOL prov_diag(const WCHAR *path); /* 导出诊断信息 */

#endif
