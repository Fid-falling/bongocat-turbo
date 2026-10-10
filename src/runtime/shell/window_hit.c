/* 命中测试与点击穿透: 判断指针下的像素是否不透明, 据此决定窗口接不接鼠标事件。采样来源
 * 按后端分叉 —— Vulkan 取整帧回读缓存里的 alpha (有一帧延迟), OpenGL 读前/后台缓冲,
 * 存在交互快照时问预览窗口。
 * 调用时序: 指针移动时 mouse_pipeline 会调 capture_pointer_hit 立即采样, 输入路径用
 * schedule_pointer_hit 做 8ms 防抖, 每帧呈现后用 schedule_hit_check 排 100ms 兜底。
 * 易改错: 入参是窗口内逻辑坐标 (不是屏幕坐标, 也不是像素坐标); 判定不出来时必须偏向
 * "可点击", 否则桌宠会变成点不到。
 */
#include "runtime.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"

#include <SDL3/SDL_opengl.h>

/* 该点是否为不透明像素 (true = 应该把鼠标事件交给窗口)。有交互快照时问快照: 拖动或滚轮
   缩放期间真实窗口的表面可能还没跟上。窗口尺寸取不到时返回 false, 交由调用方按穿透处理。 */
bool bongo_cat_window_visible_at_pointer(BongoCatApp *app, float x, float y) {
    if (app->window_snapshot) return bongo_cat_window_snapshot_hit(app, x, y);
    int width, height, pixel_width, pixel_height;
    if (!SDL_GetWindowSize(app->window, &width, &height) ||
        !SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height) ||
        width <= 0 || height <= 0 || pixel_width <= 0 || pixel_height <= 0) return false;
    if (app->settings.window.rounded_corners) {
        float radius = SDL_min(pixel_width, pixel_height) *
            SDL_clamp(app->settings.window.corner_radius_percent, 0.0f, 50.0f) / 100.0f;
        float px = x * pixel_width / width;
        float py = y * pixel_height / height;
        float dx = SDL_max(SDL_fabsf(px - pixel_width * 0.5f) -
            (pixel_width * 0.5f - radius), 0.0f);
        float dy = SDL_max(SDL_fabsf(py - pixel_height * 0.5f) -
            (pixel_height * 0.5f - radius), 0.0f);
        if (dx * dx + dy * dy > radius * radius) return false;
    }
    /* 下面是 GL 习惯的左下原点坐标 (y 翻转); 平台缓存的已呈现帧也是这个约定, 只有 Vulkan
       分支例外, 它在下面自己换算。 */
    int pixel_x = SDL_clamp((int)(x * pixel_width / width), 0, pixel_width - 1);
    int pixel_y = pixel_height - 1 -
        SDL_clamp((int)(y * pixel_height / height), 0, pixel_height - 1);
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        /* Vulkan 路径没有可读的 GL 前后缓冲: 用渲染帧回读缓存下来的 alpha。
           Vulkan 图像第 0 行在视觉上方, 所以像素 y 不需要翻转。 */
        uint8_t alpha = 0;
        if (bongo_cat_vk_hit_alpha(bongo_cat_vk_current(),
                SDL_clamp((int)(x * pixel_width / width), 0, pixel_width - 1),
                SDL_clamp((int)(y * pixel_height / height), 0, pixel_height - 1),
                &alpha))
            return alpha > 8;
        /* 还没有回读结果 (刚启动或指针在缓存块之外): 先按不透明处理, 宁可多点
           一下, 也不要把窗口变成不可点。 */
        return true;
    }
    /* 先问平台缓存的"已呈现帧" alpha (层叠/代理窗口路径), 拿不到才真的 glReadPixels ——
       后者会打断 GL 流水线, 每帧做一次代价明显。 */
    uint8_t presented_alpha = 0;
    if (bongo_cat_platform_frame_alpha(&app->platform, pixel_width, pixel_height,
        pixel_x, pixel_y, &presented_alpha)) return presented_alpha > 8;
    SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
    if (!SDL_GL_MakeCurrent(app->window, app->gl_context)) return false;
    GLint previous_buffer;
    GLubyte pixel[4] = {0};
    glGetIntegerv(GL_READ_BUFFER, &previous_buffer);
    glReadBuffer(GL_FRONT);
    glReadPixels(pixel_x, pixel_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (pixel[3] <= 8) {
        glReadBuffer(GL_BACK);
        glReadPixels(pixel_x, pixel_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    }
    glReadBuffer((GLenum)previous_buffer);
    if (previous_window && previous_context)
        SDL_GL_MakeCurrent(previous_window, previous_context);
    return pixel[3] > 8;
}

/* 指针移动时的即时采样。不支持动态命中的平台、强制穿透、悬停隐藏或鼠标按下期间都直接
   返回, 把判定留给 sync_click_through; 采样后清掉 dirty 与 deadline, 表示当前结果就是
   最新的。 */
void bongo_cat_window_capture_pointer_hit(BongoCatApp *app) {
    /* Backends without dynamic hit testing would discard the readback, so the
       alpha sample would only stall the pipeline for every presented frame. */
    if (!app || !app->window || !app->pointer_known ||
        !bongo_cat_platform_dynamic_hit_supported() ||
        app->settings.window.pass_through || app->hover_hidden ||
        app->left_mouse_down || app->right_mouse_down) return;
    float local_x, local_y;
    bool inside = bongo_cat_platform_pointer_local(&app->platform,
        app->pointer_x, app->pointer_y, &local_x, &local_y);
    app->pointer_transparent = inside &&
        !bongo_cat_window_visible_at_pointer(app, local_x, local_y);
    app->pointer_hit_dirty = false;
    app->pointer_hit_deadline_ns = 0;
}

/* 几何、可见性或像素内容变化后必须调用, 否则会继续用旧像素的透明判断。deadline 清零 =
   允许立刻采样, 不等调度窗口。 */
void bongo_cat_window_mark_hit_dirty(BongoCatApp *app) {
    if (!app) return;
    app->pointer_hit_dirty = true;
    app->pointer_hit_deadline_ns = 0;
}

/* 隐藏时丢弃交互快照并清掉"首帧待显示"标志; Linux 上要在 unmap 之前记下 XWayland 给的
   位置, 否则重新显示时窗口会跳到别处。显示时恢复最小化/透明度并夹回显示器, 但真正显示由
   渲染路径在首帧提交之后完成 (startup_visibility_pending)。 */
void bongo_cat_window_set_visible(BongoCatApp *app, bool visible) {
    if (!app || !app->window) return;
    app->session.window.visible = visible;
    if (!visible) bongo_cat_window_snapshot_discard(app);
    if (!visible) {
        app->startup_visibility_pending = false;
#if defined(__linux__)
        /* Remember XWayland placement before unmapping the surface. */
        int x = 0, y = 0;
        if (SDL_GetWindowPosition(app->window, &x, &y)) {
            app->session.window.x = x;
            app->session.window.y = y;
            app->session.window.position_known = true;
        }
#endif
        bongo_cat_platform_set_visible(&app->platform, false);
        return;
    }
    if (SDL_GetWindowFlags(app->window) & SDL_WINDOW_MINIMIZED)
        SDL_RestoreWindow(app->window);
    app->window_minimized = false;
    app->hover_hidden = false;
    bongo_cat_app_cancel_hover_fade(app);
    bongo_cat_app_reset_pointer_tracking(app);
    bongo_cat_platform_set_opacity(&app->platform,
        app->session.window.opacity_percent / 100.0f);
#if defined(__linux__)
    /* Restore before clamping so stale off-screen coordinates cannot undo it. */
    if (app->session.window.position_known)
        SDL_SetWindowPosition(app->window, app->session.window.x,
            app->session.window.y);
#endif
    if (app->settings.window.keep_in_screen) bongo_cat_window_clamp_to_display(app);
    else bongo_cat_window_recover_to_display(app);
    /* Keep the native surface hidden until the first complete frame has been
       submitted. The render loop will reveal it next to that presentation. */
    bongo_cat_platform_set_visible(&app->platform,
        !app->startup_visibility_pending);
    bongo_cat_window_mark_hit_dirty(app);
    app->dirty = true;
}

/* 延迟置前: 主循环每轮调用。首帧还没出来时不执行 —— 把一张空白窗口提到最前会把启动黑闪
   暴露给用户; 执行后清掉 due, 只做一次。 */
void bongo_cat_window_raise_when_due(BongoCatApp *app, uint64_t now) {
    if (!app || !app->startup_raise_due_ns ||
        app->startup_visibility_pending || now < app->startup_raise_due_ns) return;
    app->startup_raise_due_ns = 0;
    bongo_cat_window_set_visible(app, true);
    bongo_cat_platform_raise_window(app->window);
}

/* 输入路径的防抖调度: 8ms 内的多次移动合并成一次采样。已有待办时只把 deadline 提前 (取更
   早的那个), 不会把排好的检查推后。 */
void bongo_cat_window_schedule_pointer_hit(BongoCatApp *app) {
    if (!app) return;
    uint64_t deadline = SDL_GetTicksNS() + 8000000ull;
    if (!app->pointer_hit_dirty) {
        app->pointer_hit_dirty = true;
        app->pointer_hit_deadline_ns = deadline;
    } else if (app->pointer_hit_deadline_ns &&
        app->pointer_hit_deadline_ns > deadline) {
        app->pointer_hit_deadline_ns = deadline;
    }
}

/* 每帧呈现后的惰性兜底: 100ms 后重判一次, 让慢速移动或悬停停住时也能收敛; 已经有待办就
   不重复安排。 */
void bongo_cat_window_schedule_hit_check(BongoCatApp *app) {
    if (!app || app->pointer_hit_dirty || !app->pointer_known) return;
    app->pointer_hit_dirty = true;
    app->pointer_hit_deadline_ns = SDL_GetTicksNS() + 100000000ull;
}

/* 把"是否穿透"下发给平台。调用点分散在状态可能变化之后 (呈现后、鼠标输入后、可见性变化
   后), 但只有 forced 或 pointer_transparent 与上次不同才真的调平台接口 —— Windows 上每次
   调用都要重设窗口样式。鼠标按下期间冻结判定 (拖动中窗口突然变透明会丢事件);
   click_through_* 三个字段同时充当下一次的比较基准。 */
void bongo_cat_window_sync_click_through(BongoCatApp *app) {
    if (!app || !app->window) return;
    bool forced = app->settings.window.pass_through || app->hover_hidden;
    if (forced && app->window_snapshot) bongo_cat_window_snapshot_end(app);
    if (!forced && !bongo_cat_platform_dynamic_hit_supported()) {
        app->pointer_transparent = false;
        app->pointer_hit_dirty = false;
    }
    if (!forced && (app->left_mouse_down || app->right_mouse_down)) return;
    if (!forced && app->session.window.visible && app->pointer_known &&
        app->pointer_hit_dirty &&
        (!app->pointer_hit_deadline_ns || SDL_GetTicksNS() >= app->pointer_hit_deadline_ns)) {
        float local_x, local_y;
        bool inside = bongo_cat_platform_pointer_local(&app->platform,
            app->pointer_x, app->pointer_y, &local_x, &local_y);
        app->pointer_transparent = inside && !bongo_cat_window_visible_at_pointer(app,
            local_x, local_y);
        app->pointer_hit_dirty = false;
        app->pointer_hit_deadline_ns = 0;
    }
    bool pointer_transparent = !forced && app->pointer_known && app->pointer_transparent;
    bool forced_changed = !app->click_through_valid ||
        app->click_through_forced_applied != forced;
    if (app->click_through_valid && !forced_changed &&
        app->click_through_applied == pointer_transparent) return;
    bongo_cat_platform_set_click_through(&app->platform, forced, pointer_transparent);
    app->click_through_applied = pointer_transparent;
    app->click_through_forced_applied = forced;
    app->click_through_valid = true;
    if (forced_changed) app->dirty = true;
}

/* 渲染前统一处理 resize 事件攒下的像素尺寸。滚轮手势中只做 reshape, 不重建渲染目标 ——
   手势期间尺寸每帧都在变。resize_render_target_pending 表示"尺寸没变但渲染目标要重建",
   这时不清 model_pointer_anchor_ready (归一化注视锚点仍然有效)。 */
void bongo_cat_window_apply_pending_resize(BongoCatApp *app) {
    if (!app) return;
    if (app->window_snapshot) return;
    if (app->wheel_animation_active) {
        if (!app->resize_pending) return;
        app->resize_pending = false;
        app->resize_render_target_pending = true;
        /* Keep the normalized gaze anchor stable throughout the gesture. */
        bongo_cat_live2d_reshape(app->live2d,
            app->resize_pixel_width, app->resize_pixel_height);
        return;
    }
    if (!app->resize_pending && !app->resize_render_target_pending) return;
    app->resize_pending = false;
    if (!app->resize_render_target_pending)
        app->model_pointer_anchor_ready = false;
    app->resize_render_target_pending = false;
    bongo_cat_live2d_resize(app->live2d,
        app->resize_pixel_width, app->resize_pixel_height);
    bongo_cat_window_mark_hit_dirty(app);
}
