/*
 * 没有 Cubism SDK 时的替代实现 (诊断构建)。
 *
 * 它的目的不是"还能跑 Live2D", 而是让整个程序在没有 SDK 的情况下依然能编译、链接并启动
 * 到主界面, 从而区分"是渲染的问题"还是"是模型/SDK 的问题"。
 *
 * 因此约定是: 查询类一律返回 false/空, 但凡"返回 false 会让上层把某个功能面整个关掉"的
 * 查询, 要返回中性值 —— 见末尾 shaders_available 与 motion_visible 的说明。另外这里所有
 * 函数都必须与真实的 C 接口签名逐字一致, 否则链接期才发现对不上。
 */
#include "bongo_cat/live2d_vulkan.h"
#include "bongo_cat/model.h"
#include "model_import_manifest.h"
#include "bongo_cat/json.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_video.h>
#include <stdlib.h>

/* 桩运行时只保留"加载过没有"和最近一次的画布尺寸: 前者让 ready() 的语义与真实实现一致,
 * 后者让视口查询返回一个合法的 (虽然没用) 矩形, 而不是 0 尺寸把上层布局算崩。 */
struct BongoCatLive2D {
    int width;
    int height;
    bool loaded;
};

static BongoCatLive2D *create_runtime(const char *asset_root,
    BongoCatError *error) {
    static bool warning_logged;
    (void)asset_root;
    if (!warning_logged) {
        warning_logged = true;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Cubism SDK unavailable: diagnostic backend active; Live2D "
            "rendering, animation, pointer tracking, and cover generation "
            "are disabled");
    }
    BongoCatLive2D *value = calloc(1, sizeof(*value));
    if (!value) bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "Cannot allocate Live2D runtime");
    return value;
}

BongoCatLive2D *bongo_cat_live2d_create(const char *asset_root,
    BongoCatError *error) {
    return create_runtime(asset_root, error);
}

void bongo_cat_live2d_destroy(BongoCatLive2D *live2d) { free(live2d); }

BongoCatResult bongo_cat_live2d_load(BongoCatLive2D *live2d, const char *model_dir,
    const char *setting_file, bool preset,
    const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata,
    BongoCatError *error) {
    (void)preset; (void)render_options;
    if (!live2d || !model_dir || !setting_file) return BONGO_CAT_ERROR_ARGUMENT;
    if (progress) progress(userdata, 0.1f);
    char path[BONGO_CAT_PATH_CAP];
    yyjson_doc *document = bongo_cat_path_join(path, sizeof(path), model_dir,
        setting_file) ? bongo_cat_model_json_read(path, NULL) : NULL;
    bool valid = bongo_cat_import_manifest_document_valid(model_dir, document, true);
    yyjson_doc_free(document);
    if (!valid) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Model manifest or required assets are invalid: %s", setting_file);
        return BONGO_CAT_ERROR_FORMAT;
    }
    live2d->loaded = true;
    if (progress) progress(userdata, 1.0f);
    return BONGO_CAT_OK;
}

/* 分步加载会话的桩: 把一次性版本里那段"校验清单 -> 置 loaded"的语义原样搬到一步之内 ——
 * 诊断构建没有真资源要读, 所以第一步就完成, 但**不能**返回失败: 那会让每一次换模型都弹出
 * "加载失败", 而这里其实根本没有加载失败这回事。 */
struct BongoCatLive2DLoadSession {
    BongoCatLive2D *runtime;
    bool finished;
};

BongoCatLive2DLoadSession *bongo_cat_live2d_load_session_begin(
    BongoCatLive2D *live2d, const char *model_dir, const char *setting_file,
    bool preset, const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error) {
    (void)preset; (void)render_options; (void)progress; (void)userdata;
    if (!live2d || !model_dir || !setting_file) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Cannot load a model without a runtime and a model directory");
        return NULL;
    }
    BongoCatLive2DLoadSession *session = calloc(1, sizeof(*session));
    if (!session) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate the Live2D load session");
        return NULL;
    }
    session->runtime = live2d;
    return session;
}

BongoCatLive2DStepResult bongo_cat_live2d_load_session_activate(
    BongoCatLive2DLoadSession *session, SDL_Window *window, void *context,
    BongoCatError *error) {
    (void)window; (void)context; (void)error;
    return session ? BONGO_CAT_LIVE2D_STEP_WORKING : BONGO_CAT_LIVE2D_STEP_FAILED;
}

