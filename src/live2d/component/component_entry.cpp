/*
 * `cores/bongo_live2d.dll` 的 ABI 入口: 导出唯一的查询符号, 返回版本化描述符。
 *
 * 这一层只做三件事, 都要做对:
 *   1. **双向校验**: 先把宿主表头对一遍(abi_version / struct_size), 不符就返回 NULL ——
 *      老组件遇到新宿主必须能干净退出, 而不是按错位偏移去读宿主的结构体;
 *   2. 绑定服务表: 桥接代码不认"宿主表"这个概念, 它直接调宿主符号; 那些符号由
 *      component_host.c 定义并转发到这张表。所以绑定必须发生在**任何**桥接调用之前;
 *   3. 填描述符: 必需 7 个回调 + 可选回调(缺失就留空, 宿主会判空回落诊断实现)。
 *
 * 能力位: REQUIRES_CORE(没有用户自备的 Core 就没有渲染), 后端两位都声明 —— 组件里同时
 * 编了 OpenGL 与 Vulkan 两条渲染路径, 具体走哪条由宿主配置层决定(cubism_renderer_uses_vulkan)。
 */
#include "bongo_cat/model.h"
#include "bongo_cat/renderer_module_abi.h"

#include "component_host.h"

#include <SDL3/SDL_video.h>

#include <string.h>

/* 桥接实现的入口(声明在 include/bongo_cat/model.h 与 live2d_vulkan.h 里, 这里再声明一次
   是为了让"组件对外只依赖这几个符号"一目了然)。 */
extern "C" {
BongoCatLive2D *bongo_cat_live2d_create(const char *asset_root,
    BongoCatError *error);
void bongo_cat_live2d_destroy(BongoCatLive2D *live2d);
BongoCatResult bongo_cat_live2d_load(BongoCatLive2D *live2d,
    const char *model_dir, const char *setting_file, bool preset,
    const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error);
bool bongo_cat_live2d_ready(const BongoCatLive2D *live2d);
bool bongo_cat_live2d_update(BongoCatLive2D *live2d, float delta_seconds);
void bongo_cat_live2d_draw(BongoCatLive2D *live2d);
void bongo_cat_live2d_resize(BongoCatLive2D *live2d, int width, int height);
void bongo_cat_live2d_reshape(BongoCatLive2D *live2d, int width, int height);
bool bongo_cat_live2d_canvas_size(const BongoCatLive2D *live2d, int *width,
    int *height);
bool bongo_cat_live2d_frame(const BongoCatLive2D *live2d,
    BongoCatLive2DFrame *frame);
bool bongo_cat_live2d_viewport(const BongoCatLive2D *live2d, int *x, int *y,
    int *width, int *height);
void bongo_cat_live2d_set_mirror(BongoCatLive2D *live2d, bool mirror);
void bongo_cat_live2d_set_vertical_flip(BongoCatLive2D *live2d, bool flipped);
void bongo_cat_live2d_set_render_options(BongoCatLive2D *live2d,
    const BongoCatLive2DRenderOptions *options);
void bongo_cat_live2d_set_dragging(BongoCatLive2D *live2d, float x, float y);
void bongo_cat_live2d_set_centered_dragging(BongoCatLive2D *live2d, float x,
    float y);
bool bongo_cat_live2d_set_parameter(BongoCatLive2D *live2d, const char *id,
    float value);
bool bongo_cat_live2d_parameter(BongoCatLive2D *live2d, const char *id,
    BongoCatParameterRange *range);
bool bongo_cat_live2d_start_motion(BongoCatLive2D *live2d, const char *group,
    int index);
bool bongo_cat_live2d_restore_motion_state(BongoCatLive2D *live2d,
    const char *group, int index);
bool bongo_cat_live2d_preview_motion(BongoCatLive2D *live2d, const char *group,
    int index);
bool bongo_cat_live2d_restore_motion_preview(BongoCatLive2D *live2d);
bool bongo_cat_live2d_commit_motion_preview(BongoCatLive2D *live2d,
    const char *group, int index);
bool bongo_cat_live2d_motion_selected(const BongoCatLive2D *live2d,
    const char *group, int index);
bool bongo_cat_live2d_motion_persistent(const BongoCatLive2D *live2d,
    const char *group, int index);
bool bongo_cat_live2d_motion_visible(const BongoCatLive2D *live2d,
    const char *group, int index);
bool bongo_cat_live2d_motion_same_toggle(const BongoCatLive2D *live2d,
    const char *group, int left_index, const char *right_group, int right_index);
bool bongo_cat_live2d_set_expression(BongoCatLive2D *live2d, int index);
int bongo_cat_live2d_expression(const BongoCatLive2D *live2d);
bool bongo_cat_live2d_visual_state(const BongoCatLive2D *live2d,
    BongoCatLive2DVisualState *state);
bool bongo_cat_live2d_prepare_cover_capture(BongoCatLive2D *live2d);
void bongo_cat_live2d_prepare_viewer_audit(BongoCatLive2D *live2d);
BongoCatLive2DLoadSession *bongo_cat_live2d_load_session_begin(
    BongoCatLive2D *live2d, const char *model_dir, const char *setting_file,
    bool preset, const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error);
BongoCatLive2DStepResult bongo_cat_live2d_load_session_activate(
    BongoCatLive2DLoadSession *session, SDL_Window *window, void *context,
    BongoCatError *error);
BongoCatLive2DStepResult bongo_cat_live2d_load_session_step(
    BongoCatLive2DLoadSession *session, const char *model_dir,
    const char *setting_file, BongoCatError *error);
void bongo_cat_live2d_load_session_end(BongoCatLive2DLoadSession *session);
void bongo_cat_live2d_load_session_abort(BongoCatLive2DLoadSession *session);
}

