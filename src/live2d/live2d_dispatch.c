/*
 * Live2D 调用点的分发层: `bongo_cat_live2d_*` 是宿主里唯一的实现入口, 它把每次调用送到
 * 当前可用的实现上 —— 优先渲染组件(cores/bongo_live2d.*), 否则回落到诊断实现(live2d_stub.c)。
 *
 * 为什么要有这一层, 而不是让调用点直接找组件:
 *   1. 宿主有 60 多处调用 `bongo_cat_live2d_*`, 让它们各自处理"组件在不在"会把这套判断散到
 *      渲染/UI/诊断各处, 一旦漏一处就是崩溃或静默失效;
 *   2. "组件不在"必须是**正常状态**(用户没放组件、组件被 .disabled 停用、ABI 不匹配),
 *      回落语义要集中定义: 查询类返回中性值, 动作类安静地不做;
 *   3. 组件的可选回调可能为空(它只保证 7 个必备回调), 所以**每个调用点都要判空**再回落 ——
 *      这正是本文件里每个函数的统一形状。
 *
 * 不变式: 组件在**本次运行内只会被装载一次**(首次需要真实例时装载), 因此"实例来自组件还是
 * 来自诊断实现"可以用一个进程级标志判断, 不需要在每个句柄里记来源。
 */
#include "live2d_dispatch.h"

#include "bongo_cat/live2d_vulkan.h"
#include "bongo_cat/log.h"
#include "bongo_cat/model.h"
#include "bongo_cat/renderer_module.h"
#include "bongo_cat/renderer_module_abi.h"

#include <SDL3/SDL.h>

#define STUB(name) stub_live2d_##name

/* 诊断实现(编译期被宏改名, 见 cmake/RuntimeSources.cmake): 它与本文件是同一组符号的两个
   来源, 改名之后才能同时存在。 */
/* 声明(本文件是 C, 默认 C 链接) */
BongoCatLive2D *STUB(create)(const char *asset_root, BongoCatError *error);
void STUB(destroy)(BongoCatLive2D *live2d);
BongoCatResult STUB(load)(BongoCatLive2D *live2d, const char *model_dir,
    const char *setting_file, bool preset,
    const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error);
BongoCatLive2DLoadSession *STUB(load_session_begin)(BongoCatLive2D *live2d,
    const char *model_dir, const char *setting_file, bool preset,
    const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error);
BongoCatLive2DStepResult STUB(load_session_activate)(
    BongoCatLive2DLoadSession *session, SDL_Window *window, void *context,
    BongoCatError *error);
BongoCatLive2DStepResult STUB(load_session_step)(
    BongoCatLive2DLoadSession *session, const char *model_dir,
    const char *setting_file, BongoCatError *error);
void STUB(load_session_end)(BongoCatLive2DLoadSession *session);
void STUB(load_session_abort)(BongoCatLive2DLoadSession *session);
bool STUB(ready)(const BongoCatLive2D *live2d);
bool STUB(canvas_size)(const BongoCatLive2D *live2d, int *width, int *height);
bool STUB(frame)(const BongoCatLive2D *live2d, BongoCatLive2DFrame *frame);
bool STUB(viewport)(const BongoCatLive2D *live2d, int *x, int *y, int *width,
    int *height);
void STUB(resize)(BongoCatLive2D *live2d, int width, int height);
void STUB(reshape)(BongoCatLive2D *live2d, int width, int height);
bool STUB(update)(BongoCatLive2D *live2d, float delta_seconds);
void STUB(draw)(BongoCatLive2D *live2d);
void STUB(set_mirror)(BongoCatLive2D *live2d, bool mirror);
void STUB(set_vertical_flip)(BongoCatLive2D *live2d, bool flipped);
void STUB(set_render_options)(BongoCatLive2D *live2d,
    const BongoCatLive2DRenderOptions *options);
void STUB(set_dragging)(BongoCatLive2D *live2d, float x, float y);
void STUB(set_centered_dragging)(BongoCatLive2D *live2d, float x, float y);
bool STUB(set_parameter)(BongoCatLive2D *live2d, const char *id, float value);
bool STUB(parameter)(BongoCatLive2D *live2d, const char *id,
    BongoCatParameterRange *range);
bool STUB(start_motion)(BongoCatLive2D *live2d, const char *group, int index);
bool STUB(restore_motion_state)(BongoCatLive2D *live2d, const char *group,
    int index);
