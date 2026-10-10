/* "构图是否稳定"的视觉审计: 每个用例都自己清屏画一帧, 读回像素与 BongoCatLive2DVisualState, 往
 * state_root/live2d-visual-audit.csv 追加一行 (fit_scale 与平移、可见包围盒、边缘 alpha、fitted /
 * visible / mver_projection 以及通过位)。核心不变量是: 切表达式、拖动、镜像、改画布尺寸都不应改变
 * fit_scale —— 构图由投影决定, 内容只改参数。边缘像素只进 CSV 不做断言, Vulkan 下记 ~0u 表示"没取到"
 * (需要独立的离屏渲染路径)。结束时恢复拖拽、镜像与画布并置 app->dirty。
 */
#include "runtime.h"
#include "bongo_cat/render.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <math.h>
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>

/* 只推进模型时间, 不渲染也不交换缓冲 —— 与 live2d_viewer_audit.c 里的同名函数行为不同, 这边只有在
 * record() 里才真正画一帧。
 */
static void advance(BongoCatApp *app, int frames) {
    for (int i = 0; i < frames; ++i)
        bongo_cat_live2d_update(app->live2d, 1.0f / 60.0f);
}

/* 1e-4 的容差对应"浮点重算出的同一构图": fit_scale 由窗口尺寸与画布尺寸决定, 内容变化不该动它,
 * 所以可以按近似相等判定。
 */
static bool close_scale(float left, float right) {
    return fabsf(left - right) <= 0.0001f;
}

/* 可见包围盒是否落在 [-1,1] 标准画布内, 允许 8% 溢出 (投影与标定本身有取整误差); visible_min/max 是
 * 投影到 NDC 之后的范围, 不是像素坐标。
 */
static bool inside(const BongoCatLive2DVisualState *state) {
    return state && state->visible_min_x >= -1.08f &&
        state->visible_min_y >= -1.08f && state->visible_max_x <= 1.08f &&
        state->visible_max_y <= 1.08f;
}

/* 读四条 1 像素宽的边并统计 alpha>8 的像素数, 用来发现"模型被裁到只剩边缘"这类构图错误。Vulkan 下没有
 * 可读的 GL 缓冲, 返回 ~0u (CSV 里是 4294967295); 它只进 CSV 不参与通过判定, 所以哨兵值不会污染结果。
 */
static unsigned edge_pixels(int width, int height) {
    /* 该审计靠"自己清屏 + 画一帧"再读像素, Vulkan 需要先有离屏渲染路径 */
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) return ~0u;
    size_t bytes = (size_t)(width > height ? width : height) * 4;
    unsigned char *pixels = malloc(bytes);
    if (!pixels) return ~0u;
    unsigned visible = 0;
    const int rectangles[][4] = {{0, 0, width, 1}, {0, height - 1, width, 1},
        {0, 0, 1, height}, {width - 1, 0, 1, height}};
    for (size_t side = 0; side < 4; ++side) {
        int count = rectangles[side][2] * rectangles[side][3];
        glReadPixels(rectangles[side][0], rectangles[side][1],
            rectangles[side][2], rectangles[side][3], GL_RGBA,
            GL_UNSIGNED_BYTE, pixels);
        for (int i = 0; i < count; ++i)
            if (pixels[(size_t)i * 4 + 3] > 8) visible++;
    }
    free(pixels);
    return visible;
}

/* 自绘一帧、采集状态、追加一行 CSV, 返回这一行是否通过; 调用方必须用 && 累积。contained=false 用于表达式
 * 与动作这类可能故意超出画布的用例, 此时跳过包围盒检查。另外它不恢复 GL 状态 (viewport、scissor 等)。
 */
static bool record(FILE *file, BongoCatApp *app, const char *name,
    bool contained, BongoCatLive2DVisualState *state) {
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(app->window, &width, &height);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f); glClear(GL_COLOR_BUFFER_BIT);
    bongo_cat_live2d_draw(app->live2d);
    unsigned edges = edge_pixels(width, height);
    bool available = bongo_cat_live2d_visual_state(app->live2d, state);
    bool finite = available && isfinite(state->fit_scale) &&
        isfinite(state->fit_translate_x) && isfinite(state->fit_translate_y) &&
        isfinite(state->visible_min_x) && isfinite(state->visible_min_y) &&
        isfinite(state->visible_max_x) && isfinite(state->visible_max_y);
    bool visible = finite && state->visible &&
        state->visible_min_x < state->visible_max_x &&
        state->visible_min_y < state->visible_max_y;
    bool inside = !contained || (visible && state->visible_min_x >= -1.08f &&
        state->visible_min_y >= -1.08f && state->visible_max_x <= 1.08f &&
        state->visible_max_y <= 1.08f);
    bool passed = visible && state->fit_scale > 0.0f &&
        state->fit_scale <= 1.0001f && inside;
    fprintf(file, "%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%d,%u,%d\n",
        name, state->fit_scale, state->fit_translate_x,
        state->fit_translate_y, state->visible_min_x, state->visible_min_y,
        state->visible_max_x, state->visible_max_y, state->fitted,
        state->visible, state->mver_projection, edges, passed);
    return passed;
}

