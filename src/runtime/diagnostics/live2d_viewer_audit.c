/* 对齐 Cubism Viewer 5.3 参数表现的逐帧回归: 把窗口临时改成 900x900, 用固定 1/60s 步进推进模型, 每一步
 * 自绘一帧并抓 BMP 与参数 CSV (native-parameters.csv / native-trace.csv, 落在 state_root/cubism-viewer-audit/)。
 * 它是唯一自己接管渲染与缓冲交换的审计: 绕过主循环的背景、遮罩与叠加层, 结果不能与实机帧直接比较, 也会
 * 真的把审计帧显示给用户。抓像素只支持 OpenGL —— Vulkan 下 save_frame 一律失败, 所以上层对
 * viewer-sequence 场景不做参数断言, 只记录 operation 与 assertions。
 */
#include "runtime.h"
#include "bongo_cat/render.h"
#include "live2d_viewer_timing.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL_opengl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CSV 的列顺序就是这个数组的顺序, 外部脚本按列号取值, 插队或改序等于破坏契约。前四项 (ParamAngleX/Y、
 * ParamEyeBallX/Y) 被视为必需, 缺任何一个该帧就记 required=0, 后面的项缺了只写 nan。
 */
static const char *metric_parameters[] = {
    "ParamAngleX", "ParamAngleY", "ParamEyeBallX", "ParamEyeBallY",
    "ParamEyeLOpen", "ParamEyeROpen",
    "ParamBodyAngleX", "ParamBreath", "ParamAngleZ", "ParamMouseX", "ParamMouseY",
    "ParamMouseLeftDown", "ParamMouseRightDown", "CatParamLeftHandDown",
    "hair1", "hair2", "hair3", "hair4", "hair5", "hair6",
    "hair10", "hair11", "hair12", "hair13",
    "chest1", "chest3", "chest4", "Param", "Param18", "Param19", "Param20",
    "Param24", "Param25", "Param26",
    "Param28", "RE1", "RE2", "LE1", "LE2"
};
/* 这两组名字必须与 live2d_viewer_timing.c 里的同名数组逐字对应: 那边按名字从参考 trace 反查帧号, 缺项即整轮审计失败。 */
static const char *track_names[] = {"track-001", "track-002", "track-004",
    "track-008", "track-015", "track-030"};
static const char *return_names[] = {"return-001", "return-002", "return-004",
    "return-008", "return-015", "return-030"};
/* 取参数当前值; 参数不存在时返回 false 且不写 *value, 而 parameter_range 会把取值范围整个交出去。 */
static bool parameter(BongoCatApp *app, const char *id, float *value) {
    BongoCatParameterRange range;
    if (!bongo_cat_live2d_parameter(app->live2d, id, &range)) return false;
    *value = range.value;
    return true;
}
static bool parameter_range(BongoCatApp *app, const char *id,
    BongoCatParameterRange *range) {
    return bongo_cat_live2d_parameter(app->live2d, id, range);
}
/* 复刻 Viewer 的目标值公式: 按拖动方向取该侧幅度 (负方向用 minimum, 正方向用 maximum), 再乘方向、
 * 拖动比例与增益。期望值由参数范围反推而来, 与实机实现是两份独立代码, 两边不一致正是本审计要抓的。
 */
static float viewer_target(const BongoCatParameterRange *range,
    float direction, float weight) {
    float extent = direction < 0.0f ? fabsf(range->minimum) :
        fabsf(range->maximum);
    return direction * extent * weight;
}
static bool parameter_near(BongoCatApp *app, const char *id,
    float expected, float tolerance) {
    float actual = 0.0f;
    return parameter(app, id, &actual) && fabsf(actual - expected) <= tolerance;
}
/* 自绘一帧、读回像素、上下翻转后存 BMP, 同时把该帧参数写进 metrics。返回 false 的常见原因: Vulkan
 * 后端 (无离屏路径)、窗口太小、前四项必需参数缺失、写盘失败。会改动 GL 状态且不恢复, 只能在这种
 * 独占式审计里用。
 */
