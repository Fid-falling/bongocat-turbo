#ifndef BONGO_CAT_MODEL_H
#define BONGO_CAT_MODEL_H

#include "bongo_cat/config.h"

typedef enum BongoCatModelSourceFormat {
    BONGO_CAT_MODEL_SOURCE_UNKNOWN,
    BONGO_CAT_MODEL_SOURCE_BUILTIN,
    BONGO_CAT_MODEL_SOURCE_TAURI,
    BONGO_CAT_MODEL_SOURCE_MVER,
    BONGO_CAT_MODEL_SOURCE_MVER_PATCH
} BongoCatModelSourceFormat;

typedef enum BongoCatModelCapability {
    BONGO_CAT_MODEL_CAPABILITY_LIVE2D = 1u << 0,
    BONGO_CAT_MODEL_CAPABILITY_PREVIEW = 1u << 1,
    BONGO_CAT_MODEL_CAPABILITY_RUNTIME_ADAPTER = 1u << 2,
    BONGO_CAT_MODEL_CAPABILITY_INPUT_IMAGES = 1u << 3,
    BONGO_CAT_MODEL_CAPABILITY_KEYBOARD_INPUT = 1u << 4,
    BONGO_CAT_MODEL_CAPABILITY_GAMEPAD_INPUT = 1u << 5,
    BONGO_CAT_MODEL_CAPABILITY_BEHAVIORS = 1u << 6,
    BONGO_CAT_MODEL_CAPABILITY_AUDIO = 1u << 7,
    BONGO_CAT_MODEL_CAPABILITY_EFFECTS = 1u << 8,
    BONGO_CAT_MODEL_CAPABILITY_MVER_PROJECTION = 1u << 9,
    BONGO_CAT_MODEL_CAPABILITY_POINTER_OVERLAY = 1u << 10,
    BONGO_CAT_MODEL_CAPABILITY_IMAGE_PATCH = 1u << 11
} BongoCatModelCapability;

typedef struct BongoCatModelEntry {
    char id[BONGO_CAT_ID_CAP];
    char package_id[BONGO_CAT_ID_CAP];
    char content_digest[65];
    char family_id[BONGO_CAT_ID_CAP];
    char display_name[BONGO_CAT_ID_CAP];
    char directory[BONGO_CAT_PATH_CAP];
    char adapter_directory[BONGO_CAT_PATH_CAP];
    char storage_directory[BONGO_CAT_PATH_CAP];
    char setting_file[BONGO_CAT_PATH_CAP];
    BongoCatModelMode mode;
    BongoCatModelSourceFormat source_format;
    uint32_t capabilities;
    int adapter_schema;
    int adapter_generator;
    bool preset;
    bool managed;
} BongoCatModelEntry;

typedef struct BongoCatModelCatalog {
    BongoCatModelEntry entries[BONGO_CAT_MODEL_CAP];
    size_t count;
} BongoCatModelCatalog;

typedef enum BongoCatBehaviorKind {
    BONGO_CAT_BEHAVIOR_MOTION,
    BONGO_CAT_BEHAVIOR_EXPRESSION,
    BONGO_CAT_BEHAVIOR_SOUND,
    BONGO_CAT_BEHAVIOR_EFFECT
} BongoCatBehaviorKind;

typedef struct BongoCatBehaviorEntry {
    char id[BONGO_CAT_PATH_CAP];
    char label[BONGO_CAT_ID_CAP];
    char group[BONGO_CAT_ID_CAP];
    char sound[BONGO_CAT_PATH_CAP];
    char effect[BONGO_CAT_PATH_CAP];
    int index;
    BongoCatBehaviorKind kind;
    bool momentary;
    bool sound_overlap;
    bool sound_clear;
} BongoCatBehaviorEntry;

typedef struct BongoCatBehaviorCatalog {
    BongoCatBehaviorEntry entries[BONGO_CAT_BEHAVIOR_CAP];
    size_t count;
} BongoCatBehaviorCatalog;

