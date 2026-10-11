#ifndef BONGO_CAT_PLATFORM_H
#define BONGO_CAT_PLATFORM_H

#include "bongo_cat/config.h"
#include "bongo_cat/input.h"

#include <stdint.h>

typedef struct SDL_Window SDL_Window;

typedef struct BongoCatPlatform {
    SDL_Window *window;
    BongoCatInputState *input;
    void *native;
    void *presenter;
    uint32_t wake_event_type;
    float window_opacity;
    bool hover_hide_unavailable;
    /* 只在采集软件里可见 (见 bongo_cat_platform_set_capture_only) */
    bool capture_only;
    /*
     * window.glTransparency 的镜像: "让背景真正透明"这个设置项 (仅 Windows + OpenGL
     * 后端有意义)。平台层看不到 BongoCatSettings, 而每帧的穿透同步
     * (bongo_cat_window_sync_click_through) 又要用到它, 所以由运行时把它同步进来。
     * 其它平台忽略这个字段。
     */
    bool gl_transparency;
} BongoCatPlatform;

typedef enum BongoCatMenuAction {
    BONGO_CAT_MENU_NONE,
    BONGO_CAT_MENU_PREFERENCES,
    BONGO_CAT_MENU_HIDE,
    BONGO_CAT_MENU_PASS_THROUGH,
    BONGO_CAT_MENU_ALWAYS_ON_TOP,
    BONGO_CAT_MENU_SCALE_50,
    BONGO_CAT_MENU_SCALE_60,
    BONGO_CAT_MENU_SCALE_70,
    BONGO_CAT_MENU_SCALE_80,
    BONGO_CAT_MENU_SCALE_90,
    BONGO_CAT_MENU_SCALE_100,
    BONGO_CAT_MENU_SCALE_110,
    BONGO_CAT_MENU_SCALE_120,
    BONGO_CAT_MENU_SCALE_130,
    BONGO_CAT_MENU_SCALE_140,
    BONGO_CAT_MENU_SCALE_150,
    BONGO_CAT_MENU_SCALE_160,
    BONGO_CAT_MENU_SCALE_170,
    BONGO_CAT_MENU_SCALE_180,
    BONGO_CAT_MENU_SCALE_190,
    BONGO_CAT_MENU_SCALE_200,
    BONGO_CAT_MENU_OPACITY_10,
    BONGO_CAT_MENU_OPACITY_20,
    BONGO_CAT_MENU_OPACITY_30,
    BONGO_CAT_MENU_OPACITY_40,
    BONGO_CAT_MENU_OPACITY_50,
    BONGO_CAT_MENU_OPACITY_60,
    BONGO_CAT_MENU_OPACITY_70,
    BONGO_CAT_MENU_OPACITY_80,
    BONGO_CAT_MENU_OPACITY_90,
    BONGO_CAT_MENU_OPACITY_100,
    BONGO_CAT_MENU_EXIT,
    BONGO_CAT_MENU_MODEL_ADD,
    BONGO_CAT_MENU_REMOVE_PET,
    BONGO_CAT_MENU_MODEL_FIRST = 1000,
    BONGO_CAT_MENU_MOTION_FIRST = 2000,
    BONGO_CAT_MENU_EXPRESSION_FIRST = 3000,
    BONGO_CAT_MENU_AUDIO_FIRST = 4000
} BongoCatMenuAction;
typedef void (*BongoCatMenuPreview)(void *userdata, BongoCatMenuAction action);

typedef struct BongoCatMenuLabels {
    const char *preferences, *hide, *pass_through, *always_on_top;
    const char *window_size, *opacity, *model, *add_model, *exit;
    const char *wheel_size_hint, *wheel_opacity_hint, *motion, *expression;
    const char *const *model_names;
    const char (*motion_names)[BONGO_CAT_MENU_LABEL_CAP];
    const char (*expression_names)[BONGO_CAT_MENU_LABEL_CAP];
    const bool *motion_checked;
    size_t model_count, current_model, motion_count;
    size_t expression_count, current_expression;
    float scale_percent, opacity_percent;
    bool pass_through_checked, always_on_top_checked, dark_theme;
    BongoCatMenuPreview preview;
    void (*preview_tick)(void *userdata);
    BongoCatMenuPreview restore;
    void *preview_userdata;
    const char *remove_pet;
    bool remove_pet_visible;
    const char *audio;
    const char (*audio_names)[BONGO_CAT_MENU_LABEL_CAP];
    const bool *audio_checked;
    size_t audio_count;
    const char *const *model_cover_directories;
} BongoCatMenuLabels;

typedef void (*BongoCatTrayClick)(void *userdata);
typedef void (*BongoCatModalTick)(void *userdata);
typedef void (*BongoCatTrayRestore)(void *userdata);