static bool save_frame(BongoCatApp *app, const char *directory,
    const char *name, FILE *metrics) {
    /* 该审计自己清屏画一帧再读像素, Vulkan 下先跳过 (需要独立的离屏渲染路径) */
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) return false;
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(app->window, &width, &height);
    if (width < 2 || height < 2) return false;
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    bongo_cat_live2d_draw(app->live2d);
    glFinish();
    size_t pitch = (size_t)width * 4;
    unsigned char *pixels = malloc(pitch * (size_t)height);
    unsigned char *row = malloc(pitch);
    if (!pixels || !row) { free(pixels); free(row); return false; }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (int y = 0; y < height / 2; ++y) {
        unsigned char *top = pixels + (size_t)y * pitch;
        unsigned char *bottom = pixels + (size_t)(height - 1 - y) * pitch;
        memcpy(row, top, pitch); memcpy(top, bottom, pitch); memcpy(bottom, row, pitch);
    }
    SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height,
        SDL_PIXELFORMAT_RGBA32, pixels, (int)pitch);
    char filename[64], path[BONGO_CAT_PATH_CAP];
    snprintf(filename, sizeof(filename), "%s.bmp", name);
    bool saved = surface && bongo_cat_path_join(path, sizeof(path), directory,
        filename) && SDL_SaveBMP(surface, path);
    if (surface) SDL_DestroySurface(surface);
    free(row); free(pixels);
    bool required = true;
    if (metrics) {
        fputs(name, metrics);
        for (size_t i = 0; i < sizeof(metric_parameters) /
            sizeof(metric_parameters[0]); ++i) {
            float value = 0.0f;
            bool available = parameter(app, metric_parameters[i], &value);
            if (i < 4) required = available && required;
            if (available) fprintf(metrics, ",%.6f", value);
            else fputs(",nan", metrics);
        }
        BongoCatLive2DVisualState visual = {0};
        bool visual_ready = bongo_cat_live2d_visual_state(app->live2d, &visual);
        if (visual_ready) fprintf(metrics, ",%d,%d,%d,%d,%d,%d,%d",
            visual.drawable_count, visual.drawable_visible,
            visual.drawable_vertex_changed, visual.offscreen_count,
            visual.offscreen_positive, visual.part_count, visual.part_positive);
        else fputs(",nan,nan,nan,nan,nan,nan,nan", metrics);
        fprintf(metrics, ",%d\n", required);
    }
    return saved && required;
}
/* 审计专用最小渲染路径: 清屏、画模型、交换缓冲; 不跑背景色、圆角遮罩与叠加层, 所以它推进的是 Cubism
 * 的渲染时序, 而不是实机帧。
 */
static void render_step(BongoCatApp *app) {
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(app->window, &width, &height);
    if (width < 2 || height < 2) return;
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    bongo_cat_live2d_draw(app->live2d);
    SDL_GL_SwapWindow(app->window);
}
/* 固定步长推进 (每次 1/60s), 与真实帧率无关, 这样同样的帧号在任何机器上都对应同样的模型时间,
 * 回归结果才有可比性; 每帧都会真的 Swap 一次。
 */
static void advance(BongoCatApp *app, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        bongo_cat_live2d_update(app->live2d, 1.0f / 60.0f);
        render_step(app);
    }
}
/* 在 save_frame 之外再往 trace 里写一行 (名字, 帧号, 换算出的毫秒), 供 live2d_viewer_timing.c 在下一轮
 * 审计里反推"当时是在第几帧抓的图"。毫秒列由帧号换算而来, 不是真实时钟。
 */
static bool save_timed(BongoCatApp *app, const char *directory,
    const char *name, int frame, FILE *metrics, FILE *timing) {
    if (timing) fprintf(timing, "%s,%d,%.3f\n", name, frame,
        frame * (1000.0 / 60.0));
    return save_frame(app, directory, name, metrics);
}
/* 推进并逐帧跟踪 ParamAngleX 的变化, 取相邻帧差的最大值; 阈值 6.0 在下面判定, 用来挡住"参数瞬间跳到
 * 目标值"这种不符合 Viewer 速度曲线的实现。
 */
static bool advance_tracking(BongoCatApp *app, int frames,
    float *previous_x, float *maximum_step) {
    for (int frame = 0; frame < frames; ++frame) {
        bongo_cat_live2d_update(app->live2d, 1.0f / 60.0f);
        render_step(app);
        float current_x = 0.0f;
        if (!parameter(app, "ParamAngleX", &current_x)) return false;
        float step = fabsf(current_x - *previous_x);
        if (step > *maximum_step) *maximum_step = step;
        *previous_x = current_x;
    }
    return true;
}