bool STUB(preview_motion)(BongoCatLive2D *live2d, const char *group, int index);
bool STUB(restore_motion_preview)(BongoCatLive2D *live2d);
bool STUB(commit_motion_preview)(BongoCatLive2D *live2d, const char *group,
    int index);
bool STUB(motion_selected)(const BongoCatLive2D *live2d, const char *group,
    int index);
bool STUB(motion_persistent)(const BongoCatLive2D *live2d, const char *group,
    int index);
bool STUB(motion_visible)(const BongoCatLive2D *live2d, const char *group,
    int index);
bool STUB(motion_same_toggle)(const BongoCatLive2D *live2d, const char *left_group,
    int left_index, const char *right_group, int right_index);
bool STUB(set_expression)(BongoCatLive2D *live2d, int index);
int STUB(expression)(const BongoCatLive2D *live2d);
bool STUB(visual_state)(const BongoCatLive2D *live2d,
    BongoCatLive2DVisualState *state);
bool STUB(prepare_cover_capture)(BongoCatLive2D *live2d);
void STUB(prepare_viewer_audit)(BongoCatLive2D *live2d);


/* 本次运行是否已经在用组件。见文件头的不变式。 */
static bool component_used;
static bool load_attempted;
static char component_path[BONGO_CAT_PATH_CAP];

static const BongoCatRendererModuleOps *ops(void) {
    const BongoCatRendererModule *module =
        bongo_cat_renderer_module_active_descriptor();
    return module ? &module->ops : NULL;
}

bool bongo_cat_live2d_native_active(void) { return component_used; }

const char *bongo_cat_live2d_component_path(void) { return component_path; }

/* 首次需要真实例时装载组件: 这一步需要资源根(着色器), 而资源根只有 create 的调用点知道,
   所以装载放在这里而不是启动期 —— 启动期只做"发现 + 记日志"。 */
static bool ensure_component(void) {
    if (load_attempted) return component_used;
    load_attempted = true;
    BongoCatRendererModuleInfo *modules = NULL;
    size_t count = bongo_cat_renderer_module_last(&modules);
    for (size_t i = 0; i < count; ++i) {
        if (modules[i].state != BONGO_CAT_RENDERER_MODULE_READY) continue;
        BongoCatError error = {0};
        if (!bongo_cat_renderer_module_load(modules[i].path, &error)) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[live2d] 渲染组件装载失败(%s): %s — 回落到诊断实现",
                modules[i].path, error.message);
            continue;
        }
        snprintf(component_path, sizeof(component_path), "%s", modules[i].path);
        component_used = true;
        return true;
    }
    return false;
}

BongoCatLive2D *bongo_cat_live2d_create(const char *asset_root,
    BongoCatError *error) {
    if (ensure_component()) {
        const BongoCatRendererModuleOps *active = ops();
        if (active && active->create) {
            void *instance = active->create(bongo_cat_renderer_module_host(),
                asset_root, error);
            if (instance) return (BongoCatLive2D *)instance;
            /* 组件创建失败: 不把它当成致命 —— 回落到诊断实现, 用户至少还能看到界面。 */
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[live2d] 组件创建实例失败, 回落到诊断实现: %s",
                error ? error->message : "");
            component_used = false;
        }
    }
    return STUB(create)(asset_root, error);
}

void bongo_cat_live2d_destroy(BongoCatLive2D *live2d) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->destroy) {
        active->destroy(live2d);
        return;
    }
    STUB(destroy)(live2d);
}

/* 下面的形状是一致的: 组件在且有这个回调 -> 交给它; 否则回落诊断实现的中性行为。
   查询类必须返回中性值而不是"随机 false": 见 live2d_stub.c 的约定。 */
#define FORWARD_BOOL(name, ...) do { \
        const BongoCatRendererModuleOps *active = component_used ? ops() : NULL; \
        if (active && active->name) return active->name(__VA_ARGS__); \
        return STUB(name)(__VA_ARGS__); \
    } while (0)

bool bongo_cat_live2d_ready(const BongoCatLive2D *live2d) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->ready) return active->ready(live2d);
    return STUB(ready)(live2d);
}

bool bongo_cat_live2d_canvas_size(const BongoCatLive2D *live2d, int *width,
    int *height) {
    FORWARD_BOOL(canvas_size, live2d, width, height);
}

bool bongo_cat_live2d_frame(const BongoCatLive2D *live2d,
    BongoCatLive2DFrame *frame) {
    FORWARD_BOOL(frame, live2d, frame);
}

