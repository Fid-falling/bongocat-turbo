#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "validation_image.h"

#include <stdlib.h>
#include <string.h>

/*
 * GDI+ 的 C 直连绑定: 手动声明用到的几个导出函数, 而不是包含 <gdiplus.h>。
 *
 * 为什么不直接 #include <gdiplus.h>: 那是 C++ 头(依赖 Gdiplus 命名空间和模板),
 * 而这些工具是纯 C 编译的。这里只声明真正调用到的入口, 顺便把 GDI+ 依赖压到最小。
 * 代价是类型和常量必须自己与官方头文件保持一致(见下面的 enum), 写错没有编译期
 * 检查, 只能靠运行验证。
 */
typedef int GpStatus;
typedef int PixelFormat;
typedef struct GpImage GpImage;
typedef struct GpBitmap GpBitmap;
typedef struct GpGraphics GpGraphics;
typedef struct GpFontFamily GpFontFamily;
typedef struct GpFont GpFont;
typedef struct GpBrush GpBrush;
typedef struct GpSolidFill GpSolidFill;
typedef struct GpRectF { float x, y, width, height; } GpRectF;
typedef struct GdiplusStartupInputC {
    UINT32 version;
    void *debug_callback;
    BOOL suppress_background_thread;
    BOOL suppress_external_codecs;
} GdiplusStartupInputC;

GpStatus WINAPI GdiplusStartup(ULONG_PTR *token,
    const GdiplusStartupInputC *input, void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);
GpStatus WINAPI GdipCreateBitmapFromScan0(INT width, INT height, INT stride,
    PixelFormat format, BYTE *scan0, GpBitmap **bitmap);
GpStatus WINAPI GdipCreateBitmapFromFile(const wchar_t *path, GpBitmap **bitmap);
GpStatus WINAPI GdipGetImageWidth(GpImage *image, UINT *width);
GpStatus WINAPI GdipGetImageHeight(GpImage *image, UINT *height);
GpStatus WINAPI GdipGetImageGraphicsContext(GpImage *image, GpGraphics **graphics);
GpStatus WINAPI GdipGraphicsClear(GpGraphics *graphics, DWORD color);
GpStatus WINAPI GdipSetCompositingMode(GpGraphics *graphics, int mode);
GpStatus WINAPI GdipSetInterpolationMode(GpGraphics *graphics, int mode);
GpStatus WINAPI GdipSetPixelOffsetMode(GpGraphics *graphics, int mode);
GpStatus WINAPI GdipDrawImageRectRect(GpGraphics *graphics, GpImage *image,
    float destination_x, float destination_y, float destination_width,
    float destination_height, float source_x, float source_y,
    float source_width, float source_height, int source_unit,
    void *attributes, void *callback, void *callback_data);
GpStatus WINAPI GdipDrawImageI(GpGraphics *graphics, GpImage *image, int x, int y);
GpStatus WINAPI GdipDeleteGraphics(GpGraphics *graphics);
GpStatus WINAPI GdipDisposeImage(GpImage *image);
GpStatus WINAPI GdipCreateFontFamilyFromName(const wchar_t *name,
    void *collection, GpFontFamily **family);
GpStatus WINAPI GdipDeleteFontFamily(GpFontFamily *family);
GpStatus WINAPI GdipCreateFont(const GpFontFamily *family, float size,
    int style, int unit, GpFont **font);
GpStatus WINAPI GdipDeleteFont(GpFont *font);
GpStatus WINAPI GdipCreateSolidFill(DWORD color, GpSolidFill **brush);
GpStatus WINAPI GdipDeleteBrush(GpBrush *brush);
GpStatus WINAPI GdipDrawString(GpGraphics *graphics, const wchar_t *text,
    int length, const GpFont *font, const GpRectF *layout,
    const void *format, const GpBrush *brush);

/* PixelFormat / CompositingMode 这些数值来自 GDI+ 官方头文件, 必须逐位对齐:
   例如 32BPP_ARGB 的 0x26200A 里还编码了每像素位数等信息。 */
enum {
    GDIP_OK = 0,
    GDIP_PIXEL_FORMAT_32BPP_ARGB = 0x26200A,
    GDIP_COMPOSITING_SOURCE_OVER = 0,
    GDIP_COMPOSITING_SOURCE_COPY = 1,
    GDIP_INTERPOLATION_HIGH_QUALITY_BICUBIC = 7,
    GDIP_PIXEL_OFFSET_HIGH_QUALITY = 2,
    GDIP_UNIT_PIXEL = 2
};

