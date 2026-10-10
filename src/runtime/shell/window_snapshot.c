/* 交互预览 (Windows 快照窗口): 拖动与滚轮缩放期间把上一帧画面交给一个原生预览窗口跟着指针
 * 走, 预览的创建/销毁与指针同步全部归本文件所有; 非 Windows 平台与 Vulkan 后端退化为实时
 * 渲染 (快照需要 GL 前缓冲的 glReadPixels)。
 * 契约: begin 可重复调用 (只刷新 idle 期限), end 会先补画一帧真实画面, discard 不画;
 * app->window_snapshot 非空即表示预览有效, 其它模块只读这个指针。
 */
#include "runtime.h"
#include "bongo_cat/log.h"
#include "bongo_cat/render.h"

#ifdef _WIN32
#include "windows_snapshot.h"
#endif

/* 没有输入后预览的最长存活时间; 到点由 snapshot_update 结束预览, 回到实时渲染。 */
#define SNAPSHOT_IDLE_NS 180000000ull

/* 进入交互 (拖动/滚轮缩放) 时申请预览; 已经有预览就只刷新 idle 期限。snapshot_blocked 是
   粘滞标志 (Vulkan 没有 GL 上下文, 或者创建失败), 只有 idle 到期后由 update 清除, 免得
   每次交互都白试一遍。创建前必须先渲染一帧 (快照抓的是当前前台像素), 所以这里会临时切换
   GL 上下文, 并在返回前恢复。 */
void bongo_cat_window_snapshot_begin(BongoCatApp *app) {
    if (!app) return;
    app->snapshot_deadline_ns = SDL_GetTicksNS() + SNAPSHOT_IDLE_NS;
    if (app->window_snapshot || app->snapshot_blocked) return;
#ifdef _WIN32
    /*
     * 快照 (windows_snapshot_image.c 的 glReadPixels) 需要 GL 上下文, Vulkan 后端
     * 没有: 拖动/缩放时退回实时渲染。这里显式标记不可用, 免得每次交互都白试一遍。
     */
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        if (!app->snapshot_blocked)
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[runtime] 交互快照在 Vulkan 下不可用, 使用实时渲染");
        app->snapshot_blocked = true;
        return;
    }
    if (!app->window || !app->live2d || !app->session.window.visible ||
        app->startup_visibility_pending || app->window_minimized || app->hover_hidden ||
        app->hover_fade_active ||
        app->settings.window.pass_through ||
        !(SDL_GetWindowFlags(app->window) & SDL_WINDOW_TRANSPARENT)) return;
    SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
    if (!SDL_GL_MakeCurrent(app->window, app->gl_context)) return;
    /* Capture a complete, up-to-date frame before changing native geometry. */
    bongo_cat_app_render_now(app);
    if (!app->render_retry_ns) {
        app->window_snapshot = bongo_cat_windows_snapshot_create(app->window,
            app->platform.window_opacity);
        if (!app->window_snapshot) {
            app->snapshot_blocked = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Interaction snapshot unavailable; using live rendering: %s", SDL_GetError());
        }
    }
    SDL_GetGlobalMouseState(&app->snapshot_pointer_x, &app->snapshot_pointer_y);
    if (previous_window && previous_context)
        SDL_GL_MakeCurrent(previous_window, previous_context);
#endif
}

/* 不渲染直接释放 (隐藏、关机、呈现失败); 释放后标脏并让命中缓存失效, 下一帧用真实像素
   重新判定透明。没有预览时什么都不做。 */
void bongo_cat_window_snapshot_discard(BongoCatApp *app) {
    if (!app || !app->window_snapshot) return;
#ifdef _WIN32
    bongo_cat_windows_snapshot_destroy(app->window_snapshot);
#endif
    app->window_snapshot = NULL;
    app->last_frame_ns = SDL_GetTicksNS();
    app->dirty = true;
    bongo_cat_window_mark_hit_dirty(app);
}

