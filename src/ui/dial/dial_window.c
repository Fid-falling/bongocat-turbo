/*
 * 菜单窗口的建立与释放 (本文件) 与模态循环 (dial_loop.c) 分开, 一半是为了守住单文件
 * 500 行 (cmake/CheckLines.cmake), 一半是因为两者的失败语义完全不同: 这里失败 =
 * "这个窗口用不了" (可以换成 GL 重建), 循环里失败 = "这一帧没端出去" (可以先重试)。
 */
#include "dial_internal.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"
#include "ui_backend.h"
#include "bongo_cat/log.h"
#include "bongo_cat/memory.h"
#include <stdlib.h>
#ifdef _WIN32
/* 代理分层呈现(仅 Windows)。头文件在 src/platform/windows 下, 非 Windows 构建不会
   走到这一行, 所以不需要那里的 include 路径。 */
#include "windows_layered.h"
#endif

bool SDLCALL dial_collect_event(void *userdata, SDL_Event *event) {
    Dial *d = userdata;
    if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_TERMINATING) {
        d->done = true; return true; /* Let the main application handle shutdown. */
    }
    SDL_Window *window = SDL_GetWindowFromEvent(event);
    bool key = event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP;
    /*
     * 代理分层呈现(仅 Windows + GL)下键盘焦点不在菜单窗口上: 抢走焦点的是"真实窗口"
     * 被压成全透明之后仍然可见的宠物窗口 —— 代理窗口带 WS_EX_NOACTIVATE, 点它不会把
     * 焦点交给菜单, 于是 SDL 把按键事件的 windowID 标成宿主(owner)窗口。
     * 菜单是模态的: 宿主的主循环此刻正被这里的模态循环挡着, 这些按键除了菜单没有第二
     * 个消费者, 所以必须收进来 —— 否则 ESC/方向键/回车全部失灵(鼠标却是好的), 而
     * 失焦关闭那条逻辑也已经被代理模式特意跳过(见 dial_input.c)。
     * 只认 key 且只认 owner: 别的宠物/设置窗口的事件照旧一个都不碰。
     */
    bool owner_keys = d->layered && key && window == d->owner;
    if (window != d->window && !owner_keys) {
        if ((event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event->common.timestamp >= d->input_after_ns) ||
            (window == d->owner && event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)) d->done = true;
        return true; /* Never consume another pet's/preferences/input events. */
    }
    bool input = (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST) ||
        event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event->type == SDL_EVENT_MOUSE_BUTTON_UP || event->type == SDL_EVENT_MOUSE_WHEEL ||
        event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP;
    if (!input || d->event_count == 128) return true;
    if (owner_keys)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[probe] 轮盘收到宿主窗口的按键: key=0x%x down=%d (代理模式下键盘焦点在宠物"
            "窗口, 见 dial_collect_event 的说明)",
            (unsigned)event->key.key, event->key.down);
    d->events[d->event_count++] = *event;
    return false;
}

void dial_preview(Dial *d, BongoCatMenuAction action) {
    if (!d->labels->preview) return;
    /* Vulkan 菜单没有 GL 上下文: 预览回调在主窗口自己的上下文里跑, 不用切换。 */
    if (!d->vk) {
        if (d->previous_window && d->previous_context)
            SDL_GL_MakeCurrent(d->previous_window,d->previous_context);
    }
    d->labels->preview(d->labels->preview_userdata,action);
    if (!d->vk && !SDL_GL_MakeCurrent(d->window,d->context)) d->done = true;
}
/* 建菜单窗口。返回 false 表示"这个窗口用不了": 调用方 (dial_loop.c) 在 Vulkan 模式下
   会换 OpenGL 整窗重建, 所以这里失败前必须把自己建了一半的东西留成可释放的状态。 */