bool bongo_cat_live2d_viewport(const BongoCatLive2D *live2d, int *x, int *y,
    int *width, int *height) {
    FORWARD_BOOL(viewport, live2d, x, y, width, height);
}

bool bongo_cat_live2d_update(BongoCatLive2D *live2d, float delta_seconds) {
    FORWARD_BOOL(update, live2d, delta_seconds);
}

void bongo_cat_live2d_draw(BongoCatLive2D *live2d) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->draw) { active->draw(live2d); return; }
    STUB(draw)(live2d);
}

void bongo_cat_live2d_resize(BongoCatLive2D *live2d, int width, int height) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->resize) { active->resize(live2d, width, height); return; }
    STUB(resize)(live2d, width, height);
}

void bongo_cat_live2d_reshape(BongoCatLive2D *live2d, int width, int height) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->reshape) { active->reshape(live2d, width, height); return; }
    STUB(reshape)(live2d, width, height);
}

void bongo_cat_live2d_set_mirror(BongoCatLive2D *live2d, bool mirror) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->set_mirror) { active->set_mirror(live2d, mirror); return; }
    STUB(set_mirror)(live2d, mirror);
}

void bongo_cat_live2d_set_vertical_flip(BongoCatLive2D *live2d, bool flipped) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->set_vertical_flip) {
        active->set_vertical_flip(live2d, flipped);
        return;
    }
    STUB(set_vertical_flip)(live2d, flipped);
}

void bongo_cat_live2d_set_render_options(BongoCatLive2D *live2d,
    const BongoCatLive2DRenderOptions *options) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->set_render_options) {
        active->set_render_options(live2d, options);
        return;
    }
    STUB(set_render_options)(live2d, options);
}

void bongo_cat_live2d_set_dragging(BongoCatLive2D *live2d, float x, float y) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->set_dragging) { active->set_dragging(live2d, x, y); return; }
    STUB(set_dragging)(live2d, x, y);
}

void bongo_cat_live2d_set_centered_dragging(BongoCatLive2D *live2d, float x,
    float y) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->set_centered_dragging) {
        active->set_centered_dragging(live2d, x, y);
        return;
    }
    STUB(set_centered_dragging)(live2d, x, y);
}

bool bongo_cat_live2d_set_parameter(BongoCatLive2D *live2d, const char *id,
    float value) {
    FORWARD_BOOL(set_parameter, live2d, id, value);
}

bool bongo_cat_live2d_parameter(BongoCatLive2D *live2d, const char *id,
    BongoCatParameterRange *range) {
    FORWARD_BOOL(parameter, live2d, id, range);
}

bool bongo_cat_live2d_start_motion(BongoCatLive2D *live2d, const char *group,
    int index) {
    FORWARD_BOOL(start_motion, live2d, group, index);
}

bool bongo_cat_live2d_restore_motion_state(BongoCatLive2D *live2d,
    const char *group, int index) {
    FORWARD_BOOL(restore_motion_state, live2d, group, index);
}

bool bongo_cat_live2d_preview_motion(BongoCatLive2D *live2d, const char *group,
    int index) {
    FORWARD_BOOL(preview_motion, live2d, group, index);
}

bool bongo_cat_live2d_restore_motion_preview(BongoCatLive2D *live2d) {
    FORWARD_BOOL(restore_motion_preview, live2d);
}

bool bongo_cat_live2d_commit_motion_preview(BongoCatLive2D *live2d,
    const char *group, int index) {
    FORWARD_BOOL(commit_motion_preview, live2d, group, index);
}

bool bongo_cat_live2d_motion_selected(const BongoCatLive2D *live2d,
    const char *group, int index) {
    FORWARD_BOOL(motion_selected, live2d, group, index);
}

bool bongo_cat_live2d_motion_persistent(const BongoCatLive2D *live2d,
    const char *group, int index) {
    FORWARD_BOOL(motion_persistent, live2d, group, index);
}

bool bongo_cat_live2d_motion_visible(const BongoCatLive2D *live2d,
    const char *group, int index) {
    FORWARD_BOOL(motion_visible, live2d, group, index);
}

bool bongo_cat_live2d_motion_same_toggle(const BongoCatLive2D *live2d,
    const char *left_group, int left_index, const char *right_group,
    int right_index) {
    FORWARD_BOOL(motion_same_toggle, live2d, left_group, left_index, right_group,
        right_index);
}

