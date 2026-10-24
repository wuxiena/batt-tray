/* 使用时长 / 历史记录的单元测试（控制台，不参与发布构建）。

   GUI 在沙箱里跑不住，托盘和菜单没法点，所以把界面层的纯逻辑摘出来直接验：
   历史记录解析、时长格式化、累计时长叠加与结算。

   编译（在仓库根目录执行）：
     zig cc -Isrc -o tools/statstest.exe tools/statstest.c src/providers.c ^
         -luser32 -lshell32 -lgdi32 -lhid -lsetupapi -lm
*/
#define WinMain app_winmain_unused
#include "BattTray.c"
#undef WinMain

#include <stdio.h>

static int fails;

static void ck(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL  %s\n", what);
        fails++;
    }
}

static void ckw(const WCHAR *got, const WCHAR *want, const char *what)
{
    if (lstrcmpW(got, want) != 0) {
        printf("FAIL  %s\n      got  %ls\n      want %ls\n", what, got, want);
        fails++;
    }
}

static void ckd(DWORD got, DWORD want, const char *what)
{
    if (got != want) {
        printf("FAIL  %s: got %lu, want %lu\n", what, got, want);
        fails++;
    }
}

int main(void)
{
    HIST h;
    WCHAR buf[48];

    /* ---- 数字解析 ---- */
    ckd(uw_atoi(L"0"), 0, "atoi 0");
    ckd(uw_atoi(L"12345xyz"), 12345, "atoi 遇到非数字停止");

    /* ---- 历史记录格式 ---- */
    ZeroMemory(&h, sizeof(h));
    hist_parse(L"雷柏 Rapoo 鼠标|3661|90312", &h);
    ckw(h.name, L"雷柏 Rapoo 鼠标", "parse 名称");
    ckd(h.sec, 3661, "parse 累计秒");
    ckd(h.stamp, 90312, "parse 最后在线");
    ck(h.used, "parse 有效标记");

    ZeroMemory(&h, sizeof(h));
    hist_parse(L"只有名字没有分隔符", &h);
    ck(!h.used, "parse 缺字段判为无效");

    ZeroMemory(&h, sizeof(h));
    hist_parse(L"设备|120", &h);
    ckd(h.sec, 120, "parse 缺时间戳仍可读");
    ckd(h.stamp, 0, "parse 缺时间戳记 0");

    /* ---- 时长格式化 ---- */
    fmt_dur(0, buf);       ckw(buf, L"0 秒", "dur 0");
    fmt_dur(59, buf);      ckw(buf, L"59 秒", "dur 59 秒");
    fmt_dur(60, buf);      ckw(buf, L"1 分钟", "dur 1 分钟");
    fmt_dur(3599, buf);    ckw(buf, L"59 分钟", "dur 59 分钟");
    fmt_dur(3600, buf);    ckw(buf, L"1 小时 0 分钟", "dur 1 小时");
    fmt_dur(9000, buf);    ckw(buf, L"2 小时 30 分钟", "dur 2.5 小时");
    fmt_dur(86400, buf);   ckw(buf, L"1 天 0 小时", "dur 1 天");
    fmt_dur(200000, buf);  ckw(buf, L"2 天 7 小时", "dur 2 天 7 小时");

    /* ---- 累计使用时长 ---- */
    ZeroMemory(g_hist, sizeof(g_hist));
    g_n = 1;
    ZeroMemory(&g_slot[0], sizeof(SLOT));
    g_slot[0].connSince = GetTickCount64() - 5000;

    DWORD t = slot_total_sec(0);
    ck(t >= 4 && t <= 8, "累计 = 本次会话（约 5 秒）");

    /* 历史里已经存了 30 分钟，应该叠加而不是覆盖 */
    lstrcpyW(g_hist[0].name, L"未知设备");
    g_hist[0].used = TRUE;
    g_hist[0].sec = 1800;
    t = slot_total_sec(0);
    ck(t >= 1804 && t <= 1808, "累计 = 历史 + 本次会话");

    /* 结算：停表、写回历史、断开后不再增长 */
    slot_settle(0);
    ck(g_slot[0].connSince == 0, "结算后停表");
    ck(g_hist[0].sec >= 1804, "结算写回历史");
    t = slot_total_sec(0);
    Sleep(1200);
    ckd(slot_total_sec(0), t, "停表后累计不再增长");

    /* 再结算一次不该重复累加 */
    slot_settle(0);
    ckd(g_hist[0].sec, t, "重复结算不会重复计时");

    /* ---- 断连判定 ---- */
    ZeroMemory(g_hist, sizeof(g_hist));
    ZeroMemory(g_slot, sizeof(g_slot));
    g_n = 2;
    g_sel = 0;

    /* 被动上报型：3 分钟没帧才算走，50 秒不该动 */
    g_slot[0].passive = TRUE;
    g_slot[0].level = 92;
    g_slot[0].linked = TRUE;
    g_slot[0].lastTick = GetTickCount64() - 50000;
    check_offline(GetTickCount64());
    ck(!g_slot[0].dead, "被动型 50 秒没上报仍在列表");

    g_slot[0].lastTick = GetTickCount64() - 200000;
    check_offline(GetTickCount64());
    ck(g_slot[0].dead, "被动型 3 分钟没上报判为断开");
    ck(g_slot[0].level < 0, "断开后电量清空");
    ck(!slot_alive(0), "断开后不算当前设备");
    ck(g_slot[0].connSince == 0, "断开后停表");

    /* 读不到电量的设备（dongle 不提供电量）永远留在列表里 */
    g_slot[1].passive = FALSE;
    g_slot[1].level = -1;
    g_slot[1].lastTick = GetTickCount64() - 900000;
    check_offline(GetTickCount64());
    ck(!g_slot[1].dead, "读不到电量的设备常驻列表");

    /* 主动查询型：一次失败先进复查，不立刻判死 */
    ZeroMemory(&g_slot[1], sizeof(SLOT));
    g_slot[1].level = 55;
    g_slot[1].linked = TRUE;
    g_slot[1].lastTick = GetTickCount64();
    g_slot[1].fail = 1;
    check_offline(GetTickCount64());
    ck(!g_slot[1].dead, "主动型一次失败不判断开");
    ck(g_slot[1].probe == 1, "主动型进入复查");

    /* 复查连失 3 次以上才判死（下一次复查时间还没到，手动推到过去） */
    for (int k = 0; k < GONE_PROBE_MAX + 1; k++) {
        g_slot[1].nextProbe = GetTickCount64() - 1;
        check_offline(GetTickCount64());
    }
    ck(g_slot[1].dead, "复查连续失败后判为断开");

    /* ---- 清理测试写入的注册表值 ---- */
    for (int i = 0; i < HIST_MAX; i++) {
        WCHAR k[8];
        wsprintfW(k, L"H%d", i);
        reg_set_str(k, L"");
    }
    for (int i = 0; i < PROV_MAX; i++) {
        WCHAR k[8];
        wsprintfW(k, L"L%d", i);
        reg_set(k, 0xFFFFFFFFu);
    }

    if (fails)
        printf("\n%d 项失败\n", fails);
    else
        printf("statstest: 全部通过\n");
    return fails ? 1 : 0;
}
