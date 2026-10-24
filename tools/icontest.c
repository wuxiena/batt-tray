// 仅用于验证图标绘制的测试程序（不参与发布）
#define RB_ICON_TEST 1
#include "RapooBattery.c"

static HICON icons[8];

static LRESULT CALLBACK test_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp; (void)lp;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc = { 0, 0, 640, 320 };
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        for (int i = 0; i < 4; i++)
            DrawIconEx(dc, 16 + i * 90, 16, icons[i], 32, 32, 0, NULL, DI_NORMAL);
        for (int i = 0; i < 4; i++)
            DrawIconEx(dc, 16 + i * 90, 70, icons[i], 16, 16, 0, NULL, DI_NORMAL);
        for (int i = 0; i < 4; i++)
            DrawIconEx(dc, 16 + i * 90, 110, icons[i], 48, 48, 0, NULL, DI_NORMAL);
        /* 深色背景下的显示效果 */
        RECT dark = { 16, 180, 400, 290 };
        FillRect(dc, &dark, (HBRUSH)GetStockObject(BLACK_BRUSH));
        for (int i = 0; i < 4; i++)
            DrawIconEx(dc, 30 + i * 90, 205, icons[i], 32, 32, 0, NULL, DI_NORMAL);
        for (int i = 0; i < 4; i++)
            DrawIconEx(dc, 30 + i * 90, 250, icons[i], 16, 16, 0, NULL, DI_NORMAL);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    SetProcessDPIAware();
    icons[0] = build_icon(93, FALSE);
    icons[1] = build_icon(45, FALSE);
    icons[2] = build_icon(12, FALSE);
    icons[3] = build_icon(-1, TRUE);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = test_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"IconTestWnd";
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"IconTest",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             100, 100, 660, 340, NULL, NULL, inst, NULL);
    ShowWindow(h, SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