namespace {

/* ops.create 收到宿主表: 先绑定, 之后所有桥接调用才会走服务表。 */
void *op_create(const BongoCatRendererModuleHost *host, const char *asset_root,
    BongoCatError *error) {
    if (!host || host->abi_version != BONGO_CAT_RENDERER_MODULE_ABI ||
        host->struct_size != sizeof(*host)) {
        /* 不能调用 bongo_cat_error_set 之外的宿主服务 —— 表本身可能就不是我们认识的那张。 */
        return NULL;
    }
    bongo_component_bind(host);
    return bongo_cat_live2d_create(asset_root, error);
}

void op_destroy(void *instance) {
    bongo_cat_live2d_destroy((BongoCatLive2D *)instance);
}

BongoCatResult op_load(void *instance, const char *model_dir,
    const char *setting_file, bool preset,
    const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error) {
    return bongo_cat_live2d_load((BongoCatLive2D *)instance, model_dir,
        setting_file, preset, render_options, progress, userdata, error);
}

bool op_ready(const void *instance) {
    return bongo_cat_live2d_ready((const BongoCatLive2D *)instance);
}

bool op_update(void *instance, float delta_seconds) {
    return bongo_cat_live2d_update((BongoCatLive2D *)instance, delta_seconds);
}

void op_draw(void *instance) {
    bongo_cat_live2d_draw((BongoCatLive2D *)instance);
}

void op_resize(void *instance, int width, int height) {
    bongo_cat_live2d_resize((BongoCatLive2D *)instance, width, height);
}

/* ---- 可选回调: 桥接里有的就接上, 没有的留空让宿主回落 ---- */

void op_reshape(void *instance, int width, int height) {
    bongo_cat_live2d_reshape((BongoCatLive2D *)instance, width, height);
}

bool op_canvas_size(const void *instance, int *width, int *height) {
    return bongo_cat_live2d_canvas_size((const BongoCatLive2D *)instance, width,
        height);
}

bool op_frame(const void *instance, BongoCatLive2DFrame *frame) {
    return bongo_cat_live2d_frame((const BongoCatLive2D *)instance, frame);
}

bool op_viewport(const void *instance, int *x, int *y, int *width, int *height) {
    return bongo_cat_live2d_viewport((const BongoCatLive2D *)instance, x, y,
        width, height);
}

void op_set_mirror(void *instance, bool mirror) {
    bongo_cat_live2d_set_mirror((BongoCatLive2D *)instance, mirror);
}

void op_set_vertical_flip(void *instance, bool flipped) {
    bongo_cat_live2d_set_vertical_flip((BongoCatLive2D *)instance, flipped);
}

void op_set_render_options(void *instance,
    const BongoCatLive2DRenderOptions *options) {
    bongo_cat_live2d_set_render_options((BongoCatLive2D *)instance, options);
}

void op_set_dragging(void *instance, float x, float y) {
    bongo_cat_live2d_set_dragging((BongoCatLive2D *)instance, x, y);
}

void op_set_centered_dragging(void *instance, float x, float y) {
    bongo_cat_live2d_set_centered_dragging((BongoCatLive2D *)instance, x, y);
}

bool op_set_parameter(void *instance, const char *id, float value) {
    return bongo_cat_live2d_set_parameter((BongoCatLive2D *)instance, id, value);
}

bool op_parameter(void *instance, const char *id, BongoCatParameterRange *range) {
    return bongo_cat_live2d_parameter((BongoCatLive2D *)instance, id, range);
}

bool op_start_motion(void *instance, const char *group, int index) {
    return bongo_cat_live2d_start_motion((BongoCatLive2D *)instance, group, index);
}

bool op_restore_motion_state(void *instance, const char *group, int index) {
    return bongo_cat_live2d_restore_motion_state((BongoCatLive2D *)instance, group,
        index);
}

bool op_preview_motion(void *instance, const char *group, int index) {
    return bongo_cat_live2d_preview_motion((BongoCatLive2D *)instance, group,
        index);
}

bool op_restore_motion_preview(void *instance) {
    return bongo_cat_live2d_restore_motion_preview((BongoCatLive2D *)instance);
}

bool op_commit_motion_preview(void *instance, const char *group, int index) {
    return bongo_cat_live2d_commit_motion_preview((BongoCatLive2D *)instance,
        group, index);
}

bool op_motion_selected(const void *instance, const char *group, int index) {
    return bongo_cat_live2d_motion_selected((const BongoCatLive2D *)instance,
        group, index);
}

bool op_motion_persistent(const void *instance, const char *group, int index) {
    return bongo_cat_live2d_motion_persistent((const BongoCatLive2D *)instance,
        group, index);
}

bool op_motion_visible(const void *instance, const char *group, int index) {
    return bongo_cat_live2d_motion_visible((const BongoCatLive2D *)instance, group,
        index);
}

bool op_motion_same_toggle(const void *instance, const char *group,
    int left_index, const char *right_group, int right_index) {
    return bongo_cat_live2d_motion_same_toggle((const BongoCatLive2D *)instance,
        group, left_index, right_group, right_index);
}

bool op_set_expression(void *instance, int index) {
    return bongo_cat_live2d_set_expression((BongoCatLive2D *)instance, index);
}

int op_expression(const void *instance) {
    return bongo_cat_live2d_expression((const BongoCatLive2D *)instance);
}

bool op_visual_state(const void *instance, BongoCatLive2DVisualState *state) {
    return bongo_cat_live2d_visual_state((const BongoCatLive2D *)instance, state);
}

bool op_prepare_cover_capture(void *instance) {
    return bongo_cat_live2d_prepare_cover_capture((BongoCatLive2D *)instance);
}

void op_prepare_viewer_audit(void *instance) {
    bongo_cat_live2d_prepare_viewer_audit((BongoCatLive2D *)instance);
}

/*
 * 分步加载会话。宿主换模型走的正是这条路径(渐进解码/上传, 其间还要继续出帧), 所以**必须**
 * 接上: 只接一次性 load 的话, 组件会在"宿主以为已经加载完"的状态下从未真正装载过模型 ——
 * 表现是模型不显示, 而且没有任何报错, 属于最难查的一类失败。
 */
void *op_load_session_begin(void *instance, const char *model_dir,
    const char *setting_file, bool preset,
    const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error) {
    return bongo_cat_live2d_load_session_begin((BongoCatLive2D *)instance,
        model_dir, setting_file, preset, render_options, progress, userdata, error);
}

int op_load_session_activate(void *session, void *window, void *context,
    BongoCatError *error) {
    return (int)bongo_cat_live2d_load_session_activate(
        (BongoCatLive2DLoadSession *)session, (SDL_Window *)window, context, error);
}

int op_load_session_step(void *session, const char *model_dir,
    const char *setting_file, BongoCatError *error) {
    return (int)bongo_cat_live2d_load_session_step(
        (BongoCatLive2DLoadSession *)session, model_dir, setting_file, error);
}

void op_load_session_end(void *session) {
    bongo_cat_live2d_load_session_end((BongoCatLive2DLoadSession *)session);
}

void op_load_session_abort(void *session) {
    bongo_cat_live2d_load_session_abort((BongoCatLive2DLoadSession *)session);
}

const BongoCatRendererModule module = {
    BONGO_CAT_RENDERER_MODULE_ABI,
    sizeof(BongoCatRendererModule),
    BONGO_CAT_RENDERER_ENGINE_LIVE2D,
    BONGO_CAT_RENDERER_FLAG_REQUIRES_CORE,
    BONGO_CAT_RENDERER_BACKEND_OPENGL | BONGO_CAT_RENDERER_BACKEND_VULKAN,
    "bongo_live2d (Cubism Framework)",
    {
        op_create, op_destroy, op_load, op_ready, op_update, op_draw, op_resize,
        op_reshape, op_canvas_size, op_frame, op_viewport, op_set_mirror,
        op_set_vertical_flip, op_set_render_options, op_set_dragging,
        op_set_centered_dragging, op_set_parameter, op_parameter,
        op_start_motion, op_restore_motion_state, op_preview_motion,
        op_restore_motion_preview, op_commit_motion_preview, op_motion_selected,
        op_motion_persistent, op_motion_visible, op_motion_same_toggle,
        op_set_expression, op_expression, op_visual_state,
        op_prepare_cover_capture, op_prepare_viewer_audit,
        op_load_session_begin, op_load_session_activate, op_load_session_step,
        op_load_session_end, op_load_session_abort
    }
};

} // namespace

extern "C" BONGO_CAT_RENDERER_MODULE_EXPORT const BongoCatRendererModule *
bongo_cat_renderer_module_query(uint32_t abi,
    const BongoCatRendererModuleHost *host) {
    /* 双向校验: 宿主表头必须与我们的 ABI 版本一致、尺寸一致, 否则拒绝配合。 */
    if (abi != BONGO_CAT_RENDERER_MODULE_ABI || !host ||
        host->abi_version != abi || host->struct_size != sizeof(*host))
        return nullptr;
    return &module;
}
