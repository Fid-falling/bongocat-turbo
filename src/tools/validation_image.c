/*
 * 验证工具的图像 I/O 与基础图像处理(WIC 负责编解码; 读取与绘制在
 * validation_gdiplus.c 里)。这里只提供上游工具真正需要的几件事: 存图、释放、
 * 只保留最大前景连通块、求前景包围盒。
 *
 * 颜色约定: 32 位 BGRA, 每行紧密排列。所有"前景"判定都准备了两套: 图带 alpha
 * 时用 alpha, 否则与采样到的背景色比对 —— 带透明通道和纯色背景两种截图都要能
 * 处理。
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

#include "validation_image.h"

#include <stdlib.h>
#include <string.h>

static void release_unknown(IUnknown *value) {
    if (value) IUnknown_Release(value);
}

/* 尺寸校验同时挡住两种坑: 非正的尺寸, 以及 width*height*4 会溢出 size_t 的
   畸形图。 */
static int image_size_ok(int width, int height) {
    return width > 0 && height > 0 &&
        (size_t)width <= SIZE_MAX / ((size_t)height * 4);
}

/* 用 WIC 编码: png 为真存 PNG(保留 alpha), 否则存 BMP。逐级创建 COM 对象,
   任何一步失败都跳到同一个 done 统一释放; result 只有走到最后一次 Commit 成功
   才为真 —— "编码器建好了"并不等于文件写成功了。 */
int bongo_cat_validation_image_save(const wchar_t *path,
    const BongoCatValidationImage *image, int png) {
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    const GUID *container = png ? &GUID_ContainerFormatPng :
        &GUID_ContainerFormatBmp;
    GUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr;
    int result = 0;
    if (!path || !image || !image_size_ok(image->width, image->height) ||
        !image->bgra) return 0;
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr)) goto done;
    hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (FAILED(hr)) goto done;
    hr = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (FAILED(hr)) goto done;
    hr = IWICImagingFactory_CreateEncoder(factory, container, NULL, &encoder);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream,
        WICBitmapEncoderNoCache);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, NULL);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_Initialize(frame, NULL);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_SetSize(frame, (UINT)image->width,
        (UINT)image->height);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &pixel_format);
    if (FAILED(hr) || !IsEqualGUID(&pixel_format, &GUID_WICPixelFormat32bppBGRA))
        goto done;
    hr = IWICBitmapFrameEncode_WritePixels(frame, (UINT)image->height,
        (UINT)image->width * 4, (UINT)((size_t)image->width * image->height * 4),
        image->bgra);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapFrameEncode_Commit(frame);
    if (FAILED(hr)) goto done;
    hr = IWICBitmapEncoder_Commit(encoder);
    result = SUCCEEDED(hr);
done:
    release_unknown((IUnknown *)frame);
    release_unknown((IUnknown *)encoder);
    release_unknown((IUnknown *)stream);
    release_unknown((IUnknown *)factory);
    return result;
}

void bongo_cat_validation_image_free(BongoCatValidationImage *image) {
    if (!image) return;
    free(image->bgra);
    memset(image, 0, sizeof(*image));
}

/* "接近白/灰"的像素不算前景: 237 这个阈值是为了滤掉截图里的浅灰底和抗锯齿
   边缘, 让连通块尽量贴合真正的角色轮廓。 */
static int foreground_pixel(const unsigned char *pixel) {
    return min(pixel[0], min(pixel[1], pixel[2])) < 237;
}

/*
 * 只保留面积最大的前景连通块, 其余前景涂成白色(而不是透明): 后续存成 BMP 时
 * 白色更容易被人眼和脚本区分出来。
 * 两处特别处理:
 *   - 落在最大连通块包围盒之外、或"青色占比超过一成"的块会被抹掉 —— 青色是验证
 *     场景里的标记色(见下面 32/220/220 那组判断), 一定不是角色本体;
 *   - 连通性用 4 邻域并显式排除左右换行处, 否则 x=0 会和上一行行尾被错误连通。
 * 洪水填充用数组队列而不是递归, 大图也不会爆栈。
 */
