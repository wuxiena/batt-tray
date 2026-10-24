/* embedres.c - 把 .ico 写进 PE 的资源段（等价于 rc.exe 的 "1 ICON"）
 *
 * 用法: embedres <目标.exe> <图标.ico>
 *
 * 做法：解析 .ico（ICONDIR + ICONDIRENTRY），每个图像写成一条 RT_ICON 资源，
 * 再写一条 RT_GROUP_ICON 指向它们。通过 Win32 的 BeginUpdateResource /
 * UpdateResource 完成 —— PE 的段结构由系统自己维护，不用手写 .res / COFF，
 * 也就绕开了 zig rc / windres / llvm-rc 全部工具链依赖。
 *
 * 注意 BMP 帧：.ico 里 DIB 头的高度是实际高度的两倍（XOR + AND 两张掩码），
 * 写进资源段时必须把 biHeight 减半，否则图标会变成纵向拉伸一倍。
 * PNG 帧（Vista+）原样写入即可。
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    WORD idReserved, idType, idCount;
} ICONDIR;

typedef struct {
    BYTE  bWidth, bHeight, bColorCount, bReserved;
    WORD  wPlanes, wBitCount;
    DWORD dwBytesInRes, dwImageOffset;
} ICONDIRENTRY;

typedef struct {
    BYTE  bWidth, bHeight, bColorCount, bReserved;
    WORD  wPlanes, wBitCount;
    DWORD dwBytesInRes;
    WORD  nID;
} GRPICONDIRENTRY;
#pragma pack(pop)

#define RT_ICON_        MAKEINTRESOURCEA(3)
#define RT_GROUP_ICON_  MAKEINTRESOURCEA(14)
#define RES_LANG        0x0409   /* en-US，资源图标与语言无关 */

static unsigned char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: embedres <target.exe> <icon.ico>\n");
        return 2;
    }

    size_t ico_len = 0;
    unsigned char *ico = slurp(argv[2], &ico_len);
    if (!ico) {
        fprintf(stderr, "embedres: cannot read %s\n", argv[2]);
        return 1;
    }
    if (ico_len < sizeof(ICONDIR)) {
        fprintf(stderr, "embedres: %s too small\n", argv[2]);
        return 1;
    }

    ICONDIR *dir = (ICONDIR *)ico;
    if (dir->idReserved != 0 || dir->idType != 1) {
        fprintf(stderr, "embedres: not an icon file\n");
        return 1;
    }
    int count = (int)dir->idCount;
    if (count <= 0 || (size_t)(6 + 16 * count) > ico_len) {
        fprintf(stderr, "embedres: bad icon directory\n");
        return 1;
    }

    HANDLE upd = BeginUpdateResourceA(argv[1], FALSE);
    if (!upd) {
        fprintf(stderr, "embedres: BeginUpdateResource failed (%lu)\n", GetLastError());
        return 1;
    }

    /* 组图标目录：6 字节头 + 每个图像 14 字节 */
    size_t group_len = 6 + (size_t)14 * count;
    unsigned char *group = (unsigned char *)calloc(1, group_len);
    if (!group) { EndUpdateResourceA(upd, TRUE); return 1; }
    group[0] = 0; group[1] = 0;            /* idReserved */
    group[2] = 1; group[3] = 0;            /* idType = 1 (icon) */
    group[4] = (unsigned char)(count & 0xff);
    group[5] = (unsigned char)((count >> 8) & 0xff);

    int failed = 0;
    for (int i = 0; i < count; i++) {
        ICONDIRENTRY *e = (ICONDIRENTRY *)(ico + 6 + 16 * i);
        if ((size_t)e->dwImageOffset + e->dwBytesInRes > ico_len) {
            fprintf(stderr, "embedres: image %d out of range\n", i);
            failed = 1;
            break;
        }
        const unsigned char *img = ico + e->dwImageOffset;
        size_t img_len = e->dwBytesInRes;

        int is_png = (img_len >= 8 && memcmp(img, "\x89PNG\r\n\x1a\n", 8) == 0);
        unsigned char *blob = (unsigned char *)malloc(img_len);
        if (!blob) { failed = 1; break; }
        memcpy(blob, img, img_len);

        /* RT_ICON 的数据布局和 .ico 里的图像完全一致：DIB 头的高度仍是实际
           高度的两倍（XOR + AND 两张掩码），Windows 按这个约定读。
           实测系统自带 exe（notepad/cmd）的 RT_ICON 也是 w=32 h=64。 */
        (void)is_png;

        WORD rid = (WORD)(i + 1);
        if (!UpdateResourceA(upd, RT_ICON_, MAKEINTRESOURCEA(rid), RES_LANG,
                             blob, (DWORD)img_len)) {
            fprintf(stderr, "embedres: UpdateResource(RT_ICON,%u) failed (%lu)\n",
                    rid, GetLastError());
            failed = 1;
        }
        free(blob);

        GRPICONDIRENTRY *g = (GRPICONDIRENTRY *)(group + 6 + 14 * i);
        g->bWidth      = e->bWidth;
        g->bHeight     = e->bHeight;
        g->bColorCount = e->bColorCount;
        g->bReserved   = 0;
        g->wPlanes     = e->wPlanes;
        g->wBitCount   = e->wBitCount;
        g->dwBytesInRes = e->dwBytesInRes;
        g->nID         = rid;
    }

    if (!failed) {
        if (!UpdateResourceA(upd, RT_GROUP_ICON_, MAKEINTRESOURCEA(1), RES_LANG,
                             group, (DWORD)group_len)) {
            fprintf(stderr, "embedres: UpdateResource(RT_GROUP_ICON) failed (%lu)\n",
                    GetLastError());
            failed = 1;
        }
    }
    free(group);

    if (!EndUpdateResourceA(upd, failed)) {
        fprintf(stderr, "embedres: EndUpdateResource failed (%lu)\n", GetLastError());
        return 1;
    }
    if (failed) return 1;

    printf("embedres: %d icons written into %s\n", count, argv[1]);
    return 0;
}
