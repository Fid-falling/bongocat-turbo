#ifndef BONGO_CAT_WINDOWS_LAYERED_INTERNAL_H
#define BONGO_CAT_WINDOWS_LAYERED_INTERNAL_H

#include "windows_layered.h"

#ifdef _WIN32
#include <windows.h>

/*
 * 分层窗口呈现器的内部状态, 只给同目录的 windows_layered*.c 使用。
 *
 * 为什么自带 window/opacity: 呈现器原先只认 BongoCatPlatform(单实例), 而轮盘是没有
 * 平台结构的独立窗口, 于是它在 GL 模式下永远拿不到逐像素透明。现在每个窗口一份实例,
 * 内部函数只认实例指针, 主窗口与轮盘跑的是同一份代码。opacity 是
 * platform->window_opacity 的镜像(唯一写入口是 windows_layered_state.c 的 set_opacity),
 * 因为"恢复源窗口"和"提交整帧"都要用它, 却不能反向依赖平台结构。
 *
 * 关键字段: pixels 是自己维护的 DIB 像素缓冲(memory_dc / bitmap 与它绑定);
 * readback 是"最近一次提交给 DWM 的内容"的副本, readback_valid 表示它可用 ——
 * 命中测试与逐像素 alpha 都读 readback, 因为提交之后 pixels 很快会被下一帧覆盖。
 * 不变量: active 为真时, pixels / bitmap / memory_dc 三件套一定同时存在;
 *         source_width/source_height 是渲染源的尺寸, width/height 是真正提交给
 *         DWM 的尺寸, 两者可能不相等(有缩放), 不要混用;
 *         实例的生命周期不得超出 window 指向的 SDL 窗口。
 */
typedef struct BongoCatWindowsLayered {
    SDL_Window *window;
    float opacity;
    HDC memory_dc;
    HBITMAP bitmap;
    HGDIOBJ original_bitmap;
    unsigned char *pixels;
    unsigned char *readback;
    size_t readback_capacity;
    HWND proxy;
    /* 代理窗口创建时用的 owner (NULL = 用源窗口自己)。轮盘必须把它设成主窗口的代理
       窗口, 否则两个顶层代理窗口互相压不住: 主窗口那边任何一次置前都会把轮盘的画面
       盖掉, 表现是"菜单看得见却点不到"。原因见 windows_layered_window.c。 */
    HWND proxy_owner;
    /* 是否让整块窗口矩形都参与命中测试(透明像素抬到 alpha=1)。只给模态菜单开:
       主窗口要的是"透明处点穿", 菜单要的是"点哪儿都算菜单的"(见 solidify_hit_area)。 */
    bool solid_hit_area;
    int width, height, source_width, source_height;
    bool readback_valid, active, has_frame, mode_logged;
    bool source_transparent, visible, topmost;
    BYTE applied_alpha;
    bool applied_alpha_valid;
} BongoCatWindowsLayered;

/* SDL 窗口 -> 原生 HWND (实现在 windows_layered.c)。 */
HWND bongo_cat_windows_layered_native_window(const BongoCatWindowsLayered *value);
HWND bongo_cat_windows_layered_window_handle(SDL_Window *window);
/* 切换呈现路径(建/销毁代理窗口 + 压制/恢复源窗口), 幂等且不写诊断日志 ——
   日志只在主窗口的公开入口 bongo_cat_windows_layered_set_click_through 里打一份。 */
void bongo_cat_windows_layered_apply_mode(BongoCatWindowsLayered *value,
    bool enabled);
/* 抓一帧 GL 前缓冲进内存 DC, 以及把 DIB 释放掉 (实现在 windows_layered_frame.c)。 */
bool bongo_cat_windows_layered_capture(BongoCatWindowsLayered *value,
    int width, int height, int display_width, int display_height);
/* Vulkan 专用: 把整帧回读 (RGBA, 第 0 行为视觉上方) 拷进内存 DC (实现在
   windows_layered_frame.c)。内部处理 R/B 交换、垂直翻转与缩放 (如需)。 */
bool bongo_cat_windows_layered_capture_rgba(BongoCatWindowsLayered *value,
    int width, int height, int display_width, int display_height,
    const unsigned char *rgba);
void bongo_cat_windows_layered_release_frame(BongoCatWindowsLayered *value);
/* 透明像素抬到 alpha=1, 让整窗矩形都能被点到(仅模态菜单, 见实现处的说明)。 */
void bongo_cat_windows_layered_solidify_hit_area(BongoCatWindowsLayered *value);
/* 重新提交整帧(实现在 windows_layered_frame.c)。调用方必须是渲染/主线程 ——
   UpdateLayeredWindow 在窗口所属线程上同步生效。 */
bool bongo_cat_windows_layered_update_proxy(BongoCatWindowsLayered *value);
/* 每帧呈现(实现在 windows_layered.c): 未启用代理时退化成 SDL_GL_SwapWindow。 */
bool bongo_cat_windows_layered_present(void *presenter, int width, int height);
/* 逐像素 alpha 查询, 供点击穿透的命中测试使用。 */
bool bongo_cat_windows_layered_frame_alpha(const BongoCatWindowsLayered *value,
    int width, int height, int x, int y, uint8_t *alpha);
#endif

#endif
