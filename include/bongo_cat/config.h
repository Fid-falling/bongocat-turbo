#ifndef BONGO_CAT_CONFIG_H
#define BONGO_CAT_CONFIG_H

#include "bongo_cat/common.h"

#define BONGO_CAT_DEFAULT_MAX_FPS 60
/* max_fps 取这个值时不做帧率节流, 由垂直同步/驱动决定实际上限 */
#define BONGO_CAT_MAX_FPS_UNLIMITED 0
#define BONGO_CAT_DEFAULT_WINDOW_SCALE_PERCENT 100.0f
#define BONGO_CAT_DEFAULT_WINDOW_OPACITY_PERCENT 100.0f
#define BONGO_CAT_DEFAULT_RANDOM_EXPRESSION_SECONDS 5.0f
#define BONGO_CAT_DEFAULT_WINDOW_CORNER_PERCENT 6.0f
#define BONGO_CAT_DEFAULT_HIDE_FADE_SECONDS 0.3f
#define BONGO_CAT_MAX_HIDE_FADE_SECONDS 3.0f
#define BONGO_CAT_DEFAULT_EDGE_SNAP_PX 20
#define BONGO_CAT_MAX_EDGE_SNAP_PX 64

typedef enum BongoCatTheme { BONGO_CAT_THEME_AUTO, BONGO_CAT_THEME_LIGHT, BONGO_CAT_THEME_DARK } BongoCatTheme;
typedef enum BongoCatLanguage {
    BONGO_CAT_LANG_EN_US,
    BONGO_CAT_LANG_ZH_CN,
    BONGO_CAT_LANG_ZH_HANT,
    BONGO_CAT_LANG_FR_FR,
    BONGO_CAT_LANG_DE_DE,
    BONGO_CAT_LANG_JA_JP,
    BONGO_CAT_LANG_KO_KR,
    BONGO_CAT_LANG_PT_BR,
    BONGO_CAT_LANG_RU_RU,
    BONGO_CAT_LANG_ES_ES,
    BONGO_CAT_LANG_COUNT
} BongoCatLanguage;
typedef enum BongoCatModelMode {
    BONGO_CAT_MODE_STANDARD,
    BONGO_CAT_MODE_KEYBOARD,
    BONGO_CAT_MODE_GAMEPAD
} BongoCatModelMode;
typedef enum BongoCatObsBackgroundColor {
    BONGO_CAT_OBS_BACKGROUND_GREEN,
    BONGO_CAT_OBS_BACKGROUND_BLUE,
    BONGO_CAT_OBS_BACKGROUND_RED,
    BONGO_CAT_OBS_BACKGROUND_MAGENTA,
    BONGO_CAT_OBS_BACKGROUND_COLOR_COUNT
} BongoCatObsBackgroundColor;

/* 渲染后端。AUTO 交给运行时按可用性挑选; OPENGL 是始终可用的那个。
   VULKAN 只有以 BONGO_CAT_ENABLE_VULKAN 构建时才可能生效。 */
typedef enum BongoCatRenderer {
    BONGO_CAT_RENDERER_AUTO,
    BONGO_CAT_RENDERER_OPENGL,
    BONGO_CAT_RENDERER_VULKAN,
    BONGO_CAT_RENDERER_COUNT
} BongoCatRenderer;

typedef struct BongoCatModelPreferences {
    bool multiple_pets;
    bool mirror;
    bool mouse_mirror;
    bool mouse_centered;
    bool ignore_mouse;
    int max_fps;
    BongoCatRenderer renderer;
} BongoCatModelPreferences;