BongoCatResult bongo_cat_platform_init(BongoCatPlatform *platform, SDL_Window *window,
    BongoCatInputState *input, BongoCatError *error);
void bongo_cat_platform_shutdown(BongoCatPlatform *platform);
void bongo_cat_platform_set_click_through(BongoCatPlatform *platform,
    bool forced, bool pointer_transparent);
/*
 * 下发"透明背景"设置 (平台层读 platform->gl_transparency 这个镜像)。
 *
 * 只在设置/环境变量强制覆盖/采集模式真的变化时调用 —— 这是代理分层呈现的总开关,
 * 绝不能放进每帧的命中测试或点击穿透同步里: 那些路径的参数逐帧都在变(光标在猫身上
 * 还是透明处), 一旦让它们参与决策, 开关就会跟着光标来回翻。
 * 只有 Windows 的 OpenGL 后端有等价机制, 其它平台是 no-op。
 */
void bongo_cat_platform_apply_transparency(BongoCatPlatform *platform);
bool bongo_cat_platform_set_opacity(BongoCatPlatform *platform, float opacity);
float bongo_cat_platform_get_opacity(const BongoCatPlatform *platform);
bool bongo_cat_platform_present(BongoCatPlatform *platform, int width, int height);
bool bongo_cat_platform_frame_alpha(const BongoCatPlatform *platform,
    int width, int height, int x, int y, uint8_t *alpha);
void bongo_cat_platform_set_visible(BongoCatPlatform *platform, bool visible);
/* 只在采集软件里可见: 桌面上隐藏窗口, 但仍参与渲染与采集 (Windows: DWM 隐藏)。
   返回 false 表示当前平台不支持。 */
bool bongo_cat_platform_set_capture_only(BongoCatPlatform *platform,
    bool enabled);
bool bongo_cat_platform_capture_only_supported(void);
/* 管理员权限: 查询 / 重启提权 / "不是管理员才重启" (Windows 用 UAC, 其它平台恒 false) */
bool bongo_cat_platform_is_elevated(void);
bool bongo_cat_platform_relaunch_elevated(void);
bool bongo_cat_platform_relaunch_elevated_if_needed(void);
bool bongo_cat_platform_pointer_local(BongoCatPlatform *platform, double screen_x,
    double screen_y, float *local_x, float *local_y);
/* Reports a foreground application's fixed/locked system cursor state. */
bool bongo_cat_platform_pointer_locked(BongoCatPlatform *platform);
bool bongo_cat_platform_relative_pointer(BongoCatPlatform *platform,
    double *x, double *y);
void bongo_cat_platform_relative_pointer_reset(BongoCatPlatform *platform);
void bongo_cat_platform_relative_pointer_release(BongoCatPlatform *platform);
void bongo_cat_platform_set_always_on_top(BongoCatPlatform *platform, bool enabled);
void bongo_cat_platform_raise_window(SDL_Window *window);
/* Configure platform-native chrome for the preferences window when available. */
void bongo_cat_platform_configure_preferences_window(SDL_Window *window);
bool bongo_cat_platform_open_directory(const char *path);
bool bongo_cat_platform_set_geometry(BongoCatPlatform *platform,
    int x, int y, int width, int height);
void bongo_cat_platform_begin_drag(BongoCatPlatform *platform,
    BongoCatModalTick modal_tick, void *userdata);
bool bongo_cat_platform_dynamic_hit_supported(void);
void bongo_cat_platform_set_tray_callbacks(void *tray,
    BongoCatTrayClick left_click, BongoCatModalTick modal_tick,
    BongoCatTrayRestore restore, void *userdata);
bool bongo_cat_platform_single_instance_begin(void);
bool bongo_cat_platform_single_instance_take_wake(void);
bool bongo_cat_platform_update_shutdown_argument(int argc, char **argv);
bool bongo_cat_platform_single_instance_take_update_shutdown(void);
void bongo_cat_platform_single_instance_end(void);
BongoCatResult bongo_cat_platform_set_autostart(bool enabled, BongoCatError *error);
BongoCatMenuAction bongo_cat_platform_context_menu(BongoCatPlatform *platform,
    const BongoCatMenuLabels *labels);
BongoCatResult bongo_cat_platform_embedded_assets(const char *target, BongoCatError *error);

#ifdef __APPLE__
/* Read the permission macOS grants the app right now, without prompting.
   Callers refresh on a user action or a focus change, never per frame. */
bool bongo_cat_platform_input_monitoring_authorized(void);
/* Ask macOS for Input Monitoring; only an explicit user action may call it. */
bool bongo_cat_platform_input_monitoring_request(void);
#endif

#endif
