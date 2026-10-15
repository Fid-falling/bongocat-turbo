#include "vk_internal.h"
#include "bongo_cat/live2d_vulkan.h"
#include "runtime.h"
#include "bongo_cat/log.h"
#include "bongo_cat/overlay.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"
#include "bongo_cat/model.h"

#include <SDL3/SDL.h>

/*
 * Vulkan 后端的主窗口帧。
 *
 * 绘制顺序与 OpenGL 路径 (app_loop.c 的 render()) 严格一致, 这样两个后端看到
 * 的是同一幅画面:
 *   清屏 -> 叠加层背景 -> 鼠标指针(按键之下) -> 叠加层按键 -> 特效 -> 指针(之上)
 *
 * 与后端无关的状态推进 (输入、动画、命中测试调度) 仍由 app_loop 统一驱动。
 * 当前进度: 设备/交换链/呈现/叠加层与指针绘制已落地; 圆角遮罩与逐像素命中
 * 测试回读属于后续阶段。
 */


/*
 * Live2D (Cubism) 的 Vulkan 渲染器把模型画进自己的离屏图像, 这里把它包成纹理,
 * 供宠物窗口的合成使用。尺寸变化或目标消失时重建; 销毁走后端的退休队列, 避免
 * 描述符还在飞行中就被回收。
 */
static BongoCatVkTexture *live2d_target_texture(BongoCatApp *app) {
    static BongoCatVkTexture *texture;
    static int width, height;
    void *image = NULL, *view = NULL;
    int target_width = 0, target_height = 0;
    if (!app || !app->vk ||
        !bongo_cat_live2d_vulkan_target_info(&image, &view, NULL, &target_width,
            &target_height)) {
        if (texture && app && app->vk) bongo_cat_vk_texture_destroy(app->vk, texture);
        texture = NULL;
        width = height = 0;
        return NULL;
    }
    if (texture && target_width == width && target_height == height) return texture;
    if (texture) bongo_cat_vk_texture_destroy(app->vk, texture);
    BongoCatError error = {0};
    texture = bongo_cat_vk_texture_from_external(app->vk, image, view, target_width,
        target_height, "live2d", &error);
    width = target_width;
    height = target_height;
    return texture;
}

/*
 * 把 Live2D 离屏结果合成到主帧。
 *
 * 镜像/垂直翻转已经由投影阶段完成 (build_projection 的 mirror_ 与
 * reflect_vertical_flip, 由 set_mirror/set_vertical_flip 下发), 与 GL 路径的
 * bongo_cat_live2d_draw 是同一套语义。所以这里合成四边形时**绝不能**再传 mirror
 * 给 draw_overlay_quad: 那等于把身体翻第二次, 净效果是身体不动、而同帧的键盘/
 * 背景却只翻一次, 身体和键盘就错开了。这个"合成不重复翻转"是固定不变式, 集中
 * 在这里, 调用点就不用关心 mirror 该怎么传。
 *
 * premultiply 传 false: SDK 的 Vulkan 渲染器输出的就是预乘像素 (贴图在
 * cubism_vulkan_texture.cpp 里已按预乘上传), 片元里再乘一次 alpha 会把半透明
 * 边缘压成 a^2 (GL 与 VK 的 alpha 覆盖一致、只有半透明区偏暗的那个 bug)。
 */
static void draw_live2d_body(BongoCatApp *app) {
    if (!bongo_cat_live2d_vulkan_ready()) return;
    BongoCatVkTexture *live2d = live2d_target_texture(app);
    if (!live2d) return;
    bongo_cat_vk_draw_overlay_quad(app->vk, live2d, false,
        live2d->width, live2d->height, false, false, false);
}