typedef struct BongoCatWindowPreferences {
    bool pass_through;
    bool always_on_top;
    bool hide_on_hover;
    bool keep_in_screen;
    /* 拖动窗口靠近显示器边缘时, 在阈值内吸附贴边。 */
    bool edge_snap;
    /* edge_snap 开启时: 窗口某条边距显示器边缘不超过这个像素数就贴平吸附。 */
    int edge_snap_threshold;
    /*
     * 只在录屏/直播软件里可见: 桌面上看不见这个窗口, 但 OBS 之类的采集
     * (窗口采集 WGC / 游戏采集) 仍然拿得到画面。Windows 上通过 DWM 隐藏
     * (DWMWA_CLOAK) 实现, 其它平台暂不支持。
     */
    bool capture_only;
    /*
     * 性能相关 (设置界面的[性能]区):
     *   supersample     0=自动 (窗口面积大时降到 1x 省显存) 1=质量 (2x) 2=省内存 (1x)
     *   font_raster_cap 1=字体图集栅格倍率封顶 2x (HiDPI 下 4x 就是 16 倍像素)
     * 两个都是 int 而不是 bool: 配置读取走可选的 read_int, 老配置文件缺键时保持默认。
     */
    int supersample;
    int font_raster_cap;
    /* 1 = 关掉界面发光/阴影 (浅色主题每帧省约 5ms) */
    int disable_effects;
    /*
     * 1 = 让宠物背景在桌面上真正透明 (默认 0 = 关闭)。
     *
     * 为什么需要它: OpenGL 模式下普通窗口的缓冲 alpha 会被 DWM 忽略, 猫以外的区域
     * 只能显示成黑块。开启后改走"代理分层呈现"(真实窗口压成全透明 + 一个分层代理
     * 窗口显示抓到的帧, 见 windows_layered_window.c)。
     * 为什么是 int 而不是 bool: 配置读取走可选的 read_int, 老配置文件缺键时保持
     * 默认值 0, 不会因为"键不存在"判定配置损坏。写入侧同样按 int 存。
     * 代价: 录屏软件按标题采集"真实窗口"时拿到的是全透明画面, 要采代理窗口
     * (设置界面与文档里都写明了这一点)。
     */
    int gl_transparency;
    bool obs_background;
    bool random_expression;
    bool rounded_corners;
    BongoCatObsBackgroundColor obs_background_color;
    float hide_delay_seconds;
    float hide_fade_seconds;
    float random_expression_interval_seconds;
    float corner_radius_percent;
} BongoCatWindowPreferences;

typedef struct BongoCatWindowState {
    bool visible;
    bool position_known;
    float scale_percent;
    float opacity_percent;
    int x;
    int y;
    int width;
    int height;
    /* The authored composition size inside width/height.  The outer window
       may be larger to hold expression geometry outside the base canvas. */
    int content_width;
    int content_height;
} BongoCatWindowState;

typedef struct BongoCatApplicationPreferences {
    bool autostart;
    /*
     * 1 = 以管理员身份运行 (启动时请求提权, 与 BongoCat-X 的 run_as_admin 对齐)。
     * 默认 0 = 普通权限。开启且当前未提权时切换会立即以管理员身份重启; 关掉只影响
     * 下一次启动, 不影响正在运行的进程。开机自启 + 开启本项 = 每次登录弹一次 UAC。
     */
    bool run_as_admin;
    bool tray_visible;
    BongoCatTheme theme;
    BongoCatLanguage language;
} BongoCatApplicationPreferences;

typedef struct BongoCatShortcutPreferences {
    char toggle_pet_visibility[BONGO_CAT_SHORTCUT_CAP];
    char visible_preferences[BONGO_CAT_SHORTCUT_CAP];
    char mirror[BONGO_CAT_SHORTCUT_CAP];
    char pass_through[BONGO_CAT_SHORTCUT_CAP];
    char always_on_top[BONGO_CAT_SHORTCUT_CAP];
} BongoCatShortcutPreferences;

typedef struct BongoCatBehaviorShortcut {
    char id[BONGO_CAT_BEHAVIOR_ID_CAP];
    char shortcut[BONGO_CAT_SHORTCUT_CAP];
    char label[BONGO_CAT_ID_CAP];
    bool shortcut_disabled;
} BongoCatBehaviorShortcut;

typedef struct BongoCatModelLabel {
    char id[BONGO_CAT_ID_CAP];
    char label[BONGO_CAT_ID_CAP];
} BongoCatModelLabel;

typedef struct BongoCatRemovedModel {
    char id[BONGO_CAT_ID_CAP];
} BongoCatRemovedModel;

typedef struct BongoCatActiveBehavior {
    char model_id[BONGO_CAT_ID_CAP];
    char behavior_id[BONGO_CAT_BEHAVIOR_ID_CAP];
} BongoCatActiveBehavior;

#define BONGO_CAT_SETTINGS_EXTENSIONS_CAP 4096