/* 表达式矩阵: 进入、稳定、两个指针方向、镜像、画布 50% 与 200% 各记一行; 断言 fit_scale 全程不变, 且稳定态
 * 的 fit_scale 为 1、无平移 (表达式不应重新构图)。width/height 用于把缩放实验改回原尺寸。
 */
static bool expression_matrix(FILE *file, BongoCatApp *app, int expression,
    int width, int height) {
    bool passed = true;
    BongoCatLive2DVisualState enter = {0}, stable = {0}, current = {0};
    bongo_cat_live2d_set_dragging(app->live2d, 0.0f, 0.0f);
    advance(app, 90);
    if (!bongo_cat_live2d_set_expression(app->live2d, expression)) return false;
    advance(app, 1);
    char label[48];
    snprintf(label, sizeof(label), "expression-%d-enter", expression);
    passed = record(file, app, label, false, &enter) && passed;
    advance(app, 120);
    snprintf(label, sizeof(label), "expression-%d-stable", expression);
    /* Expressions keep the authored projection; expanded geometry may be
       clipped by the fixed composition canvas rather than moving the model. */
    passed = record(file, app, label, false, &stable) && passed;
    passed = close_scale(enter.fit_scale, stable.fit_scale) &&
        !stable.fitted && close_scale(stable.fit_scale, 1.0f) &&
        close_scale(stable.fit_translate_x, 0.0f) &&
        close_scale(stable.fit_translate_y, 0.0f) && passed;

    const float pointers[][2] = {{-1.0f, 1.0f}, {1.0f, -1.0f}};
    for (int i = 0; i < 2; ++i) {
        bongo_cat_live2d_set_dragging(app->live2d, pointers[i][0], pointers[i][1]);
        advance(app, 90);
        snprintf(label, sizeof(label), "expression-%d-pointer-%d", expression, i);
        passed = record(file, app, label, false, &current) &&
            close_scale(stable.fit_scale, current.fit_scale) && passed;
    }
    bongo_cat_live2d_set_mirror(app->live2d, true);
    snprintf(label, sizeof(label), "expression-%d-mirror", expression);
    passed = record(file, app, label, false, &current) &&
        close_scale(stable.fit_scale, current.fit_scale) && passed;
    bongo_cat_live2d_reshape(app->live2d, width / 2, height / 2);
    snprintf(label, sizeof(label), "expression-%d-scale-50", expression);
    passed = record(file, app, label, false, &current) &&
        close_scale(stable.fit_scale, current.fit_scale) && passed;
    bongo_cat_live2d_reshape(app->live2d, width * 2, height * 2);
    snprintf(label, sizeof(label), "expression-%d-scale-200", expression);
    passed = record(file, app, label, false, &current) &&
        close_scale(stable.fit_scale, current.fit_scale) && passed;
    bongo_cat_live2d_reshape(app->live2d, width, height);
    bongo_cat_live2d_set_mirror(app->live2d, false);
    return passed;
}

static bool contains_index(const int *indexes, size_t count, int value) {
    for (size_t i = 0; i < count; ++i)
        if (indexes[i] == value) return true;
    return false;
}

/* 从 behaviors 表里挑出指定种类、去重后的索引; 容量不足就截断 (调用方给 BONGO_CAT_BEHAVIOR_CAP),
 * 返回实际个数。contains_index 是它用的线性去重助手。
 */
static size_t behavior_indexes(const BongoCatApp *app, BongoCatBehaviorKind kind,
    int *indexes, size_t capacity) {
    size_t count = 0;
    for (size_t i = 0; i < app->behaviors.count && count < capacity; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != kind || entry->index < 0 ||
            contains_index(indexes, count, entry->index)) continue;
        indexes[count++] = entry->index;
    }
    return count;
}

/* 同一个动作连播两次并比较进入帧: 第一次播放留下的缩放或位移残留会让第二次的 fit_scale 不同, 这是"动作
 * 污染构图"的典型表现。mver=true 时额外要求 fitted 为假且 fit_scale 为 1 —— MVER 投影必须保持作者构图。
 */