bool bongo_cat_live2d_set_expression(BongoCatLive2D *live2d, int index) {
    FORWARD_BOOL(set_expression, live2d, index);
}

/* 表达式查询的回落值是 -1(而不是 0): 0 是一个合法表达式下标, 返回它会让上层以为"当前就是
   第 0 个", 而诊断实现下正确的语义是"没有表达式信息"。 */
int bongo_cat_live2d_expression(const BongoCatLive2D *live2d) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->expression) return active->expression(live2d);
    return STUB(expression)(live2d);
}

bool bongo_cat_live2d_visual_state(const BongoCatLive2D *live2d,
    BongoCatLive2DVisualState *state) {
    FORWARD_BOOL(visual_state, live2d, state);
}

bool bongo_cat_live2d_prepare_cover_capture(BongoCatLive2D *live2d) {
    FORWARD_BOOL(prepare_cover_capture, live2d);
}

void bongo_cat_live2d_prepare_viewer_audit(BongoCatLive2D *live2d) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->prepare_viewer_audit) {
        active->prepare_viewer_audit(live2d);
        return;
    }
    STUB(prepare_viewer_audit)(live2d);
}

/* 一次性加载与分步加载: 组件的分步接口可能为空(它只保证必备回调), 那种情况下退回一次性
   加载语义 —— stub 的分步实现本来就是这样做的。 */
BongoCatResult bongo_cat_live2d_load(BongoCatLive2D *live2d, const char *model_dir,
    const char *setting_file, bool preset,
    const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->load)
        return active->load(live2d, model_dir, setting_file, preset,
            render_options, progress, userdata, error);
    return STUB(load)(live2d, model_dir, setting_file, preset, render_options,
        progress, userdata, error);
}

BongoCatLive2DLoadSession *bongo_cat_live2d_load_session_begin(
    BongoCatLive2D *live2d, const char *model_dir, const char *setting_file,
    bool preset, const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->load_session_begin)
        return (BongoCatLive2DLoadSession *)active->load_session_begin(live2d,
            model_dir, setting_file, preset, render_options, progress, userdata,
            error);
    return STUB(load_session_begin)(live2d, model_dir, setting_file, preset,
        render_options, progress, userdata, error);
}

BongoCatLive2DStepResult bongo_cat_live2d_load_session_activate(
    BongoCatLive2DLoadSession *session, SDL_Window *window, void *context,
    BongoCatError *error) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->load_session_activate)
        return (BongoCatLive2DStepResult)active->load_session_activate(session,
            window, context, error);
    return STUB(load_session_activate)(session, window, context, error);
}

BongoCatLive2DStepResult bongo_cat_live2d_load_session_step(
    BongoCatLive2DLoadSession *session, const char *model_dir,
    const char *setting_file, BongoCatError *error) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->load_session_step)
        return (BongoCatLive2DStepResult)active->load_session_step(session,
            model_dir, setting_file, error);
    return STUB(load_session_step)(session, model_dir, setting_file, error);
}

void bongo_cat_live2d_load_session_end(BongoCatLive2DLoadSession *session) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->load_session_end) {
        active->load_session_end(session);
        return;
    }
    STUB(load_session_end)(session);
}

void bongo_cat_live2d_load_session_abort(BongoCatLive2DLoadSession *session) {
    const BongoCatRendererModuleOps *active = component_used ? ops() : NULL;
    if (active && active->load_session_abort) {
        active->load_session_abort(session);
        return;
    }
    STUB(load_session_abort)(session);
}

/*
 * 分步加载的分段计时摘要。
 *
 * 实现搬到组件之后, "每个阶段花了多久"只有组件自己知道(它按阶段采样); 宿主这一侧**不再
 * 假装**能打印摘要 —— 打印一堆零会让"模型加载慢"的排查方向完全跑偏。这里只留一条 debug
 * 日志说明明细在组件侧(组件经宿主日志服务输出), 保留"能看见"这一点。
 */
void bongo_cat_live2d_load_session_log_timing(
    const BongoCatLive2DLoadSession *session, const char *model_id,
    uint64_t total_ns) {
    (void)session;
    if (!component_used) return;
    const char *component = bongo_cat_live2d_component_path();
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
        "[live2d] 模型 %s 分步加载总耗时 %.1f ms(分段明细由组件经日志输出: %s)",
        model_id ? model_id : "(unknown)", (double)total_ns / 1000000.0,
        component && component[0] ? component : "(component)");
}
