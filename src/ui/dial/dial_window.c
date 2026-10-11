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

static bool SDLCALL collect_event(void *userdata, SDL_Event *event) {
    Dial *d = userdata;
    if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_TERMINATING) {
        d->done = true; return true; /* Let the main application handle shutdown. */
    }
    SDL_Window *window = SDL_GetWindowFromEvent(event);
    if (window != d->window) {
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

static void animate(Dial *d, uint64_t now) {
    float elapsed = fminf(.1f,(float)(now-d->animation_at)/1000);
    d->animation_at = now;
    float t = fminf(1,(float)(now-d->opened_at)/700);
    float opening = .85f+.15f*(1-powf(1-t,4));
    if (opening != d->opening) { d->opening = opening; d->dirty = true; }
    float response = 1-expf(-17*elapsed);
    for (int i = 0; i < d->count; ++i) {
        if (d->child_focus && i != d->active) { d->lift[i] = 0; continue; }
        float target = d->active == i ? 1.0f : 0.0f;
        float next = d->lift[i]+(target-d->lift[i])*response;
        if (fabsf(next-target) < .002f) next = target;
        if (next != d->lift[i]) { d->lift[i] = next; d->dirty = true; }
    }
    if (dial_child_count(d) && now-d->changed_at <=
        (uint64_t)(DIAL_REVEAL_DURATION_MS +
            (dial_child_count(d)-1)*DIAL_REVEAL_DELAY_MS + 16))
        d->dirty = true;
    if (now-d->opened_at <= (uint64_t)(DIAL_REVEAL_DURATION_MS +
        (d->count-1)*DIAL_REVEAL_DELAY_MS + 16)) d->dirty = true;
}

static bool create(Dial *d) {
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
     */
    const bool dial_vulkan_enabled = true;
    const bool dial_vulkan = dial_vulkan_enabled &&
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
        d->vk = owner ? bongo_cat_vk_create_shared(d->window, owner, &error)
                      : bongo_cat_vk_create(d->window, &error);
        if (!d->vk) return false;
        /* 菜单是模态的: 呈现不等垂直回扫, 展开/收起的动画更顺 (同设置窗口) */
        bongo_cat_vk_set_vsync(d->vk, false);
    } else {
    int sharing = 0;
    SDL_GL_GetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT,&sharing);
    if (!SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT,0)) return false;
    d->context = SDL_GL_CreateContext(d->window);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT,sharing);
    if (!d->context || !SDL_GL_MakeCurrent(d->window,d->context)) return false;
    SDL_GL_SetSwapInterval(0); /* The modal tick supplies a 16 ms frame budget. */
    } /* dial_vulkan ? ... : GL */
    if (!SDL_GetWindowSize(d->window,&d->width,&d->height) ||
        !SDL_GetWindowSizeInPixels(d->window,&d->pixel_width,&d->pixel_height) ||
        d->width < 1 || d->height < 1) return false;
    d->scale = (float)SDL_min(d->width,d->height)/608;
    d->raster_scale = (float)d->pixel_width/(float)d->width;
#ifdef _WIN32
    /*
     * GL 模式下的逐像素透明: 轮盘是独立窗口、没有自己的 BongoCatPlatform, 拿不到主窗口
     * 那份呈现器(它是按窗口持有的实例), 所以这里给它单独挂一份 —— 否则主窗口透明了,
     * 菜单背景仍然是恒定的黑(普通窗口的 GL 缓冲 alpha 会被 DWM 忽略)。
     * Vulkan 有自己的 alpha 合成, 开关关闭时一律保持原样(照旧 SDL_GL_SwapWindow)。
     */
    if (!dial_vulkan && bongo_cat_windows_layered_requested())
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

