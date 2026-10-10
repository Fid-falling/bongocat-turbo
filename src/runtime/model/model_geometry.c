/* 模型切换时的窗口几何适配: 内容区的宽高比必须跟渲染用的参考投影一致, 否则换模型会拉伸画面;
 * 这里按新模型的参考比例重排窗口, 并尽量让"宠物本体"停在原来的屏幕位置上 (窗口尺寸变了,
 * 位置要跟着补偿)。由 model_files.c 在渲染选项与 live2d 尺寸就绪后调用; 返回 true = 几何真的变了,
 * 调用方需要 SDL_SyncWindow 并重取像素尺寸。
 */
#include "model_geometry.h"

#include <SDL3/SDL.h>

/* 换模型前先记下当前内容区的屏幕中心。内容区在窗口内可能带 Live2D 溢出留白, 所以要用帧边距
 * left/top 加 content/2, 而不是窗口中心; valid=false 表示拿不到窗口或几何, 调用方应放弃保持位置。
 */
BongoCatModelContentAnchor bongo_cat_model_content_anchor(BongoCatApp *app) {
    BongoCatModelContentAnchor anchor = {0};
    int window_x = 0, window_y = 0;
    if (!app || !app->window ||
        !SDL_GetWindowPosition(app->window, &window_x, &window_y)) return anchor;
    int left = 0, top = 0, ignored_width = 0, ignored_height = 0;
    int content_width = app->session.window.content_width > 0
        ? app->session.window.content_width : app->session.window.width;
    int content_height = app->session.window.content_height > 0
        ? app->session.window.content_height : app->session.window.height;
    if (!bongo_cat_window_frame_size(app, content_width, content_height,
            &ignored_width, &ignored_height, &left, &top)) return anchor;
    anchor.x = window_x + left + content_width / 2;
    anchor.y = window_y + top + content_height / 2;
    anchor.valid = true;
    return anchor;
}

/* 按参考宽高比重排窗口: 内容高沿用会话里记下的内容尺寸 (没记过就退回当前窗口高), 宽度按
 * reference 比例算, 两者都夹在 [64, 8192]; 参考尺寸取 Mver 授权投影的 reference_* 或 Live2D
 * 画布尺寸, 都拿不到时退回 612x354。返回 true = 真的应用了几何, false = 无需改动或失败。
 */
bool bongo_cat_model_apply_aspect(BongoCatApp *app,
    const BongoCatLive2DRenderOptions *options,
    const BongoCatModelContentAnchor *anchor, bool replacing_model) {
    if (!app || !app->window) return false;
    int reference_width = 612;
    int reference_height = 354;
    if (options && options->mver_projection) {
        reference_width = options->reference_width;
        reference_height = options->reference_height;
    } else {
        int canvas_width = 0, canvas_height = 0;
        if (bongo_cat_live2d_canvas_size(app->live2d,
            &canvas_width, &canvas_height)) {
            reference_width = canvas_width;
            reference_height = canvas_height;
        }
    }
    int x, y, width, height;
    if (reference_width <= 0 || reference_height <= 0 ||
        !SDL_GetWindowPosition(app->window, &x, &y) ||
        !SDL_GetWindowSize(app->window, &width, &height)) return false;
    int content_height = app->session.window.content_height > 0
        ? app->session.window.content_height : height;
    int content_width = (int)((double)content_height * reference_width /
        reference_height + 0.5);
    if (content_width < 64) content_width = 64;
    if (content_width > 8192) content_width = 8192;
    if (content_height < 64) content_height = 64;
    if (content_height > 8192) content_height = 8192;
    int next_width = 0, next_height = 0, left = 0, top = 0;
    if (!bongo_cat_window_frame_size(app, content_width, content_height,
            &next_width, &next_height, &left, &top)) return false;
    /* 尺寸已经吻合 (启动恢复, 或重新选中同一个模型): 按锚点重算会把用户摆好的位置挪走。 */
    bool restored_frame = !replacing_model &&
        SDL_abs(width - next_width) <= 1 && SDL_abs(height - next_height) <= 1;
    int next_x = x, next_y = y;
    if (anchor && anchor->valid && !restored_frame) {
        next_x = anchor->x - left - content_width / 2;
        next_y = anchor->y - top - content_height / 2;
    }
    app->session.window.content_width = content_width;
    app->session.window.content_height = content_height;
    if (next_x == x && next_y == y && next_width == width &&
        next_height == height) return false;
    bool changed = bongo_cat_window_apply_geometry(app, next_x, next_y,
        app->session.window.scale_percent, next_width, next_height);
    if (changed) {
        app->session.window.content_width = content_width;
        app->session.window.content_height = content_height;
    }
    return changed;
}
