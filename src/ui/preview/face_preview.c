/*
 * 面捕预览窗 (独立窗口)。
 *
 * 为什么是独立的 SDL 窗口而不是"设置页里画一块": 用户要的是一个可以拖到一边、单独看摄像头的
 * 窗口 (类似 VTube Studio 的摄像头预览)。它不参与桌宠的渲染循环, 自己一个 SDL_Renderer,
 * 所以不会和 Vulkan 交换链/设置窗口的 UI 上下文纠缠。
 *
 * 数据来源: 采集线程的预览缓冲 (240x180, 加锁快照), 这里只负责把它贴成纹理。
 * 生命周期: 按钮打开 -> 用户点窗口关闭 (或再点一次按钮) 时销毁。
 */
#include "face_preview.h"

#include "runtime.h"

#include "../runtime/input/face_capture.h"

#include "bongo_cat/log.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define PREVIEW_SOURCE_WIDTH 240
#define PREVIEW_SOURCE_HEIGHT 180
#define PREVIEW_WINDOW_WIDTH 480
#define PREVIEW_WINDOW_HEIGHT 360

static SDL_Window *preview_window = NULL;
static SDL_Renderer *preview_renderer = NULL;
static SDL_Texture *preview_texture = NULL;
static unsigned char preview_pixels[PREVIEW_SOURCE_WIDTH * PREVIEW_SOURCE_HEIGHT * 3];
static uint32_t preview_generation = 0;

static bool preview_event_watch(void *userdata, SDL_Event *event) {
    (void)userdata;
    /* 用户点预览窗的关闭按钮: 立刻销毁, 不要等下一帧轮询 */
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && preview_window &&
            event->window.windowID == SDL_GetWindowID(preview_window)) {
        bongo_cat_face_preview_close();
    }
    return true;
}

bool bongo_cat_face_preview_open(void) {
    return preview_window != NULL;
}

bool bongo_cat_face_preview_show(BongoCatApp *app) {
    if (preview_window) return true;
    if (!app || !app->face_capture) return false;
    preview_window = SDL_CreateWindow("面捕预览", PREVIEW_WINDOW_WIDTH,
        PREVIEW_WINDOW_HEIGHT, SDL_WINDOW_RESIZABLE);
    if (!preview_window) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[face] 预览窗创建失败: %s",
            SDL_GetError());
        return false;
    }
    preview_renderer = SDL_CreateRenderer(preview_window, NULL);
    if (!preview_renderer) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[face] 预览窗渲染器创建失败: %s",
            SDL_GetError());
        bongo_cat_face_preview_close();
        return false;
    }
    preview_texture = SDL_CreateTexture(preview_renderer,
        SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, PREVIEW_SOURCE_WIDTH,
        PREVIEW_SOURCE_HEIGHT);
    if (!preview_texture) {
        bongo_cat_face_preview_close();
        return false;
    }
    preview_generation = 0;
    SDL_AddEventWatch(preview_event_watch, NULL);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 预览窗已打开");
    return true;
}

void bongo_cat_face_preview_close(void) {
    if (!preview_window) return;
    SDL_RemoveEventWatch(preview_event_watch, NULL);
    if (preview_texture) SDL_DestroyTexture(preview_texture);
    if (preview_renderer) SDL_DestroyRenderer(preview_renderer);
    SDL_DestroyWindow(preview_window);
    preview_texture = NULL;
    preview_renderer = NULL;
    preview_window = NULL;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 预览窗已关闭");
}

/* 每帧调用一次: 有新帧才更新纹理, 然后重绘并呈现。 */
void bongo_cat_face_preview_tick(BongoCatApp *app) {
    if (!preview_window || !preview_texture) return;
    /* 面捕被关掉/组件卸载: 预览窗跟着关, 不留一个永远定格的窗口 */
    if (!app || !app->face_capture) {
        bongo_cat_face_preview_close();
        return;
    }
    uint32_t generation = 0;
    const bool has_frame = bongo_cat_face_capture_preview(app->face_capture,
        preview_pixels, PREVIEW_SOURCE_WIDTH, PREVIEW_SOURCE_HEIGHT, &generation);
    if (has_frame && generation != preview_generation) {
        if (SDL_UpdateTexture(preview_texture, NULL, preview_pixels,
                PREVIEW_SOURCE_WIDTH * 3))
            preview_generation = generation;
    }
    SDL_SetRenderDrawColor(preview_renderer, 18, 18, 20, 255);
    SDL_RenderClear(preview_renderer);
    /* 等比居中: 摄像头常见 4:3, 窗口可缩放 */
    int width = 0, height = 0;
    SDL_GetWindowSize(preview_window, &width, &height);
    const float scale = SDL_min((float)width / (float)PREVIEW_SOURCE_WIDTH,
        (float)height / (float)PREVIEW_SOURCE_HEIGHT);
    const SDL_FRect destination = {
        ((float)width - PREVIEW_SOURCE_WIDTH * scale) * 0.5f,
        ((float)height - PREVIEW_SOURCE_HEIGHT * scale) * 0.5f,
        PREVIEW_SOURCE_WIDTH * scale, PREVIEW_SOURCE_HEIGHT * scale
    };
    SDL_RenderTexture(preview_renderer, preview_texture, NULL, &destination);
    SDL_RenderPresent(preview_renderer);
}