BongoCatLive2DStepResult bongo_cat_live2d_load_session_step(
    BongoCatLive2DLoadSession *session, const char *model_dir,
    const char *setting_file, BongoCatError *error) {
    if (!session) return BONGO_CAT_LIVE2D_STEP_FAILED;
    if (session->finished) return BONGO_CAT_LIVE2D_STEP_DONE;
    char path[BONGO_CAT_PATH_CAP];
    yyjson_doc *document = bongo_cat_path_join(path, sizeof(path), model_dir,
        setting_file) ? bongo_cat_model_json_read(path, NULL) : NULL;
    bool valid = bongo_cat_import_manifest_document_valid(model_dir, document, true);
    yyjson_doc_free(document);
    if (!valid) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Model manifest or required assets are invalid: %s", setting_file);
        return BONGO_CAT_LIVE2D_STEP_FAILED;
    }
    session->runtime->loaded = true;
    session->finished = true;
    return BONGO_CAT_LIVE2D_STEP_DONE;
}

void bongo_cat_live2d_load_session_end(BongoCatLive2DLoadSession *session) {
    free(session);
}

void bongo_cat_live2d_load_session_abort(BongoCatLive2DLoadSession *session) {
    free(session);
}

bool bongo_cat_live2d_ready(const BongoCatLive2D *live2d) {
    return live2d && live2d->loaded;
}

bool bongo_cat_live2d_canvas_size(const BongoCatLive2D *live2d,
    int *width, int *height) {
    (void)live2d; (void)width; (void)height; return false;
}

bool bongo_cat_live2d_frame(const BongoCatLive2D *live2d,
    BongoCatLive2DFrame *frame) {
    if (!live2d || !frame) return false;
    *frame = (BongoCatLive2DFrame){0};
    return true;
}

bool bongo_cat_live2d_viewport(const BongoCatLive2D *live2d,
    int *x, int *y, int *width, int *height) {
    if (!live2d || !x || !y || !width || !height) return false;
    *x = 0;
    *y = 0;
    *width = live2d->width;
    *height = live2d->height;
    return true;
}

void bongo_cat_live2d_resize(BongoCatLive2D *live2d, int width, int height) {
    if (!live2d) return;
    live2d->width = width;
    live2d->height = height;
}
void bongo_cat_live2d_reshape(BongoCatLive2D *live2d, int width, int height) {
    bongo_cat_live2d_resize(live2d, width, height);
}

bool bongo_cat_live2d_update(BongoCatLive2D *live2d, float delta_seconds) {
    (void)live2d; (void)delta_seconds; return false;
}
void bongo_cat_live2d_draw(BongoCatLive2D *live2d) { (void)live2d; }
void bongo_cat_live2d_set_mirror(BongoCatLive2D *live2d, bool mirror) {
    (void)live2d; (void)mirror;
}
void bongo_cat_live2d_set_vertical_flip(BongoCatLive2D *live2d, bool flipped) {
    (void)live2d; (void)flipped;
}
void bongo_cat_live2d_set_render_options(BongoCatLive2D *live2d,
    const BongoCatLive2DRenderOptions *options) {
    (void)live2d; (void)options;
}
void bongo_cat_live2d_set_dragging(BongoCatLive2D *live2d, float x, float y) {
    (void)live2d; (void)x; (void)y;
}
void bongo_cat_live2d_set_centered_dragging(BongoCatLive2D *live2d,
    float x, float y) { (void)live2d; (void)x; (void)y; }