typedef struct BongoCatSettings {
    BongoCatModelPreferences model;
    BongoCatWindowPreferences window;
    BongoCatApplicationPreferences app;
    BongoCatShortcutPreferences shortcuts;
    /* Optional override for the cache directory. Empty means the platform
       default cache location is used. Written to settings.json. */
    char cache_root[BONGO_CAT_PATH_CAP];
    BongoCatBehaviorShortcut behavior_shortcuts[BONGO_CAT_BEHAVIOR_BINDING_CAP];
    size_t behavior_shortcut_count;
    BongoCatModelLabel model_labels[BONGO_CAT_MODEL_CAP];
    size_t model_label_count;
    BongoCatRemovedModel removed_models[BONGO_CAT_MODEL_CAP];
    size_t removed_model_count;
    char extensions_json[BONGO_CAT_SETTINGS_EXTENSIONS_CAP];
} BongoCatSettings;

typedef struct BongoCatSessionState {
    BongoCatWindowState window;
    char active_model_id[BONGO_CAT_ID_CAP];
    int last_update_check_day;
    char last_update_check_version[BONGO_CAT_UPDATE_VERSION_CAP];
    char available_update_version[BONGO_CAT_UPDATE_VERSION_CAP];
    char additional_model_ids[BONGO_CAT_ADDITIONAL_MODEL_CAP][BONGO_CAT_ID_CAP];
    size_t additional_model_count;
    BongoCatActiveBehavior active_behaviors[BONGO_CAT_BEHAVIOR_BINDING_CAP];
    size_t active_behavior_count;
} BongoCatSessionState;

#ifdef __cplusplus
extern "C" {
#endif

void bongo_cat_settings_defaults(BongoCatSettings *settings);
void bongo_cat_settings_validate(BongoCatSettings *settings);
void bongo_cat_session_defaults(BongoCatSessionState *session);
void bongo_cat_session_validate(BongoCatSessionState *session);
bool bongo_cat_session_model_active(const BongoCatSessionState *session,
    const char *model_id);
bool bongo_cat_session_add_model(BongoCatSessionState *session,
    const char *model_id);
bool bongo_cat_session_remove_model(BongoCatSessionState *session,
    const char *model_id);
void bongo_cat_session_clear_additional_models(BongoCatSessionState *session);
bool bongo_cat_settings_shortcut_conflicts(const BongoCatSettings *settings,
    const char *shortcut, const char *exclude);
const char *bongo_cat_settings_model_label(const BongoCatSettings *settings,
    const char *id);
bool bongo_cat_settings_set_model_label(BongoCatSettings *settings,
    const char *id, const char *label);
bool bongo_cat_settings_model_removed(const BongoCatSettings *settings,
    const char *id);
bool bongo_cat_settings_set_model_removed(BongoCatSettings *settings,
    const char *id, bool removed);
bool bongo_cat_settings_restore_model_package(BongoCatSettings *settings,
    const char *package_id);
BongoCatResult bongo_cat_settings_load(const char *path,
    BongoCatSettings *settings, BongoCatError *error);
BongoCatResult bongo_cat_settings_save(const char *path,
    const BongoCatSettings *settings, BongoCatError *error);
BongoCatResult bongo_cat_session_load(const char *path,
    BongoCatSessionState *session, BongoCatError *error);
BongoCatResult bongo_cat_session_save(const char *path,
    const BongoCatSessionState *session, BongoCatError *error);
const char *bongo_cat_theme_name(BongoCatTheme value);
const char *bongo_cat_language_name(BongoCatLanguage value);
const char *bongo_cat_renderer_name(BongoCatRenderer value);
bool bongo_cat_renderer_parse(const char *name, BongoCatRenderer *value);
bool bongo_cat_language_parse(const char *name, BongoCatLanguage *value);
bool bongo_cat_language_from_locale(const char *language,
    const char *country, BongoCatLanguage *value);
const char *bongo_cat_mode_name(BongoCatModelMode value);
const char *bongo_cat_obs_background_color_name(
    BongoCatObsBackgroundColor value);
uint32_t bongo_cat_obs_background_color_rgb(
    BongoCatObsBackgroundColor value);

#ifdef __cplusplus
}
#endif

#endif