#ifdef __cplusplus
extern "C" {
#endif

void bongo_cat_models_init(BongoCatModelCatalog *catalog);
BongoCatResult bongo_cat_models_scan(BongoCatModelCatalog *catalog, const char *root,
    bool preset, BongoCatError *error);
const BongoCatModelEntry *bongo_cat_models_find(const BongoCatModelCatalog *catalog,
    const char *id);
bool bongo_cat_model_adapter_metadata_path(const char *directory,
    char *path, size_t capacity);
const char *bongo_cat_model_default_name(const BongoCatModelEntry *entry);
const char *bongo_cat_model_name(const BongoCatSettings *settings,
    const BongoCatModelEntry *entry);
BongoCatResult bongo_cat_behaviors_load(BongoCatBehaviorCatalog *catalog,
    const BongoCatModelEntry *model, BongoCatError *error);

typedef struct BongoCatLive2D BongoCatLive2D;
typedef struct BongoCatParameterRange { float minimum, maximum, value; } BongoCatParameterRange;
typedef void (*BongoCatLive2DLoadProgress)(void *userdata, float progress);
/*
 * 诊断/测试钩子, 不是渲染输入: 报告上一帧实际画了多少 drawable / part / offscreen, 以及可见
 * 包络经投影变换到 NDC 后的范围。visual_state_ready_ 为假时 bongo_cat_live2d_visual_state()
 * 返回 false, 所以不能拿它判断"模型加载完了没有"。
 * fitted / visible 表示包络是否已经算出来, mver_projection 回显当前用的是哪套投影。
 */
typedef struct BongoCatLive2DVisualState {
    float fit_scale, fit_translate_x, fit_translate_y;
    float visible_min_x, visible_min_y, visible_max_x, visible_max_y;
    int drawable_count, drawable_visible, drawable_vertex_changed;
    int offscreen_count, offscreen_positive, part_count, part_positive;
    bool fitted, visible, mver_projection;
} BongoCatLive2DVisualState;

/* Extra transparent space around the authored model canvas, expressed as a
   fraction of that canvas dimension.  For example, top=0.5 reserves half a
   canvas height above the normal composition. */
/* 这四个值最终决定 bongo_cat_live2d_viewport() 返回的内容视口: 叠加层 (背景/按键/指针) 必须
 * 用同一个视口才能和模型对齐, 见 vk_backend.h 里关于叠加层视口的说明。 */
typedef struct BongoCatLive2DFrame {
    float left, top, right, bottom;
} BongoCatLive2DFrame;

/*
 * 允许传 NULL (表示全用默认值)。mver_projection 为真时走 Mver 授权的参考投影 (用
 * reference_width/height 与 projection_scale/offset_*), 为假则按画布自适应; auto_frame
 * 决定是否自动额外留边距 —— 经过授权标定的模型可以关掉它, 以保留作者原本的构图。
 */
typedef struct BongoCatLive2DRenderOptions {
    bool mver_projection;
    bool auto_frame;
    bool source_mirror;
    bool custom_pointer_bounds;
    bool pointer_left_handed;
    bool mouse_force_move;
    float mouse_speed;
    float projection_scale;
    float offset_x;
    float offset_y;
    int reference_width;
    int reference_height;
    int pointer_left;
    int pointer_top;
    int pointer_right;
    int pointer_bottom;
} BongoCatLive2DRenderOptions;

BongoCatLive2D *bongo_cat_live2d_create(const char *asset_root, BongoCatError *error);
void bongo_cat_live2d_destroy(BongoCatLive2D *live2d);
BongoCatResult bongo_cat_live2d_load(BongoCatLive2D *live2d, const char *model_dir,
    const char *setting_file, bool preset,
    const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata,
    BongoCatError *error);
bool bongo_cat_live2d_ready(const BongoCatLive2D *live2d);
/* Returns the authored pixel canvas size of the loaded model. */
bool bongo_cat_live2d_canvas_size(const BongoCatLive2D *live2d,
    int *width, int *height);
bool bongo_cat_live2d_frame(const BongoCatLive2D *live2d,
    BongoCatLive2DFrame *frame);
bool bongo_cat_live2d_viewport(const BongoCatLive2D *live2d,
    int *x, int *y, int *width, int *height);
void bongo_cat_live2d_resize(BongoCatLive2D *live2d, int width, int height);
void bongo_cat_live2d_reshape(BongoCatLive2D *live2d, int width, int height);
bool bongo_cat_live2d_update(BongoCatLive2D *live2d, float delta_seconds);
void bongo_cat_live2d_draw(BongoCatLive2D *live2d);
void bongo_cat_live2d_set_mirror(BongoCatLive2D *live2d, bool mirror);
void bongo_cat_live2d_set_render_options(BongoCatLive2D *live2d,
    const BongoCatLive2DRenderOptions *options);
void bongo_cat_live2d_set_dragging(BongoCatLive2D *live2d, float x, float y);
void bongo_cat_live2d_set_centered_dragging(BongoCatLive2D *live2d,
    float x, float y);
void bongo_cat_live2d_prepare_viewer_audit(BongoCatLive2D *live2d);
/* Synchronize drawables from the current state before a cover-only frame. */
bool bongo_cat_live2d_prepare_cover_capture(BongoCatLive2D *live2d);
bool bongo_cat_live2d_set_parameter(BongoCatLive2D *live2d, const char *id, float value);
bool bongo_cat_live2d_parameter(BongoCatLive2D *live2d, const char *id,
    BongoCatParameterRange *range);
/*
 * 动作 (motion) 的语义契约:
 *  - preview_motion / commit_motion_preview / restore_motion_preview 是一组**预览事务**
 *    (设置窗口里的试看): 没 commit 就结束预览, 必须调 restore 回滚。
 *  - motion_selected 是 UI 的勾选态; motion_persistent 表示该动作是"开/关"型 (可以停在终态)
 *    而不是一次性播放。
 *  - motion_visible 为假的条目是配对项 (另一个动作的"关"那侧), UI 不单独列出。
 */
bool bongo_cat_live2d_start_motion(BongoCatLive2D *live2d, const char *group, int index);
bool bongo_cat_live2d_restore_motion_state(BongoCatLive2D *live2d,
    const char *group, int index);
bool bongo_cat_live2d_preview_motion(BongoCatLive2D *live2d,
    const char *group, int index);
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
    const char *left_group, int left_index,
    const char *right_group, int right_index);
/* index == -1 表示清除表情 (走淡出, 不是硬切) */
bool bongo_cat_live2d_set_expression(BongoCatLive2D *live2d, int index);
int bongo_cat_live2d_expression(const BongoCatLive2D *live2d);
bool bongo_cat_live2d_visual_state(const BongoCatLive2D *live2d,
    BongoCatLive2DVisualState *state);

#ifdef __cplusplus
}
#endif

#endif