/* 主流程: 放大窗口到固定基准尺寸 → 清空模型状态 → 表达式、拖拽与缩放矩阵 → 回中 → 恢复窗口。早退路径
 * (窗口尺寸设置失败、目录或文件建不出来、状态取不到) 不恢复窗口尺寸也不置 app->dirty, 只有走到最后才
 * 恢复, 所以本场景失败时要留意窗口可能留在基准尺寸。passed 逐条累积, 失败会让 operation=rejected。
 */
bool bongo_cat_live2d_viewer_audit_run(BongoCatApp *app) {
    if (!app || !app->live2d || !app->window) return false;
    int original_width = 0, original_height = 0;
    SDL_GetWindowSize(app->window, &original_width, &original_height);
    if (!SDL_SetWindowSize(app->window, 900, 900) ||
        !SDL_SyncWindow(app->window)) return false;
    int audit_width = 0, audit_height = 0;
    SDL_GetWindowSizeInPixels(app->window, &audit_width, &audit_height);
    bongo_cat_live2d_resize(app->live2d, audit_width, audit_height);
    char root[BONGO_CAT_PATH_CAP], native[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(root, sizeof(root), app->state_root,
        "cubism-viewer-audit") ||
        !bongo_cat_path_join(native, sizeof(native), root, "native") ||
        !bongo_cat_path_create_directory(root) ||
        !bongo_cat_path_create_directory(native)) return false;
    char metrics_path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(metrics_path, sizeof(metrics_path), root,
        "native-parameters.csv")) return false;
    FILE *metrics = bongo_cat_file_open(metrics_path, "wb");
    if (!metrics) return false;
    char timing_path[BONGO_CAT_PATH_CAP];
    FILE *timing = bongo_cat_path_join(timing_path, sizeof(timing_path), root,
        "native-trace.csv") ? bongo_cat_file_open(timing_path, "wb") : NULL;
    if (!timing) { fclose(metrics); return false; }
    fputs("frame,display_frame,display_ms\n", timing);
    fputs("frame", metrics);
    for (size_t i = 0; i < sizeof(metric_parameters) /
        sizeof(metric_parameters[0]); ++i)
        fprintf(metrics, ",%s", metric_parameters[i]);
    fputs(",drawable_count,drawable_visible,drawable_vertex_changed,"
        "offscreen_count,offscreen_positive,part_count,part_positive,"
        "required_available\n", metrics);
    bongo_cat_live2d_prepare_viewer_audit(app->live2d);
    bool passed = bongo_cat_live2d_set_expression(app->live2d, -1);
    bongo_cat_live2d_set_dragging(app->live2d, 0.0f, 0.0f);
    advance(app, 120);
    /* 不同模型的表达式驱动参数名不一样, 这里按 Param6 → Param5 依次探测; 两个都没有时
     * expression_parameter 为假, 后面"表达式确实改变了参数"这条断言就会失败。
     */
    const char *expression_id = "Param6";
    float expression_before = 0.0f;
    bool expression_parameter = parameter(app, expression_id, &expression_before);
    if (!expression_parameter) {
        expression_id = "Param5";
        expression_parameter = parameter(app, expression_id, &expression_before);
    }
    if (bongo_cat_live2d_set_expression(app->live2d, 2)) {
        advance(app, 60);
        passed = save_frame(app, native, "track-000", metrics) && passed;
        passed = save_frame(app, native, "idle", metrics) && passed;
        passed = save_frame(app, native, "expression3", metrics) && passed;
        float expression_after = expression_before;
        passed = expression_parameter && parameter(app, expression_id,
            &expression_after) && expression_after > expression_before + 0.5f && passed;
    } else passed = false;

    bongo_cat_live2d_set_dragging(app->live2d, -0.008875728f, 0.0f);
    advance(app, 12);
    const float target_drag_x = 0.497041464f;
    const float target_drag_y = 0.598930478f;
    BongoCatParameterRange angle_x_range, angle_y_range, angle_z_range;
    BongoCatParameterRange eye_x_range, eye_y_range, body_x_range;
    passed = parameter_range(app, "ParamAngleX", &angle_x_range) &&
        parameter_range(app, "ParamAngleY", &angle_y_range) &&
        parameter_range(app, "ParamEyeBallX", &eye_x_range) &&
        parameter_range(app, "ParamEyeBallY", &eye_y_range) && passed;
    bool has_body_x = parameter_range(app, "ParamBodyAngleX", &body_x_range);
    bool has_angle_z = parameter_range(app, "ParamAngleZ", &angle_z_range);
    const float viewer_gain = 1.2f;
    const float target_x = viewer_target(&angle_x_range,
        target_drag_x, viewer_gain);
    const float target_y = viewer_target(&angle_y_range,
        target_drag_y, viewer_gain);
    const float target_eye_x = viewer_target(&eye_x_range,
        target_drag_x, viewer_gain);
    const float target_eye_y = viewer_target(&eye_y_range,
        target_drag_y, viewer_gain);
    const float target_z = has_angle_z ? viewer_target(&angle_z_range,
        -target_drag_x * target_drag_y, 1.0f) : 0.0f;
    bongo_cat_live2d_set_centered_dragging(app->live2d, target_drag_x, target_drag_y);
    float previous_x = 0.0f, maximum_step = 0.0f;
    int track_frames[] = {1, 2, 4, 8, 15, 30};
    int return_frames[] = {1, 2, 4, 8, 15, 30};
    passed = bongo_cat_live2d_viewer_timing(app->smoke_viewer_trace,
        track_frames,
        return_frames) && passed;
    passed = parameter(app, "ParamAngleX", &previous_x) && passed;
    int previous = 0;
    float track_x[6] = {0};
    for (size_t i = 0; i < 6; ++i) {
        passed = advance_tracking(app, track_frames[i] - previous,
            &previous_x, &maximum_step) && passed;
        passed = save_timed(app, native, track_names[i], track_frames[i],
            metrics, timing) && passed;
        passed = parameter(app, "ParamAngleX", &track_x[i]) && passed;
        previous = track_frames[i];
    }
    /* 断言分三层: 位移随帧数单调增大且首帧远小于目标 (说明有加速过程而不是瞬移)、单帧步进 <6 (曲线连续)、
     * 以及稳态值分别贴近按 Viewer 公式算出的目标 (角度容差 0.4, 眼球容差 0.02 —— 眼球参数幅度小得多)。
     */
    passed = track_x[0] > 0.0f && track_x[0] < target_x * 0.35f &&
        track_x[1] > track_x[0] && track_x[2] > track_x[1] &&
        track_x[3] >= track_x[2] - 0.001f &&
        track_x[4] >= track_x[3] - 0.001f &&
        track_x[5] >= track_x[4] - 0.001f && maximum_step < 6.0f &&
        parameter_near(app, "ParamAngleX", target_x, 0.4f) &&
        parameter_near(app, "ParamAngleY", target_y, 0.4f) &&
        parameter_near(app, "ParamEyeBallX", target_eye_x, 0.02f) &&
        parameter_near(app, "ParamEyeBallY", target_eye_y, 0.02f) && passed;
    if (has_angle_z) passed = parameter_near(app, "ParamAngleZ", target_z, 0.4f) && passed;
    if (has_body_x) passed = parameter_near(app, "ParamBodyAngleX",
        viewer_target(&body_x_range, target_drag_x, viewer_gain), 0.2f) && passed;

    bongo_cat_live2d_set_centered_dragging(app->live2d, 0.0f, 0.0f);
    previous = 0;
    for (size_t i = 0; i < 6; ++i) {
        passed = advance_tracking(app, return_frames[i] - previous, &previous_x, &maximum_step) && passed;
        passed = save_timed(app, native, return_names[i], return_frames[i],
            metrics, timing) && passed;
        previous = return_frames[i];
    }
    passed = maximum_step < 6.0f &&
        parameter_near(app, "ParamAngleX", 0.0f, 0.25f) &&
        parameter_near(app, "ParamAngleY", 0.0f, 0.25f) &&
        parameter_near(app, "ParamEyeBallX", 0.0f, 0.01f) &&
        parameter_near(app, "ParamEyeBallY", 0.0f, 0.01f) &&
        parameter_near(app, "ParamMouseX", 0.0f, 0.001f) &&
        parameter_near(app, "ParamMouseY", 0.0f, 0.001f) && passed;
    if (has_angle_z) passed = parameter_near(app, "ParamAngleZ", 0.0f, 0.25f) && passed;
    fclose(timing);
    fclose(metrics);
    SDL_SetWindowSize(app->window, original_width, original_height);
    SDL_SyncWindow(app->window);
    SDL_GetWindowSizeInPixels(app->window, &audit_width, &audit_height);
    bongo_cat_live2d_resize(app->live2d, audit_width, audit_height);
    app->dirty = true;
    return passed;
}