bool dial_window_create(Dial *d) {
    float layout = 1;
    bongo_cat_ui_query_window_scale(d->owner,&layout,NULL);
    SDL_Rect bounds;
    if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(d->owner),&bounds)) return false;
    float side = fminf(608*740.0f/560*layout,.96f*(float)SDL_min(bounds.w,bounds.h));
    int size = (int)ceilf(side);
    if (size < 1) return false;
    float x = 0, y = 0;
    if (SDL_GetMouseFocus() == d->owner) SDL_GetMouseState(&x,&y);
    else {
        int width, height;
        if (!SDL_GetWindowSize(d->owner,&width,&height)) return false;
        x = (float)width/2; y = (float)height/2;
    }
    /*
     * 菜单窗口的 Vulkan 通路: 纹理表、绘制、封面、字体图集与窗口生命周期都已
     * 落地, 所以在 renderer=vulkan 时启用。施工图: docs/vulkan-dial-port-recipe.md
     *
     * force_opengl 是回退开关: 上一次用 Vulkan 开这个菜单失败了 (建窗/首帧/连续提交
     * 失败), 这一次直接用 GL —— 窗口的图形 API 标志在创建时就定死了, 所以"换后端"
     * 只能换窗口, 不能换上下文。
     */
    const bool dial_vulkan_enabled = true;
    const bool dial_vulkan = dial_vulkan_enabled && !d->force_opengl &&
        bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN;
    SDL_WindowFlags flags = (dial_vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) |
        SDL_WINDOW_TRANSPARENT | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    const char *driver = SDL_GetCurrentVideoDriver();
    d->popup = driver && SDL_strcmp(driver,"wayland") == 0;
    if (d->popup) {
        /* Wayland requires compositor-managed popup placement and input grabs. */
        d->window = SDL_CreatePopupWindow(d->owner,(int)x-size/2,(int)y-size/2,
            size,size,flags | SDL_WINDOW_POPUP_MENU);
    } else {
        /* A desktop pet may be nonactivating or fully transparent. A native
           child popup inherits its focus/stacking restrictions on Win32.
           An independent SDL utility window owns its focus and GL surface. */
        d->window = SDL_CreateWindow("BongoCat - Menu",size,size,flags |
            SDL_WINDOW_BORDERLESS | SDL_WINDOW_UTILITY | SDL_WINDOW_ALWAYS_ON_TOP);
        if (d->window) {
            SDL_GetGlobalMouseState(&x,&y);
            SDL_Point position = {(int)x,(int)y};
            SDL_DisplayID display = SDL_GetDisplayForPoint(&position);
            SDL_GetDisplayUsableBounds(display,&bounds);
            int left = SDL_clamp((int)x-size/2,bounds.x,SDL_max(bounds.x,bounds.x+bounds.w-size));
            int top = SDL_clamp((int)y-size/2,bounds.y,SDL_max(bounds.y,bounds.y+bounds.h-size));
            SDL_SetWindowPosition(d->window,left,top);
        }
    }
    if (!d->window) return false;
    d->window_id = SDL_GetWindowID(d->window);
    if (dial_vulkan) {
        /*
         * 复用主窗口的 instance/device/queue, 只建自己的交换链。
         *
         * 独立建第二套设备在注入了叠加层钩子的环境里会崩: RTSS (微星小飞机 OSD) /
         * OBS graphics-hook / NVIDIA 叠加层普遍假定单设备, 第一帧走到
         * vkQueuePresentKHR 里就炸 —— 设置窗口当初就是这个原因改成共享设备的
         * (见 preferences_scale.c 的注释)。bongo_cat_vk_current() 返回的正是主
         * 上下文 (设置窗口的不会被登记)。
         */
        BongoCatError error = {0};
        BongoCatVk *owner = bongo_cat_vk_current();
        d->vk_attempted = true;
        d->vk = owner ? bongo_cat_vk_create_shared(d->window, owner, &error)
                      : bongo_cat_vk_create(d->window, &error);
        if (!d->vk) {
            /* 失败原因塞进 SDL 错误串: 上层那条 "Radial menu initialization failed: %s"
               否则会打出一个空的冒号 (SDL_GetError 在纯 Vulkan 失败路径上是空的)。 */
            SDL_SetError("Radial menu Vulkan context: %s",
                error.message[0] ? error.message : "unknown Vulkan error");
            return false;
        }
        /* 菜单是模态的: 呈现不等垂直回扫, 展开/收起的动画更顺 (同设置窗口) */
        bongo_cat_vk_set_vsync(d->vk, false);
    } else {
    /*
     * 轮盘的逐像素透明依赖"帧缓冲带 alpha 通道": 代理呈现提交的是 glReadPixels 抓
     * 到的整帧 (ULW_ALPHA), 没有 alpha 通道时读回来的 alpha 恒为 1.0, 菜单背景就是
     * 一块不透明黑底。SDL_GL_*Attribute 是进程级全局状态, 别的窗口(设置窗口)建上下
     * 文时可能刚把它改成 0, 所以这里显式要 8; 建完连同共享属性一起还原, 不给下一个
     * 窗口留下副作用。
     */
    int sharing = 0, alpha_size = 8;
    SDL_GL_GetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT,&sharing);
    SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE,&alpha_size);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,8);
    if (!SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT,0)) return false;
    d->context = SDL_GL_CreateContext(d->window);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT,sharing);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,alpha_size);
    if (!d->context || !SDL_GL_MakeCurrent(d->window,d->context)) return false;
    SDL_GL_SetSwapInterval(0); /* The modal tick supplies a 16 ms frame budget. */
    } /* dial_vulkan ? ... : GL */
    if (!SDL_GetWindowSize(d->window,&d->width,&d->height) ||
        !SDL_GetWindowSizeInPixels(d->window,&d->pixel_width,&d->pixel_height) ||
        d->width < 1 || d->height < 1) return false;
    d->scale = (float)SDL_min(d->width,d->height)/608;
    d->raster_scale = (float)d->pixel_width/(float)d->width;
    /* 轮盘按窗口坐标布局, 这一行把逻辑/像素/窗口尺寸一起打出来好对账 */
    bongo_cat_ui_log_scale("轮盘", d->window);
