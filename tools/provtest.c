/* provtest.c - 控制台调试程序：验证协议层是否正常（不参与发布） */
#include <windows.h>
#include <stdio.h>
#include "providers.h"

int main(void)
{
    printf("scanning...\n");
    fflush(stdout);

    BOOL ok = prov_scan();
    printf("scan ok=%d passive=%d\n", (int)ok, (int)prov_passive());
    fflush(stdout);

    READING r;
    r.valid = FALSE;
    r.level = -1;
    r.charging = -1;

    if (ok && prov_passive()) {
        prov_arm();
        printf("waiting for passive report (8s)...\n");
        fflush(stdout);
        HANDLE ev = prov_event();
        if (ev && WaitForSingleObject(ev, 8000) == WAIT_OBJECT_0) {
            if (prov_read_ready(&r))
                printf("report: level=%d charging=%d\n", r.level, r.charging);
            else
                printf("report parse failed\n");
        } else {
            printf("no report in time\n");
        }
    } else if (ok) {
        for (int i = 0; i < 3; i++) {
            if (prov_poll(&r))
                printf("poll[%d]: level=%d charging=%d\n", i, r.level, r.charging);
            else
                printf("poll[%d]: failed\n", i);
            fflush(stdout);
            Sleep(500);
        }
    }

    prov_diag(L"diag.txt");
    printf("diag written\n");
    prov_close();
    return 0;
}