/* 与 validation_image.c 里的同名函数是各写一份的(两份都只在本文件内使用),
   改溢出判据时要记得两处都改。 */
static int image_size_ok(int width, int height) {
    return width > 0 && height > 0 &&
        (size_t)width <= SIZE_MAX / ((size_t)height * 4);
}

/*
 * 读任意格式的图(GDI+ 支持 PNG/JPEG/BMP/GIF...)并统一成 32 位 BGRA。
 * 关键是 SOURCE_COPY 合成模式: 按源像素直接覆盖而不做 alpha 混合, 源图的透明
 * 区域才能原样保留, 否则会被默认的底色混掉。
 * 失败时 image 保持清零状态, 所以调用方可以无条件调用 free。
 */
int bongo_cat_validation_image_load(const wchar_t *path,
    BongoCatValidationImage *image) {
    const GdiplusStartupInputC startup = {1, NULL, FALSE, FALSE};
    ULONG_PTR token = 0;
    GpBitmap *source = NULL, *bitmap = NULL;
    GpGraphics *graphics = NULL;
    UINT width = 0, height = 0;
    unsigned char *pixels = NULL;
    if (!path || !image) return 0;
    memset(image, 0, sizeof(*image));
    if (GdiplusStartup(&token, &startup, NULL) != GDIP_OK ||
        GdipCreateBitmapFromFile(path, &source) != GDIP_OK ||
        GdipGetImageWidth((GpImage *)source, &width) != GDIP_OK ||
        GdipGetImageHeight((GpImage *)source, &height) != GDIP_OK ||
        !image_size_ok((int)width, (int)height)) goto done;
    pixels = (unsigned char *)malloc((size_t)width * height * 4);
    if (!pixels || GdipCreateBitmapFromScan0((int)width, (int)height,
            (int)width * 4, GDIP_PIXEL_FORMAT_32BPP_ARGB, pixels,
            &bitmap) != GDIP_OK ||
        GdipGetImageGraphicsContext((GpImage *)bitmap, &graphics) != GDIP_OK ||
        GdipSetCompositingMode(graphics, GDIP_COMPOSITING_SOURCE_COPY) != GDIP_OK ||
        GdipDrawImageI(graphics, (GpImage *)source, 0, 0) != GDIP_OK) goto done;
    image->width = (int)width; image->height = (int)height;
    image->bgra = pixels; pixels = NULL;
done:
    free(pixels);
    if (graphics) GdipDeleteGraphics(graphics);
    if (bitmap) GdipDisposeImage((GpImage *)bitmap);
    if (source) GdipDisposeImage((GpImage *)source);
    if (token) GdiplusShutdown(token);
    return image->bgra != NULL;
}

/*
 * 把一张截图按 anchor 指定的区域归一化成固定的 800x800 画布: 缩放比例由长边
 * 决定并留出 margin 边距, anchor 的中心对齐画布中心。这样不同分辨率、不同窗口
 * 位置的截图才能逐像素比较 —— 验证工具只关心角色本身的差异。
 * 先清成白底再以 SOURCE_OVER 画上去, 因此带透明的源图会贴在白底上。
 * 用双三次插值配高质量像素偏移, 是为了让缩放后的抗锯齿行为稳定可复现。
 * 失败时 output 会被 free 掉, 不会留下半成品指针。
 */