BongoCatMenuAction bongo_cat_platform_context_menu(BongoCatPlatform *platform,
    const BongoCatMenuLabels *labels) {
    if (!platform || !platform->window || !labels) return BONGO_CAT_MENU_NONE;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,"Radial menu requested: owner=%u",
        (unsigned)SDL_GetWindowID(platform->window));
    Dial *d = calloc(1,sizeof(*d));
    if (!d) return BONGO_CAT_MENU_NONE;
    d->owner = platform->window; d->labels = labels; d->dark = labels->dark_theme;
    d->previous_window = SDL_GL_GetCurrentWindow();
    d->previous_context = SDL_GL_GetCurrentContext();
    d->active = d->child = d->pressed = -1;
    dial_items(d);
    bool ready = create(d);
    if (!ready) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"Radial menu initialization failed: %s",SDL_GetError());
    while (ready && !d->done) {
        uint64_t start = SDL_GetTicks();
        ++d->frames;
        SDL_PumpEvents();
        if (!SDL_GetWindowFromID(d->window_id)) { d->done = true; break; }
        d->event_count = 0;
        SDL_FilterEvents(collect_event,d);
        for (int i = 0; i < d->event_count && !d->done; ++i) {
            dial_event(d,&d->events[i]);
            if (d->done) SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "Radial menu dismissed: event=0x%x action=%d elapsed_ms=%llu",
                (unsigned)d->events[i].type,(int)d->result,
                (unsigned long long)(SDL_GetTicks()-d->opened_at));
        }
        if (d->done) break;
        if (labels->preview_tick) {
            if (!d->vk && d->previous_window && d->previous_context)
                SDL_GL_MakeCurrent(d->previous_window,d->previous_context);
            labels->preview_tick(labels->preview_userdata);
        }
        if (!d->vk && !SDL_GL_MakeCurrent(d->window,d->context)) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO,"Radial menu context activation failed: %s",SDL_GetError());
            break;
        }
        dial_covers_tick(d);
        animate(d,SDL_GetTicks());
        if (d->dirty) {
            d->dirty = false;
            if (!dial_paint_frame(d)) {
                SDL_LogError(SDL_LOG_CATEGORY_VIDEO,"Radial menu rendering failed: %s",SDL_GetError());
                break;
            }
        }
        uint64_t elapsed = SDL_GetTicks()-start;
        if (elapsed < 16) SDL_Delay((Uint32)(16-elapsed));
    }
    /*
     * 帧率诊断。frames 是模态循环的迭代率, presents 才是真正回读整窗并提交给 DWM 的次数
     * —— 代理分层呈现下这两个数会明显分离(静止的菜单 presents 不再增长), 这正是帧率问题
     * 的第一手证据, 出问题时先看这一行。
     */
    if (ready) {
        uint64_t total = SDL_GetTicks()-d->opened_at;
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "Radial menu loop: frames=%d presents=%d elapsed_ms=%llu fps=%.1f",
            d->frames,d->presents,(unsigned long long)total,
            total ? 1000.0*(double)d->frames/(double)total : 0.0);
    }
    if (d->vk || !d->context || (SDL_GetWindowFromID(d->window_id) &&
        SDL_GL_MakeCurrent(d->window,d->context))) dial_paint_free(d);
    else {
        /* A destroyed window cannot safely own GL deletion calls. Its context
           releases GPU allocations; release only CPU allocations here. */
        if (d->paint.atlas.permanent.alloc) nk_font_atlas_clear(&d->paint.atlas);
        for (int i = 0; i < 4; ++i) free(d->paint.ranges[i]);
        free(d->paint.vertices);
    }
    /* 交换链/表面必须在窗口销毁之前释放。 */
    if (d->vk) bongo_cat_vk_destroy(d->vk);
#ifdef _WIN32
    /* 同理, 呈现器也要在窗口销毁之前拆: 代理窗口是菜单窗口的 owned window, 会跟着
       owner 一起消失, 之后就没人能恢复真实窗口的样式与属性了。 */
    if (d->layered) bongo_cat_windows_layered_destroy(d->layered);
#endif
    if (d->context) SDL_GL_DestroyContext(d->context);
    if (d->window_id && SDL_GetWindowFromID(d->window_id)) SDL_DestroyWindow(d->window);
    if (d->previous_window && d->previous_context)
        SDL_GL_MakeCurrent(d->previous_window,d->previous_context);
    if (labels->restore) labels->restore(labels->preview_userdata,d->result);
    BongoCatMenuAction result = d->result;
    free(d);
    /* Trim only after GL/CPU teardown and preview restoration have finished. */
    bongo_cat_platform_trim_memory();
    return result;
}