void bongo_cat_live2d_prepare_viewer_audit(BongoCatLive2D *live2d) { (void)live2d; }
bool bongo_cat_live2d_prepare_cover_capture(BongoCatLive2D *live2d) {
    return live2d && live2d->loaded;
}
bool bongo_cat_live2d_set_parameter(BongoCatLive2D *live2d, const char *id, float value) {
    (void)live2d; (void)id; (void)value; return false;
}
bool bongo_cat_live2d_parameter(BongoCatLive2D *value, const char *id, BongoCatParameterRange *range) {
    (void)value; (void)id; (void)range; return false;
}
bool bongo_cat_live2d_start_motion(BongoCatLive2D *value, const char *group, int index) {
    (void)value; (void)group; (void)index; return false;
}
bool bongo_cat_live2d_restore_motion_state(BongoCatLive2D *value,
    const char *group, int index) {
    (void)value; (void)group; (void)index; return false;
}
bool bongo_cat_live2d_preview_motion(BongoCatLive2D *value,
    const char *group, int index) {
    (void)value; (void)group; (void)index; return false;
}
bool bongo_cat_live2d_restore_motion_preview(BongoCatLive2D *value) {
    (void)value; return false;
}
bool bongo_cat_live2d_commit_motion_preview(BongoCatLive2D *value,
    const char *group, int index) {
    (void)value; (void)group; (void)index; return false;
}
bool bongo_cat_live2d_motion_selected(const BongoCatLive2D *value,
    const char *group, int index) {
    (void)value; (void)group; (void)index; return false;
}
bool bongo_cat_live2d_motion_persistent(const BongoCatLive2D *value,
    const char *group, int index) {
    (void)value; (void)group; (void)index; return false;
}
/* 唯一返回 true 的动作查询: 返回 false 会被上层理解成"这条动作是配对隐藏项", 于是把它从
 * 动作列表里摘掉 —— 诊断构建下用户会以为动作丢了。 */
bool bongo_cat_live2d_motion_visible(const BongoCatLive2D *value,
    const char *group, int index) {
    (void)value; (void)group; (void)index; return true;
}
bool bongo_cat_live2d_motion_same_toggle(const BongoCatLive2D *value,
    const char *left_group, int left_index,
    const char *right_group, int right_index) {
    (void)value; (void)left_group; (void)left_index;
    (void)right_group; (void)right_index; return false;
}
bool bongo_cat_live2d_set_expression(BongoCatLive2D *value, int index) {
    (void)value; (void)index; return false;
}
int bongo_cat_live2d_expression(const BongoCatLive2D *value) {
    (void)value; return -1;
}
bool bongo_cat_live2d_visual_state(const BongoCatLive2D *value,
    BongoCatLive2DVisualState *state) {
    (void)value; (void)state; return false;
}

/* Vulkan 桥的桩: 没有 Cubism (或没有 Vulkan 后端) 时一律失败, 上层回退 OpenGL。 */
bool bongo_cat_live2d_vulkan_attach(const BongoCatVkNativeInfo *info, int width, int height) {
    (void)info; (void)width; (void)height;
    return false;
}

void *bongo_cat_live2d_vulkan_create_texture(const void *pixels, int width,
    int height) {
    (void)pixels; (void)width; (void)height;
    return NULL;
}

void bongo_cat_live2d_vulkan_destroy_texture(void *texture) { (void)texture; }

bool bongo_cat_live2d_vulkan_bind_texture(void *renderer, void *texture) {
    (void)renderer; (void)texture;
    return false;
}

void *bongo_cat_live2d_vulkan_create_texture_attached(const void *pixels,
    int width, int height) {
    (void)pixels; (void)width; (void)height;
    return NULL;
}

bool bongo_cat_live2d_vulkan_ready(void) { return false; }
bool bongo_cat_live2d_vulkan_begin_frame(void) { return false; }
bool bongo_cat_live2d_vulkan_end_frame(void) { return false; }
void bongo_cat_live2d_vulkan_detach(void) {}

bool bongo_cat_live2d_vulkan_target_info(void **image, void **view, void **sampler,
    int *width, int *height) {
    (void)image; (void)view; (void)sampler; (void)width; (void)height;
    return false;
}

/* 没有 Cubism/Vulkan 的构建走不到 Vulkan 模型路径, 这里一律当作"可用" */
bool bongo_cat_live2d_vulkan_shaders_available(const char *assets_root) {
    (void)assets_root;
    return true;
}

uint64_t bongo_cat_live2d_vulkan_target_generation(void) { return 0; }

void bongo_cat_live2d_vulkan_release_target(void) {
}

void bongo_cat_live2d_vulkan_set_supersample(int mode) {
    (void)mode;
}

void bongo_cat_live2d_vulkan_set_shader_root(const char *assets_root) {
    (void)assets_root;
}