int bongo_cat_validation_image_normalize_file(const wchar_t *path,
    BongoCatValidationRect anchor, BongoCatValidationImage *output) {
    const int canvas = 800, margin = 40;
    const GdiplusStartupInputC startup = {1, NULL, FALSE, FALSE};
    ULONG_PTR token = 0;
    GpBitmap *source_bitmap = NULL, *output_bitmap = NULL;
    GpGraphics *graphics = NULL;
    UINT source_width = 0, source_height = 0;
    double scale;
    float scale_float, left, top;
    int padding, crop_left, crop_top, crop_right, crop_bottom, ok = 0;
    if (!path || !output || anchor.right <= anchor.left ||
        anchor.bottom <= anchor.top) return 0;
    memset(output, 0, sizeof(*output));
    if (GdiplusStartup(&token, &startup, NULL) != GDIP_OK ||
        GdipCreateBitmapFromFile(path, &source_bitmap) != GDIP_OK ||
        GdipGetImageWidth((GpImage *)source_bitmap, &source_width) != GDIP_OK ||
        GdipGetImageHeight((GpImage *)source_bitmap, &source_height) != GDIP_OK)
        goto done;
    output->width = output->height = canvas;
    output->bgra = (unsigned char *)malloc((size_t)canvas * canvas * 4);
    if (!output->bgra) { memset(output, 0, sizeof(*output)); goto done; }
    scale = min((canvas - 2.0 * margin) / (anchor.right - anchor.left),
        (canvas - 2.0 * margin) / (anchor.bottom - anchor.top));
    scale_float = (float)scale;
    left = (float)((canvas - (anchor.right - anchor.left) * scale) / 2.0 -
        anchor.left * scale);
    top = (float)((canvas - (anchor.bottom - anchor.top) * scale) / 2.0 -
        anchor.top * scale);
    padding = max(4, max(anchor.right - anchor.left,
        anchor.bottom - anchor.top) / 40);
    crop_left = max(0, anchor.left - padding);
    crop_top = max(0, anchor.top - padding);
    crop_right = min((int)source_width, anchor.right + padding);
    crop_bottom = min((int)source_height, anchor.bottom + padding);
    if (GdipCreateBitmapFromScan0(canvas, canvas, canvas * 4,
            GDIP_PIXEL_FORMAT_32BPP_ARGB, output->bgra,
            &output_bitmap) != GDIP_OK ||
        GdipGetImageGraphicsContext((GpImage *)output_bitmap,
            &graphics) != GDIP_OK ||
        GdipGraphicsClear(graphics, 0xffffffffu) != GDIP_OK ||
        GdipSetCompositingMode(graphics, GDIP_COMPOSITING_SOURCE_OVER) != GDIP_OK ||
        GdipSetInterpolationMode(graphics,
            GDIP_INTERPOLATION_HIGH_QUALITY_BICUBIC) != GDIP_OK ||
        GdipSetPixelOffsetMode(graphics, GDIP_PIXEL_OFFSET_HIGH_QUALITY) != GDIP_OK ||
        GdipDrawImageRectRect(graphics, (GpImage *)source_bitmap,
            left + crop_left * scale_float, top + crop_top * scale_float,
            (crop_right - crop_left) * scale_float,
            (crop_bottom - crop_top) * scale_float,
            (float)crop_left, (float)crop_top,
            (float)(crop_right - crop_left), (float)(crop_bottom - crop_top),
            GDIP_UNIT_PIXEL, NULL, NULL, NULL) != GDIP_OK) goto done;
    ok = 1;
done:
    if (graphics) GdipDeleteGraphics(graphics);
    if (output_bitmap) GdipDisposeImage((GpImage *)output_bitmap);
    if (source_bitmap) GdipDisposeImage((GpImage *)source_bitmap);
    if (token) GdiplusShutdown(token);
    if (!ok) bongo_cat_validation_image_free(output);
    return ok;
}

/* 在图上写一行标注文字(比较图里用来标明这是哪一帧)。字体固定 Segoe UI 20px、
   纯黑, 只为人眼可读, 不参与任何度量。 */
int bongo_cat_validation_image_draw_label(BongoCatValidationImage *image,
    const wchar_t *label, float x, float y) {
    const GdiplusStartupInputC startup = {1, NULL, FALSE, FALSE};
    GpRectF layout = {x, y, 0.0f, 0.0f};
    ULONG_PTR token = 0;
    GpBitmap *bitmap = NULL;
    GpGraphics *graphics = NULL;
    GpFontFamily *family = NULL;
    GpFont *font = NULL;
    GpSolidFill *fill = NULL;
    int ok = 0;
    if (!image || !image->bgra || !label) return 0;
    if (GdiplusStartup(&token, &startup, NULL) != GDIP_OK ||
        GdipCreateBitmapFromScan0(image->width, image->height, image->width * 4,
            GDIP_PIXEL_FORMAT_32BPP_ARGB, image->bgra, &bitmap) != GDIP_OK ||
        GdipGetImageGraphicsContext((GpImage *)bitmap, &graphics) != GDIP_OK ||
        GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &family) != GDIP_OK ||
        GdipCreateFont(family, 20.0f, 1, 3, &font) != GDIP_OK ||
        GdipCreateSolidFill(0xff000000u, &fill) != GDIP_OK ||
        GdipDrawString(graphics, label, -1, font, &layout, NULL,
            (GpBrush *)fill) != GDIP_OK) goto done;
    ok = 1;
done:
    if (fill) GdipDeleteBrush((GpBrush *)fill);
    if (font) GdipDeleteFont(font);
    if (family) GdipDeleteFontFamily(family);
    if (graphics) GdipDeleteGraphics(graphics);
    if (bitmap) GdipDisposeImage((GpImage *)bitmap);
    if (token) GdiplusShutdown(token);
    return ok;
}