/* 结束交互: 先补画一帧真实画面再销毁原生预览窗口, 否则用户会看到预览消失后的空白 —— 与
   discard 的唯一区别就是这个渲染。结束后把 snapshot_blocked 置回 true: 一次交互只建一次
   预览窗口, 连续交互不会反复创建/销毁原生窗口。 */
void bongo_cat_window_snapshot_end(BongoCatApp *app) {
    if (!app || !app->window_snapshot) return;
    void *snapshot = app->window_snapshot;
    app->window_snapshot = NULL;
    app->snapshot_blocked = true;
    app->last_frame_ns = SDL_GetTicksNS();
    app->dirty = true;
    /* Keep the preview visible until the final real frame is submitted. */
    SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
    bongo_cat_app_render_now(app);
#ifdef _WIN32
    bongo_cat_windows_snapshot_destroy(snapshot);
#else
    (void)snapshot;
#endif
    if (previous_window && previous_context)
        SDL_GL_MakeCurrent(previous_window, previous_context);
    bongo_cat_window_mark_hit_dirty(app);
}

/* 主循环每轮调用。结束条件: 窗口不可见、最小化、穿透、悬停隐藏, 指针离开起点 2px (用平方
   比较), 或 idle 到期。没有预览时它只负责在 idle 到期后解除 snapshot_blocked; 拖动中鼠标
   已松开也会在这里兜底结束拖动 (收不到 BUTTON_UP 的场合)。 */
void bongo_cat_window_snapshot_update(BongoCatApp *app, uint64_t now) {
    if (!app) return;
    if (!app->window_snapshot) {
        if (!app->window_drag_active && !app->wheel_animation_active &&
            now >= app->snapshot_deadline_ns) app->snapshot_blocked = false;
        return;
    }
#ifdef _WIN32
    bongo_cat_windows_snapshot_pointer(app->window_snapshot);
#endif
    bool finish = !app->session.window.visible || app->window_minimized ||
        app->settings.window.pass_through || app->hover_hidden;
    if (!app->window_drag_active) {
        float x, y;
        SDL_GetGlobalMouseState(&x, &y);
        float dx = x - app->snapshot_pointer_x, dy = y - app->snapshot_pointer_y;
        finish = finish || dx * dx + dy * dy >= 4.0f ||
            (!app->wheel_animation_active && now >= app->snapshot_deadline_ns);
    }
    if (finish) bongo_cat_window_snapshot_end(app);
    else if (app->window_drag_active &&
        !(SDL_GetGlobalMouseState(NULL, NULL) & SDL_BUTTON_LMASK))
        bongo_cat_window_drag_end(app);
}

/* rect 为 NULL 表示跟随原生窗口矩形 (拖动), 否则用子像素矩形 (滚轮缩放要画在半像素上);
   非 Windows 平台是空实现, 窗口状态仍由调用方维护。 */
void bongo_cat_window_snapshot_geometry(BongoCatApp *app, const SDL_FRect *rect) {
#ifdef _WIN32
    if (app) bongo_cat_windows_snapshot_geometry(app->window_snapshot, rect);
#else
    (void)app; (void)rect;
#endif
}

/* 预览呈现失败就当作快照不可用: 置 snapshot_blocked 并丢弃, 下一帧走实时渲染; 成功才清
   dirty, 表示这一帧已经端出去了。 */
void bongo_cat_window_snapshot_present(BongoCatApp *app) {
    if (!app || !app->window_snapshot) return;
#ifdef _WIN32
    if (!bongo_cat_windows_snapshot_present(app->window_snapshot)) {
        app->snapshot_blocked = true;
        bongo_cat_window_snapshot_discard(app);
        return;
    }
#endif
    app->dirty = false;
}

/* 预览期间的命中测试问的是预览窗口的像素 (真实窗口可能还没跟上几何变化); 非 Windows 平台
   恒 false, 调用方会退回真实缓冲。 */
bool bongo_cat_window_snapshot_hit(BongoCatApp *app, float x, float y) {
#ifdef _WIN32
    return app && bongo_cat_windows_snapshot_hit(app->window_snapshot, x, y);
#else
    (void)app; (void)x; (void)y;
    return false;
#endif
}
