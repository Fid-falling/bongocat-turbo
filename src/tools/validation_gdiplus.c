#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "validation_image.h"

#include <stdlib.h>
#include <string.h>

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

enum {
    GDIP_OK = 0,
    GDIP_PIXEL_FORMAT_32BPP_ARGB = 0x26200A,
    GDIP_COMPOSITING_SOURCE_OVER = 0,
    GDIP_COMPOSITING_SOURCE_COPY = 1,
    GDIP_INTERPOLATION_HIGH_QUALITY_BICUBIC = 7,
    GDIP_PIXEL_OFFSET_HIGH_QUALITY = 2,
    GDIP_UNIT_PIXEL = 2
};

static int image_size_ok(int width, int height) {
    return width > 0 && height > 0 &&
        (size_t)width <= SIZE_MAX / ((size_t)height * 4);
}

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