static bool motion_replay_matrix(FILE *file, BongoCatApp *app,
    const BongoCatBehaviorEntry *entry, bool mver) {
    BongoCatLive2DVisualState active = {0}, stable = {0}, replayed = {0};
    if (!bongo_cat_live2d_start_motion(app->live2d, entry->group, entry->index))
        return false;
    advance(app, 1);
    char label[96];
    snprintf(label, sizeof(label), "motion-%d-enter", entry->index);
    bool passed = record(file, app, label, !mver, &active);
    advance(app, 60);
    snprintf(label, sizeof(label), "motion-%d-stable", entry->index);
    passed = record(file, app, label, !mver, &stable) && passed;
    if (!bongo_cat_live2d_start_motion(app->live2d, entry->group, entry->index))
        return false;
    advance(app, 1);
    snprintf(label, sizeof(label), "motion-%d-replayed", entry->index);
    passed = record(file, app, label, !mver, &replayed) && passed;
    if (mver) passed = !active.fitted && !stable.fitted && !replayed.fitted &&
        close_scale(active.fit_scale, 1.0f) &&
        close_scale(stable.fit_scale, 1.0f) &&
        close_scale(replayed.fit_scale, 1.0f) && passed;
    return passed;
}

/* 入口: 需要 app/live2d/window, CSV 写在 state_root 下; baseline 的 mver_projection 决定后续判定口径
 * (MVER 走动作重放, 普通投影走 inside 包围盒检查), 所以这个字段必须在最前面采到。结束时恢复拖拽、
 * 镜像与画布并置 app->dirty; 返回值就是整轮是否通过, false 会被上层记成失败。
 */
bool bongo_cat_live2d_visual_audit_run(BongoCatApp *app) {
    if (!app || !app->live2d || !app->window) return false;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), app->state_root,
        "live2d-visual-audit.csv")) return false;
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) return false;
    fputs("case,fit_scale,translate_x,translate_y,min_x,min_y,max_x,max_y,"
        "fitted,visible,mver_projection,edge_pixels,passed\n", file);
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(app->window, &width, &height);
    bool passed = width > 1 && height > 1;
    BongoCatLive2DVisualState baseline = {0}, current = {0};
    bongo_cat_live2d_set_expression(app->live2d, -1);
    bongo_cat_live2d_set_dragging(app->live2d, 0.0f, 0.0f);
    advance(app, 90);
    passed = record(file, app, "idle", false, &baseline) && passed;
    if (!baseline.mver_projection) passed = inside(&baseline) && passed;
    passed = !baseline.fitted && close_scale(baseline.fit_scale, 1.0f) && passed;
    const float pointers[][2] = {{-1.0f, -1.0f}, {1.0f, 1.0f}};
    for (int i = 0; i < 2; ++i) {
        bongo_cat_live2d_set_dragging(app->live2d, pointers[i][0], pointers[i][1]);
        advance(app, 90);
        char label[32]; snprintf(label, sizeof(label), "pointer-%d", i);
        passed = record(file, app, label, !baseline.mver_projection, &current) &&
            close_scale(baseline.fit_scale, current.fit_scale) &&
            !current.fitted && passed;
    }
/* 模型没提供任何表达式时退回 0/1/2 三个内置索引, 保证审计至少覆盖一次表达式切换;
 * 真实模型都从 behaviors 表拿索引, 这条只是兜底。
 */
    int expression_indexes[BONGO_CAT_BEHAVIOR_CAP];
    size_t expression_count = behavior_indexes(app, BONGO_CAT_BEHAVIOR_EXPRESSION,
        expression_indexes, BONGO_CAT_BEHAVIOR_CAP);
    if (!expression_count) {
        expression_indexes[0] = 0;
        expression_indexes[1] = 1;
        expression_indexes[2] = 2;
        expression_count = 3;
    }
    for (size_t i = 0; i < expression_count; ++i) {
        int expression = expression_indexes[i];
        passed = expression_matrix(file, app, expression, width, height) && passed;
        bongo_cat_live2d_set_expression(app->live2d, -1);
        advance(app, 90);
        char label[32]; snprintf(label, sizeof(label), "expression-%d-reset", expression);
        passed = record(file, app, label, !baseline.mver_projection, &current) &&
            close_scale(current.fit_scale, 1.0f) && !current.fitted && passed;
    }
    /* 只有 MVER 投影才跑带 _lock 的动作重放: 这些动作自带固有位移, 普通自适应投影下 fit_scale
     * 本来就会变, 断言没有意义。
     */
    if (baseline.mver_projection) {
        for (size_t i = 0; i < app->behaviors.count; ++i) {
            const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
            if (entry->kind != BONGO_CAT_BEHAVIOR_MOTION ||
                strstr(entry->group, "_lock") == NULL) continue;
            passed = motion_replay_matrix(file, app, entry, true) && passed;
        }
    }
    bongo_cat_live2d_set_dragging(app->live2d, 0.0f, 0.0f);
    bongo_cat_live2d_set_mirror(app->live2d, false);
    bongo_cat_live2d_reshape(app->live2d, width, height);
    fprintf(file, "result,1,0,0,0,0,0,0,0,0,%d,0,%d\n",
        baseline.mver_projection, passed);
    fclose(file);
    app->dirty = true;
    return passed;
}
