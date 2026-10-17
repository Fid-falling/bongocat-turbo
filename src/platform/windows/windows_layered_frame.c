#include "windows_layered_internal.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stb_image_resize2.h>
#include <stdlib.h>
#include <windows.h>

/*
 * 代理分层呈现的"抓帧 + 提交"部分: 把 GL 前缓冲拷进自己的 DIB, 再整帧交给 DWM。
 * 窗口的创建/样式/模式切换在 windows_layered.c, 这里只管像素。
 *
 * 为什么单独一个文件: windows_layered.c 顶着 500 行策略上限, 而这一段是自成一体的
 * 一条数据通路, 与窗口生命周期没有耦合。
 *
 * 整条链路是"预乘 alpha": glReadPixels 取回 BGRA, 缩放用 STBIR_BGRA_PM(告诉重采样
 * 器数据已预乘), UpdateLayeredWindow 用 ULW_ALPHA(要求预乘)—— 三者必须一致, 任何
 * 一环改成直通 alpha, 透明边缘就会出现黑边或亮边。
 */

/* 删除 DIB 之前必须先 SelectObject 换回原位图 —— 还选在 DC 里的位图不能删。
   同时清掉 pixels/width/height, 让 resize_bitmap 下次必定重建。
   销毁路径与"尺寸变了"都走它, 所以对外可见(在 windows_layered.c 的 destroy 里用)。 */
void bongo_cat_windows_layered_release_frame(BongoCatWindowsLayered *value) {
    if (!value || !value->bitmap) return;
    SelectObject(value->memory_dc, value->original_bitmap);
    DeleteObject(value->bitmap);
    value->bitmap = NULL;
    value->original_bitmap = NULL;
    value->pixels = NULL;
    value->width = value->height = 0;
}

/* 尺寸没变就复用现有 DIB。biHeight 用正值 = 自下而上的行序, 与 glReadPixels 的
   输出方向一致, 因此拷进去之后不需要翻转。 */
static bool resize_bitmap(BongoCatWindowsLayered *value, int width, int height) {
    if (value->bitmap && value->width == width && value->height == height) return true;
    bongo_cat_windows_layered_release_frame(value);
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = NULL;
    value->bitmap = CreateDIBSection(value->memory_dc, &info,
        DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!value->bitmap || !pixels) {
        if (value->bitmap) DeleteObject(value->bitmap);
        value->bitmap = NULL;
        return false;
    }
    value->original_bitmap = SelectObject(value->memory_dc, value->bitmap);
    value->pixels = pixels;
    value->width = width;
    value->height = height;
    return true;
}

/*
 * 抓取 GL 前缓冲。渲染尺寸与窗口尺寸相同时直接读进 pixels; 不同时(高 DPI 缩放、
 * supersample)先读进 readback 再缩放 —— readback_valid 就是"这一帧走了缩放路径"
 * 的标记, 逐像素 alpha 查询要靠它决定读哪块内存。
 *
 * 缩放那条分支明显更贵(多一次全幅拷贝 + 重采样), 所以只在尺寸真不一致时走; 实测
 * 常见配置下窗口矩形与像素尺寸相等(DIAL/主窗口都是 DPI 感知的), 走的是直读。
 */
static bool read_frame(BongoCatWindowsLayered *value, int width, int height,
    int display_width, int display_height) {
    value->source_width = width; value->source_height = height;
    value->readback_valid = width != display_width || height != display_height;
    if (!value->readback_valid) {
        glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, value->pixels);
        return true;
    }
    size_t bytes = (size_t)width * (size_t)height * 4;
    if (width <= 0 || height <= 0 ||
        bytes / 4 != (size_t)width * (size_t)height) return false;
    if (value->readback_capacity < bytes) {
        unsigned char *next = realloc(value->readback, bytes);
        if (!next) return false;
        value->readback = next;
        value->readback_capacity = bytes;
    }
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, value->readback);
    return stbir_resize_uint8_linear(value->readback, width, height, width * 4,
        value->pixels, display_width, display_height, display_width * 4,
        STBIR_BGRA_PM) != NULL;
}

/* 抓一帧进内存 DC。present 里的"抓不动就算这一帧失败"由调用方负责。 */
bool bongo_cat_windows_layered_capture(BongoCatWindowsLayered *value,
    int width, int height, int display_width, int display_height) {
    return resize_bitmap(value, display_width, display_height) &&
        read_frame(value, width, height, display_width, display_height);
}

/* 把一行 RGBA -> BGRA 拷到 dst, dst_pitch 为单字节步幅。只换 R/B, 不碰 alpha:
   与 GL 路径一致, ULW_ALPHA 要求调用者喂的就是"渲染出来的那种 alpha 语义"
   (预乘), 这里不多做一次预乘, 避免把半透明边缘压成 a^2。 */
static void swap_rgba_row(const unsigned char *src, unsigned char *dst,
    int width, int pitch) {
    for (int x = 0; x < width; ++x) {
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = src[x * 4 + 3];
    }
    (void)pitch;
}

/*
 * Vulkan 专用抓帧: 把整帧回读 (RGBA, 第 0 行为视觉上方) 换进内存 DC。
 *
 * 与 read_frame 对齐但方向相反: 回读是 RGBA 且自下而上翻转 (Vulkan 图像第 0 行在
 * 视觉上方), DIB 是底上方的 BGRA —— 所以除了 R/B 交换还要把行翻转回去。缩放分支与
 * GL 一致 (先翻进 readback, 再用 STBIR_BGRA_PM 重采样到 pixels), 约定已在 read_frame
 * 上方说明; 命中测试走 Vulkan 自己的 vk_hit_alpha (vk_readback.c), 不走这里的
 * frame_alpha, 所以 readback_valid 恒为 false。
 */