int bongo_cat_validation_image_keep_largest(BongoCatValidationImage *image) {
    size_t size, index;
    int *labels = NULL, *queue = NULL, *counts = NULL, *cyan = NULL;
    int label = 0, best_label = 0, best_count = 0;
    int best_left = 0, best_top = 0, best_right, best_bottom;
    if (!image || !image->bgra || !image_size_ok(image->width, image->height))
        return 0;
    size = (size_t)image->width * image->height;
    labels = (int *)calloc(size, sizeof(*labels));
    queue = (int *)malloc(size * sizeof(*queue));
    counts = (int *)calloc(size + 1, sizeof(*counts));
    cyan = (int *)calloc(size + 1, sizeof(*cyan));
    if (!labels || !queue || !counts || !cyan) goto fail;
    best_right = image->width - 1; best_bottom = image->height - 1;
    for (index = 0; index < size; ++index) {
        int head = 0, tail = 0, left = image->width, top = image->height;
        int right = -1, bottom = -1, current;
        if (labels[index] || !foreground_pixel(image->bgra + index * 4)) continue;
        ++label; labels[index] = label; queue[tail++] = (int)index;
        while (head < tail) {
            int x, y, next[4], n;
            current = queue[head++]; x = current % image->width;
            y = current / image->width;
            left = min(left, x); top = min(top, y);
            right = max(right, x); bottom = max(bottom, y);
            if (image->bgra[(size_t)current * 4] < 32 &&
                image->bgra[(size_t)current * 4 + 1] > 220 &&
                image->bgra[(size_t)current * 4 + 2] > 220) ++cyan[label];
            next[0] = current - 1; next[1] = current + 1;
            next[2] = current - image->width; next[3] = current + image->width;
            for (n = 0; n < 4; ++n) {
                int value = next[n];
                if (value < 0 || (size_t)value >= size || labels[value] ||
                    (n == 0 && x == 0) || (n == 1 && x == image->width - 1) ||
                    !foreground_pixel(image->bgra + (size_t)value * 4)) continue;
                labels[value] = label; queue[tail++] = value;
            }
        }
        counts[label] = tail;
        if (tail > best_count) {
            best_count = tail; best_label = label;
            best_left = left; best_top = top; best_right = right;
            best_bottom = bottom;
        }
    }
    for (index = 0; index < size; ++index) {
        int current = labels[index], x = (int)(index % image->width);
        int y = (int)(index / image->width);
        int outside = x < best_left || x > best_right || y < best_top ||
            y > best_bottom;
        int is_cyan = current && cyan[current] * 10 > counts[current];
        if (current && current != best_label && (outside || is_cyan))
            memset(image->bgra + index * 4, 255, 4);
    }
    free(labels); free(queue); free(counts); free(cyan);
    return 1;
fail:
    free(labels); free(queue); free(counts); free(cyan);
    return 0;
}

/*
 * 求前景包围盒, 只算面积最大的那个连通块。背景色取图像上部 1/20 高度处水平
 * 中线上的一个像素 —— 那里通常是纯背景; 同时稀疏采样判断整幅图有没有 alpha,
 * 有 alpha 就改用 alpha 阈值判前景(与 keep_largest 保持同一套策略)。
 * 返回 false 表示图里找不到前景。
 */
int bongo_cat_validation_image_bounds(const BongoCatValidationImage *image,
    BongoCatValidationRect *bounds) {
    size_t size, index;
    unsigned char *foreground = NULL;
    int *queue = NULL, best_count = 0;
    if (!image || !image->bgra || !bounds || !image_size_ok(image->width,
        image->height)) return 0;
    size = (size_t)image->width * image->height;
    foreground = (unsigned char *)calloc(size, 1);
    queue = (int *)malloc(size * sizeof(*queue));
    if (!foreground || !queue) goto fail;
    {
        const unsigned char *background = image->bgra +
            ((size_t)min(image->height - 1, max(1, image->height / 20)) *
                image->width + image->width / 2) * 4;
        int alpha_samples = 0, non_opaque = 0;
        int step_y = max(1, image->height / 32), step_x = max(1, image->width / 32);
        int x, y, alpha;
        for (y = 0; y < image->height; y += step_y)
        for (x = 0; x < image->width; x += step_x) {
            if (image->bgra[((size_t)y * image->width + x) * 4 + 3] < 250)
                ++non_opaque;
            ++alpha_samples;
        }
        alpha = non_opaque >= max(2, alpha_samples / 10);
        for (index = 0; index < size; ++index) {
            const unsigned char *pixel = image->bgra + index * 4;
            int visible = alpha ? pixel[3] > 8 : max(abs((int)pixel[0] -
                background[0]), max(abs((int)pixel[1] - background[1]),
                abs((int)pixel[2] - background[2]))) > 18;
            foreground[index] = (unsigned char)visible;
        }
    }
    bounds->left = image->width; bounds->top = image->height;
    bounds->right = -1; bounds->bottom = -1;
    for (index = 0; index < size; ++index) if (foreground[index]) {
        int head = 0, tail = 0, left = image->width, top = image->height;
        int right = -1, bottom = -1;
        foreground[index] = 0; queue[tail++] = (int)index;
        while (head < tail) {
            int current = queue[head++], x = current % image->width;
            int y = current / image->width, next[4] = {current - 1,
                current + 1, current - image->width, current + image->width};
            int n;
            left = min(left, x); top = min(top, y);
            right = max(right, x); bottom = max(bottom, y);
            for (n = 0; n < 4; ++n) {
                int value = next[n];
                if (value < 0 || (size_t)value >= size || !foreground[value] ||
                    (n == 0 && x == 0) || (n == 1 && x == image->width - 1)) continue;
                foreground[value] = 0; queue[tail++] = value;
            }
        }
        if (tail > best_count) {
            best_count = tail; bounds->left = left; bounds->top = top;
            bounds->right = right + 1; bounds->bottom = bottom + 1;
        }
    }
    free(foreground); free(queue);
    return bounds->right > bounds->left && bounds->bottom > bounds->top;
fail:
    free(foreground); free(queue);
    return 0;
}
