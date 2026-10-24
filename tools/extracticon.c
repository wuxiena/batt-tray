/* extracticon.c - 从 exe 里取出图标并存成 BMP（验证资源段是否真的可用）
 *
 * 用法: extracticon <目标.exe> <输出.bmp>
 * 走的是 Windows 自己的图标加载器（ExtractIconEx），和资源管理器显示 exe
 * 图标走的是同一条路径。输出 BMP 可直接查看。
 *
 * 注意：不要用 DrawIconEx 画到 32bpp DIB 上再取——GDI 合成会丢 alpha，
 * 透明图标会变成全透明。这里直接读图标自己的 hbmColor 位图。
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: extracticon <exe> <out.bmp>\n"); return 2; }
    HICON large = NULL, small = NULL;
    if (!ExtractIconExA(argv[1], 0, &large, &small, 1) || !large) {
        printf("ExtractIconEx failed (%lu)\n", GetLastError());
        return 1;
    }

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    if (!GetIconInfo(large, &ii)) { printf("GetIconInfo failed\n"); return 1; }

    BITMAP bm;
    ZeroMemory(&bm, sizeof(bm));
    GetObject(ii.hbmColor, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          /* 负数 = 自上而下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(NULL);
    unsigned char *bits = (unsigned char *)malloc((size_t)w * h * 4);
    if (!bits) return 1;
    if (GetDIBits(dc, ii.hbmColor, 0, (UINT)h, bits, &bi, DIB_RGB_COLORS) == 0) {
        printf("GetDIBits failed %lu\n", GetLastError());
        return 1;
    }

    FILE *f = fopen(argv[2], "wb");
    if (!f) { printf("cannot write %s\n", argv[2]); return 1; }
    BITMAPFILEHEADER fh;
    ZeroMemory(&fh, sizeof(fh));
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + (DWORD)w * h * 4;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(bits, (size_t)w * h * 4, 1, f);
    fclose(f);

    /* 统计 alpha，确认不是全透明 */
    int opaque = 0, clear = 0;
    for (int i = 0; i < w * h; i++) {
        if (bits[i * 4 + 3] > 0) opaque++;
        else clear++;
    }
    printf("wrote %s (%dx%d) opaque=%d clear=%d\n", argv[2], w, h, opaque, clear);
    return 0;
}