static bool read_frame_rgba(BongoCatWindowsLayered *value, int width, int height,
    int display_width, int display_height, const unsigned char *rgba) {
    value->source_width = width; value->source_height = height;
    if (!resize_bitmap(value, display_width, display_height)) return false;
    value->readback_valid = false;
    if (width <= 0 || height <= 0 || !rgba) return false;
    if (display_width == width && display_height == height) {
        for (int y = 0; y < height; ++y)
            swap_rgba_row(rgba + (size_t)y * width * 4u,
                value->pixels + (size_t)(height - 1 - y) * width * 4u,
                width, width * 4);
        return true;
    }
    size_t bytes = (size_t)width * (size_t)height * 4u;
    if (bytes / 4 != (size_t)width * (size_t)height) return false;
    if (value->readback_capacity < bytes) {
        unsigned char *next = realloc(value->readback, bytes);
        if (!next) return false;
        value->readback = next;
        value->readback_capacity = bytes;
    }
    for (int y = 0; y < height; ++y)
        swap_rgba_row(rgba + (size_t)y * width * 4u,
            value->readback + (size_t)(height - 1 - y) * width * 4u,
            width, width * 4);
    return stbir_resize_uint8_linear(value->readback, width, height, width * 4,
        value->pixels, display_width, display_height, display_width * 4,
        STBIR_BGRA_PM) != NULL;
}

bool bongo_cat_windows_layered_capture_rgba(BongoCatWindowsLayered *value,
    int width, int height, int display_width, int display_height,
    const unsigned char *rgba) {
    return read_frame_rgba(value, width, height, display_width, display_height, rgba);
}

/*
 * 把"完全透明"的像素抬成 alpha=1, 让整块窗口矩形都参与命中测试(只给模态菜单用)。
 *
 * 为什么需要: 分层窗口的命中测试是逐像素的, alpha=0 的地方鼠标直接穿到下面的窗口。
 * 主窗口要的正是"透明处点穿", 但轮盘是模态菜单 —— 菜单就开在光标处, 猫通常压在下半张,
 * 于是点在扇区外面(菜单的透明区)会打到猫身上: 菜单没关掉, 反而把猫拖走了。Vulkan 那条
 * 路的菜单窗口没有分层, 命中区域本来就是整块矩形, 这里对齐它的行为。
 * 视觉上 1/255 的 alpha 不可见; 预乘 alpha 的不变量也不破(那些像素的 RGB 已经是 0,
 * 满足 RGB <= alpha)。
 */
void bongo_cat_windows_layered_solidify_hit_area(BongoCatWindowsLayered *value) {
    if (!value || !value->pixels || value->width <= 0 || value->height <= 0) return;
    size_t count = (size_t)value->width * (size_t)value->height;
    uint32_t *words = (uint32_t *)value->pixels;
    for (size_t i = 0; i < count; ++i)
        if (!(words[i] & 0xFF000000u)) words[i] |= 0x01000000u;
}

/* 把内存 DC 里的整帧提交给 DWM。destination 用源窗口的屏幕位置, 所以窗口每次移动
   后都要重新提交; blend 里的 SourceConstantAlpha 就是整窗不透明度, 提交成功后记进
   applied_alpha, 好让 set_opacity 跳过"值没变"的重复提交。
   失败细节交给 SDL_SetError, 上层只看到 false 加一条错误信息。 */
bool bongo_cat_windows_layered_update_proxy(BongoCatWindowsLayered *value) {
    HWND source_window = bongo_cat_windows_layered_native_window(value);
    RECT bounds;
    if (!source_window || !value || !value->proxy || !value->bitmap ||
        !GetWindowRect(source_window, &bounds))
        return SDL_SetError("Cannot locate the Windows layered window");
    POINT destination = {bounds.left, bounds.top};
    POINT source = {0, 0};
    SIZE size = {value->width, value->height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0,
        (BYTE)(value->opacity * 255.0f + 0.5f), AC_SRC_ALPHA};
    HDC screen = GetDC(NULL);
    bool presented = screen && UpdateLayeredWindow(value->proxy, screen,
        &destination, &size, value->memory_dc, &source, 0, &blend,
        ULW_ALPHA) != FALSE;
    DWORD failure = presented ? ERROR_SUCCESS : GetLastError();
    if (screen) ReleaseDC(NULL, screen);
    if (!presented) return SDL_SetError(
        "UpdateLayeredWindow failed (%lu)", (unsigned long)failure);
    value->applied_alpha = blend.SourceConstantAlpha;
    value->applied_alpha_valid = true;
    return true;
}

/* 取某个像素的 alpha, 供点击穿透的命中测试使用(见 windows_pointer.c)。
   优先用 readback(最近一次真正提交出去的内容), 只有它还没准备好时才退回 pixels ——
   "刚写完但还没提交"的那一刻两者是不同的。
   尺寸不匹配直接返回 false: 拿旧尺寸的 alpha 会算出错误的穿透区域。 */
bool bongo_cat_windows_layered_frame_alpha(const BongoCatWindowsLayered *value,
    int width, int height, int x, int y, uint8_t *alpha) {
    const unsigned char *pixels = value && value->readback_valid ?
        value->readback : value ? value->pixels : NULL;
    if (!value || !value->active || !value->has_frame || !pixels ||
        value->source_width != width || value->source_height != height ||
        !alpha || x < 0 || y < 0 || x >= width || y >= height)
        return false;
    *alpha = pixels[((size_t)y * (size_t)width + (size_t)x) * 4 + 3];
    return true;
}
#endif
