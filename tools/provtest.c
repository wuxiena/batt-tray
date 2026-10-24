/* provtest.c - 控制台调试程序：验证协议层是否正常（不参与发布） */
#include <windows.h>
#include <stdio.h>
#include "providers.h"

static char *u8(const WCHAR *w)
{
    static char buf[4][512];
    static int slot;
    char *out = buf[slot++ & 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, 511, NULL, NULL);
    if (n <= 0)
        out[0] = 0;
    return out;
}

int main(void)
{
    SetConsoleOutputCP(65001);
    printf("scanning...\n");
    fflush(stdout);

    int n = prov_scan();
    printf("devices = %d\n", n);

    for (int i = 0; i < n; i++) {
        DEVDESC d;
        if (!prov_desc(i, &d))
            continue;
        printf("[%d] %s | %s | %s | passive=%d xinput=%d\n",
               i, u8(d.brand), u8(d.type), u8(d.detail),
               (int)d.passive, (int)d.xinput);
    }
    fflush(stdout);

    /* 主动型：直接查一次 */
    for (int i = 0; i < n; i++) {
        if (prov_is_passive(i))
            continue;
        READING r;
        ZeroMemory(&r, sizeof(r));
        if (prov_poll(i, &r))
            printf("  poll[%d] level=%d charging=%d\n", i, r.level, r.charging);
        else
            printf("  poll[%d] failed\n", i);
    }
    fflush(stdout);

    /* 被动型：同时挂起，等 10 秒 */
    HANDLE evs[PROV_MAX];
    int    map[PROV_MAX];
    int    cnt = 0;
    for (int i = 0; i < n; i++) {
        if (!prov_is_passive(i))
            continue;
        HANDLE ev = prov_event(i);
        if (!ev)
            continue;
        prov_arm(i);
        evs[cnt] = ev;
        map[cnt] = i;
        cnt++;
    }

    if (cnt) {
        printf("waiting for %d passive device(s), 10s...\n", cnt);
        fflush(stdout);

        ULONGLONG end = GetTickCount64() + 10000;
        int seen[PROV_MAX];
        ZeroMemory(seen, sizeof(seen));

        while (GetTickCount64() < end) {
            DWORD left = (DWORD)(end - GetTickCount64());
            if (left > 2000) left = 2000;
            DWORD r = WaitForMultipleObjects((DWORD)cnt, evs, FALSE, left);
            if (r == WAIT_TIMEOUT)
                continue;
            if (r == WAIT_FAILED)
                break;
            int k = (int)(r - WAIT_OBJECT_0);
            if (k < 0 || k >= cnt)
                break;
            int i = map[k];
            READING rd;
            if (prov_read_ready(i, &rd))
                printf("  report[%d] level=%d charging=%d\n", i, rd.level, rd.charging);
            else
                printf("  report[%d] parse failed\n", i);
            fflush(stdout);
            seen[k] = 1;
            prov_arm(i);
        }
    }

    BYTE raw[24];
    for (int i = 0; i < n; i++) {
        int rl = prov_last_raw(i, raw, sizeof(raw));
        if (rl <= 0)
            continue;
        printf("raw[%d] ", i);
        for (int j = 0; j < rl && j < 10; j++)
            printf("%02X ", raw[j]);
        printf("\n");
    }

    prov_diag(L"diag.txt");
    printf("diag written\n");
    prov_close();
    return 0;
}