bool bongo_cat_vk_render_main(BongoCatApp *app, bool present) {
    if (!app || !app->vk || !app->window) return false;
    /*
     * 离屏截帧 (模型封面): Vulkan 没有可读的 GL 前后缓冲, 改成"照常渲染这一帧,
     * 另外把整帧入队回读"。这一帧仍然 present (消费 acquire, 画面与普通帧一致),
     * 像素要到下一次复用帧槽时才拿得到 (vk_readback.c 的一帧延迟语义), 于是封面
     * 分两帧完成: 这一帧排队, 下一帧 take 到结果再写 PNG。
     */
    const bool cover_capture = !present;
    uint64_t now = SDL_GetTicksNS();
    if (app->render_retry_ns > now) return false;
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(app->window, &width, &height);
    if (width <= 0 || height <= 0) return false;

    float rgba[4];
    bongo_cat_window_background_rgba(app, rgba);
    bongo_cat_vk_set_clear_color(app->vk, rgba[0], rgba[1], rgba[2], rgba[3]);
    /*
     * 交换链先跟上窗口尺寸, 再画 Live2D。
     *
     * Live2D 的离屏目标是按交换链尺寸重建的 (见 cubism_vulkan_bridge.cpp 的
     * ensure_target_size), 而交换链本来要到下面的 begin_frame 才重建 —— 那样
     * 改"缩放"后的第一帧 Sdk 拿到的还是旧尺寸, 画面会先错一帧。这里提前重建,
     * 尺寸没变时是空操作 (bongo_cat_vk_resize 自己比 extent)。
     */
    bongo_cat_vk_resize(app->vk, width, height);
    /*
     * Live2D 先画到它自己的离屏目标: SDK 的 Vulkan 渲染器会自行提交命令并
     * 等待完成, 所以必须在开始录制本帧之前做, 不能和我们的命令缓冲交错。
     */
    /*
     * 这里必须无条件画一次: 模型的渲染器是惰性创建的 (GL 时代是因为需要当前
     * 上下文), 由这次 draw 触发。之前用 bongo_cat_live2d_vulkan_ready() 把它
     * 挡住, 就变成"不 ready 不画, 不画永远不 ready"的死锁 —— 桥也就永远挂不上。
     */
    bongo_cat_live2d_set_mirror(app->live2d, app->settings.model.mirror);
    bongo_cat_live2d_set_vertical_flip(app->live2d,
        app->settings.model.vertical_flip);
    bongo_cat_live2d_draw(app->live2d);
    if (!bongo_cat_vk_begin_frame(app->vk, width, height)) {
        app->dirty = true;
        app->render_retry_ns = now + 1000000000ull;
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 帧开始失败");
        return false;
    }

    /*
     * 叠加层的视口: 与 OpenGL 路径逐行对齐 (app_loop.c 的 render())。
     *
     * 背景图/按键/特效/指针都是按 NDC 画的整屏四边形, GL 那边它们被画在
     * "模型内容视口" 里 (glViewport(content_x, content_y, content_w, content_h)),
     * 只有 Live2D 自己按整窗视口画 —— 因为它的投影里已经带了内容变换。这里过去
     * 一直用整窗视口画叠加层, 于是背景/键盘图比模型偏上、缩放也不同, 看起来就是
     * "猫的位置不对"。
     */
    int content_x = 0, content_y = 0;
    int content_width = width, content_height = height;
    const bool content_viewport = bongo_cat_live2d_viewport(app->live2d,
        &content_x, &content_y, &content_width, &content_height) &&
        content_width > 0 && content_height > 0;
    if (content_viewport)
        /* GL 的视口 y 从左下角算, Vulkan 从左上角算, 这里翻转 */
        bongo_cat_vk_set_viewport(app->vk, content_x,
            height - content_y - content_height, content_width, content_height);
    bongo_cat_overlay_draw_background(app->overlay, app->settings.model.mirror);
    bongo_cat_vk_set_viewport(app->vk, 0, 0, width, height);
    /*
     * Live2D 的离屏结果按整窗四边形合成。reference 传纹理自己的像素尺寸:
     * (image_size / reference) 在顶点着色器里就是"这张图按多大铺开", 传自身尺寸
     * 得到 1.0, 整张目标正好铺满窗口。超采样之后目标是窗口的 2 倍 (见
     * cubism_vulkan_bridge.cpp 的 kSupersample), 于是这里正好做一次线性缩小 ——
     * 抗锯齿就是靠这一步 + 2 倍分辨率渲染。
     */
    if (bongo_cat_live2d_vulkan_ready())
        draw_live2d_body(app);
    if (content_viewport)
        bongo_cat_vk_set_viewport(app->vk, content_x,
            height - content_y - content_height, content_width, content_height);
    bongo_cat_overlay_draw_pointer_before_keys(app->overlay);
    bongo_cat_overlay_draw_keys(app->overlay, app->settings.model.mirror);
    bongo_cat_overlay_draw_effect(app->overlay, app->settings.model.mirror);
    bongo_cat_overlay_draw_pointer_after_keys(app->overlay);
    bongo_cat_vk_set_viewport(app->vk, 0, 0, width, height);

    /* 圆角遮罩必须在所有内容之后: 它把 RGB 与 alpha 一起乘上覆盖率 */
    if (app->settings.window.rounded_corners &&
        app->settings.window.corner_radius_percent > 0.0f)
        bongo_cat_vk_mask_corners(app->vk, width, height,
            SDL_clamp(app->settings.window.corner_radius_percent, 0.0f, 50.0f));

    /* 命中测试: 需要采样时把指针附近的小块 alpha 拷回主机, 下一次绘制前取回 */
    if (app->pointer_hit_dirty && app->pointer_known) {
        float local_x = 0.0f, local_y = 0.0f;
        int window_width = 0, window_height = 0, pixel_width = 0, pixel_height = 0;
        if (bongo_cat_platform_pointer_local(&app->platform, app->pointer_x,
                app->pointer_y, &local_x, &local_y) &&
            SDL_GetWindowSize(app->window, &window_width, &window_height) &&
            SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height) &&
            window_width > 0 && window_height > 0 &&
            pixel_width > 0 && pixel_height > 0) {
            /* Vulkan 图像的第 0 行在视觉上方, 与 window_hit.c 的 GL 约定相反 */
            bongo_cat_vk_queue_hit_sample(app->vk,
                (int)(local_x * pixel_width / window_width),
                (int)(local_y * pixel_height / window_height));
        }
    }

    /* 冒烟诊断: 与封面共用同一条整帧回读 (present 前入队, 下一帧 collect) */
    if (app->smoke && !cover_capture &&
        !bongo_cat_vk_queue_frame_sample(app->vk, 0, 0, width, height))
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 冒烟整帧回读入队失败");

    /* 封面帧: 整帧入队, 结果由下一帧的 take_frame_sample 取走 */
    if (cover_capture &&
        !bongo_cat_vk_queue_frame_sample(app->vk, 0, 0, width, height))
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 封面整帧回读入队失败 (%dx%d)", width, height);

    app->render_retry_ns = 0;
    bool presented = bongo_cat_vk_end_frame(app->vk);
    if (!presented) {
        app->dirty = true;
        app->render_retry_ns = now + 1000000000ull;
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 帧提交/呈现失败");
        return false;
    }
    /* 首帧已经送到合成器之后才显示窗口, 与 OpenGL 路径一致: 不让用户看到一张
       还没画过的空白表面。 */
    bool reveal_startup = app->startup_visibility_pending &&
        app->session.window.visible;
    if (reveal_startup) {
        bongo_cat_platform_set_visible(&app->platform, true);
        app->startup_visibility_pending = false;
    }
    app->input_diagnostics.presented_frames++;
    app->dirty = false;
    bongo_cat_startup_ready(app);
    bongo_cat_memory_policy_frame_presented();
    bongo_cat_window_sync_click_through(app);
    bongo_cat_window_schedule_hit_check(app);
    /* 与 OpenGL 路径同一处: 冒烟时统计首帧/帧序列并落盘 (app->smoke 门控) */
    bongo_cat_frame_audit(app, width, height);
    /* 帧节奏探针: 每端出一帧记一次。放在这里而不是 app_loop 里, 是因为这条 Vulkan 帧
     * 路径也会被离屏截帧 (模型封面) 之外的调用点触发, 只有在这里才"真的端出去了一帧"。 */
    bongo_cat_frame_interval_probe(app, "vulkan-present");
    return true;
}