#ifdef _WIN32
    /*
     * GL 模式下的逐像素透明: 轮盘是独立窗口、没有自己的 BongoCatPlatform, 拿不到主窗口
     * 那份呈现器(它是按窗口持有的实例), 所以这里给它单独挂一份 —— 否则主窗口透明了,
     * 菜单背景仍然是恒定的黑(普通窗口的 GL 缓冲 alpha 会被 DWM 忽略)。
     * Vulkan 有自己的 alpha 合成, 开关关闭时一律保持原样(照旧 SDL_GL_SwapWindow)。
     */
    if (!dial_vulkan && bongo_cat_windows_layered_requested(d->gl_transparency))
        d->layered = bongo_cat_windows_layered_window_attach(d->window,d->owner,true);
#endif
    if (!dial_paint_init(d)) return false;
    d->opened_at = d->changed_at = d->animation_at = SDL_GetTicks();
    d->opening = .85f;
    d->input_after_ns = SDL_GetTicksNS();
    if (!dial_paint_frame(d) || !SDL_ShowWindow(d->window)) return false;
    /*
     * 代理模式下隐藏窗口不提交帧(见 bongo_cat_windows_layered_window_present 的返回值
     * 说明), 所以上面那一帧并没有真正显示出去; 显示之后必须立刻补一帧, 否则真实窗口会
     * 以未呈现的内容停留一帧 —— 用户看到的是黑方块闪一下。
     */
    if (d->layered && !dial_paint_frame(d)) return false;
    if (!d->popup) SDL_RaiseWindow(d->window);
#ifdef _WIN32
    /* 真正显示画面的是代理窗口, 它得跟着一起抬到最前。 */
    if (d->layered) bongo_cat_windows_layered_window_raise(d->layered);
#endif
    d->dirty = true;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "Radial menu opened: driver=%s window=%u size=%dx%d popup=%d",
        driver ? driver : "unknown",(unsigned)d->window_id,d->width,d->height,d->popup);
    return true;
}
