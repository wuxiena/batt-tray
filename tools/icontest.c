/* 托盘图标离线预览：直接包含主程序源码，复用同一份绘制代码，
   预览与实际渲染因此不会出现偏差（不参与发布构建）。

   输出 preview.bmp（32bpp 顶向下，256 色以内）：
     第 1 行 = 浅色任务栏底色上的各电量状态
     第 2 行 = 深色任务栏底色上的各电量状态

   编译（在仓库根目录执行）：
     zig cc -Isrc -o tools/icontest.exe tools/icontest.c src/providers.c ^
         -luser32 -lgdi32 -lhid -lsetupapi -lm
*/
#define WinMain app_winmain_unused
#include "BattTray.c"
#undef WinMain

#include <stdio.h>
#include <stdlib.h>

#define CELL   16
#define GAP    6
#define MARGIN 8

/* 把带 alpha 的图标像素合成到不透明底色上 */
static void blend_over(DWORD *dst, int dw, int dx, int dy, const DWORD *src, int s, DWORD bg)
{
    for (int y = 0; y < s; y++) {
        for (int x = 0; x < s; x++) {
            DWORD p = src[y * s + x];
            int a  = (int)(p >> 24);
            int r  = (int)((p >> 16) & 0xFF), g = (int)((p >> 8) & 0xFF), b = (int)(p & 0xFF);
            int br = (int)((bg >> 16) & 0xFF), bgc = (int)((bg >> 8) & 0xFF), bb = (int)(bg & 0xFF);
            int rr = (r * a + br * (255 - a)) / 255;
            int gg = (g * a + bgc * (255 - a)) / 255;
            int bl = (b * a + bb * (255 - a)) / 255;
            dst[(dy + y) * dw + dx + x] =
                0xFF000000u | ((DWORD)rr << 16) | ((DWORD)gg << 8) | (DWORD)bl;
        }
    }
}

static void icon_pixels(HICON ic, DWORD *out, int s)
{
    ICONINFO ii;
    if (!GetIconInfo(ic, &ii))
        return;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = s;
    bi.bmiHeader.biHeight      = -s;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(NULL);
    GetDIBits(dc, ii.hbmColor, 0, (UINT)s, out, &bi, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    if (ii.hbmMask)
        DeleteObject(ii.hbmMask);
}

static void write_bmp(const char *path, const DWORD *px, int w, int h)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    ZeroMemory(&fh, sizeof(fh));
    ZeroMemory(&ih, sizeof(ih));

    DWORD imgBytes = (DWORD)w * h * 4;
    ih.biSize        = sizeof(ih);
    ih.biWidth       = w;
    ih.biHeight      = -h;        /* 负值 = 顶向下 */
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage   = imgBytes;

    fh.bfType    = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize    = fh.bfOffBits + imgBytes;

    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(px, 1, imgBytes, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    static const int def[5] = { 92, 45, 8, 100, -1 };
    int levels[12];
    int N = 5;

    /* 可选：按命令行给的电量渲染，例如 icontest.exe 78 60 35 100 */
    if (argc > 1) {
        N = 0;
        for (int i = 1; i < argc && N < 12; i++)
            levels[N++] = atoi(argv[i]);
        if (N == 0) {
            N = 5;
            for (int i = 0; i < 5; i++)
                levels[i] = def[i];
        }
    } else {
        for (int i = 0; i < 5; i++)
            levels[i] = def[i];
    }

    /* 三行：浅色任务栏 / 深色任务栏 / 缓存读数（未连接） */
    static const DWORD bgs[3]   = { 0xFFF3F3F3u, 0xFF202020u, 0xFFF3F3F3u };
    static const int   dims[3]  = { 0, 0, 1 };
    const int ROWS = 3;

    const int W = MARGIN * 2 + N * CELL + (N - 1) * GAP;
    const int H = MARGIN * 2 + ROWS * CELL + (ROWS - 1) * GAP;

    DWORD *sheet = (DWORD *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      (SIZE_T)W * H * 4);
    if (!sheet)
        return 1;

    for (int row = 0; row < ROWS; row++)
        for (int y = 0; y < CELL; y++)
            for (int x = 0; x < W; x++)
                sheet[(MARGIN + row * (CELL + GAP) + y) * W + x] = bgs[row];

    DWORD px[ICON_MAX * ICON_MAX];
    for (int row = 0; row < ROWS; row++) {
        for (int i = 0; i < N; i++) {
            HICON ic = build_icon(levels[i], dims[row]);
            if (!ic)
                continue;
            ZeroMemory(px, sizeof(px));
            icon_pixels(ic, px, CELL);
            DestroyIcon(ic);
            blend_over(sheet, W, MARGIN + i * (CELL + GAP),
                       MARGIN + row * (CELL + GAP), px, CELL, bgs[row]);
        }
    }

    write_bmp("preview.bmp", sheet, W, H);
    printf("preview.bmp %dx%d\n", W, H);

    HeapFree(GetProcessHeap(), 0, sheet);
    return 0;
}
