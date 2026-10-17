#ifndef BONGO_CAT_CONFIG_H
#define BONGO_CAT_CONFIG_H

#include "bongo_cat/common.h"

#define BONGO_CAT_DEFAULT_MAX_FPS 60
/* max_fps 取这个值时不做帧率节流, 由垂直同步/驱动决定实际上限 */
#define BONGO_CAT_MAX_FPS_UNLIMITED 0
/*
 * "垂直同步"开关的三态取值。0/1 是用户显式选的开关状态, -1 是"老配置里还没有这个键"
 * 的哨兵值 —— 它只在读取阶段存在, 由 bongo_cat_settings_validate() 按兼容规则收口。
 * 保留三态的原因见 config_defaults.c 里收口处的注释: 老版本里垂直同步并不是一个独立
 * 设置, 而是由最大帧率推出来的 (见 frame_clock.c), 不照搬那条规则老用户升级后会突变。
 */
#define BONGO_CAT_VSYNC_DEFAULT (-1)
/* 兼容规则: 没有独立开关时, 限帧(30/60)原本就开同步, "无限"原本就关同步。 */
#define BONGO_CAT_VSYNC_FOR_MAX_FPS(max_fps) \
    ((max_fps) != BONGO_CAT_MAX_FPS_UNLIMITED)
#define BONGO_CAT_DEFAULT_WINDOW_SCALE_PERCENT 100.0f
#define BONGO_CAT_DEFAULT_WINDOW_OPACITY_PERCENT 100.0f
#define BONGO_CAT_DEFAULT_RANDOM_EXPRESSION_SECONDS 5.0f
#define BONGO_CAT_DEFAULT_RANDOM_MOTION_SECONDS 5.0f
#define BONGO_CAT_DEFAULT_RANDOM_SOUND_SECONDS 5.0f
#define BONGO_CAT_DEFAULT_SEQUENTIAL_MODEL_SECONDS 600.0f
#define BONGO_CAT_DEFAULT_WINDOW_CORNER_PERCENT 6.0f
#define BONGO_CAT_DEFAULT_HIDE_FADE_SECONDS 0.3f
#define BONGO_CAT_MAX_HIDE_FADE_SECONDS 3.0f
#define BONGO_CAT_DEFAULT_EDGE_SNAP_PX 20
#define BONGO_CAT_MAX_EDGE_SNAP_PX 64
/* 音效总音量 (0-100)。100 = 与加入这个设置之前完全一致 (miniaudio 引擎默认满音量)。 */
#define BONGO_CAT_SOUND_VOLUME_DEFAULT 100
/* 每拍速度乘 (100-值)/100: 0 表示不衰减 —— 恒速滑行且撞边不损失速度 (就是设置页里
   "一直滑动"打开时的效果), 越大人越快停下。合并 PR#4 后默认从 0 改成 15。 */
#define BONGO_CAT_DEFAULT_SLIDE_SLOWDOWN_PERCENT 15
#define BONGO_CAT_MAX_SLIDE_SLOWDOWN_PERCENT 99
/*
 * 多键显示: 同一侧同时按住的按键全部画出来 (而不是每侧只保留最后按下的那一个)。
 * 每侧最多显示 BONGO_CAT_MULTI_KEY_LIMIT_DEFAULT 个; 超出的按键仍然保留"按住"状态,
 * 松开前面的键之后自动补位 (与 BongoCat-X 的 maxKeys 语义一致)。
 */
#define BONGO_CAT_MULTI_KEY_LIMIT_DEFAULT 5
#define BONGO_CAT_MULTI_KEY_LIMIT_MAX 8
/*
 * 渲染精度 (设置 → 性能 → 渲染精度): 以 N 倍分辨率渲染这一帧, 再用线性过滤缩回窗口
 * 尺寸 —— 2.0 = 2×宽 × 2×高 渲染后降采样, 0.5 = 半分辨率渲染后放大, 1.0 = 与窗口 1:1。
 * 默认 1.0: 不设该键的老配置走的就是"直接按窗口尺寸渲染", 与 1.17.0 逐像素一致。
 * 上下限是产品决定 (0.1 再低只剩几个像素, 4.0 已经能吃满显存), 校验时按 0.1 取整。
 */
#define BONGO_CAT_RENDER_SCALE_MIN 0.1f
#define BONGO_CAT_RENDER_SCALE_MAX 4.0f
#define BONGO_CAT_RENDER_SCALE_DEFAULT 1.0f
#define BONGO_CAT_RENDER_SCALE_STEP 0.1f
/*
 * 模型纹理质量 (设置 → 性能 → 纹理质量): 模型自带的图集在**上传前**要不要被缩小。
 *
 * 为什么需要它: 图集可能声明 8192px, 而桌宠只画几百像素, 于是默认把工作尺寸钳在
 * BONGO_CAT_LIVE2D_TEXTURE_LIMIT (2048) —— 这对大面积渐变/纯色块没有损失, 但模型自带的
 * 艺术字、署名、小图标只有几十个像素, 缩 4 倍之后在屏幕上再放大回来就糊了。实测 (A-清宵):
 * 2048 → 4096 让署名区域的梯度能量 +20% (Vulkan 在 337px 窗口就有 +20%, 它没有 mip 链,
 * 只能靠源纹理自身的预平均; OpenGL 有 mip 链, 小窗口下几乎无感, 675px 窗口 +20%)。
 * 代价是显存: 8192×4096 的图集从 8.4MB 涨到 33.6MB (高) 或 134MB (原生), 所以默认必须
 * 保持标准档 —— 那也正是 1.17.1 的行为, 逐像素一致。
 *
 * 取值 0=标准(2048) 1=高(4096) 2=原生(不缩小, 由上传路径按硬件上限收口)。
 */
#define BONGO_CAT_TEXTURE_QUALITY_STANDARD 0
#define BONGO_CAT_TEXTURE_QUALITY_HIGH 1
#define BONGO_CAT_TEXTURE_QUALITY_NATIVE 2
#define BONGO_CAT_TEXTURE_QUALITY_COUNT 3
#define BONGO_CAT_TEXTURE_QUALITY_DEFAULT BONGO_CAT_TEXTURE_QUALITY_STANDARD
/* 各档实际下发的"图集工作尺寸上限"; 原生档给一个足够大的值 = 不缩小 (仍受纹理硬件上限约束) */
#define BONGO_CAT_TEXTURE_QUALITY_HIGH_LIMIT 4096
#define BONGO_CAT_TEXTURE_QUALITY_NATIVE_LIMIT 65536

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
    bool vertical_flip;
    bool mouse_mirror;
    bool mouse_vertical_flip;
    bool mouse_centered;
    bool ignore_mouse;
    int max_fps;
    /*
     * 垂直同步开关, 取 BONGO_CAT_VSYNC_DEFAULT(未设置) / 0(关) / 1(开)。
     * 用 int 而不是 bool 就是为了容下"未设置"这个第三态; 校验/保存之后一定收敛到 0/1,
     * 只有"文件里没有 vsync 键"的那一次读取会短暂带着 -1。
     */
    int vsync;
    BongoCatRenderer renderer;
    /*
     * 1 = 多键显示 (默认), 0 = 关掉 —— 关掉时退回"每侧只显示最早按下的那个键"。
     * 用 int 而不是 bool: 走可选读 read_int, 老配置里缺这个键时保持默认 1, 不会因为
     * "键不存在"判定配置损坏。
     */
    int multi_key_display;
    /*
     * 每侧最多显示几个键, 1..BONGO_CAT_MULTI_KEY_LIMIT_MAX。只有 multi_key_display
     * 打开时才有意义; 关掉时这一行在设置页里隐藏、也不生效, 但取值原样保留。
     */
    int multi_key_limit;
    /*
     * 1 = 合并成一个气泡 (默认): 同侧多键时只画一个气泡, 里面用 "+" 把各键名连起来 ("A + S")。
     * 0 = 每个键各占一个气泡、并排显示。只对自带气泡素材的模型有意义 —— 内置模型画的是彩色
     * 键帽, 没有气泡可合并, 两种取值下都是"一只手 + N 块键帽"。同样用 int + 可选读, 老配置
     * 里缺这个键时保持默认 1。
     */
    int bubble_merge;
    /*
     * 渲染精度: 以 N 倍分辨率渲染后再线性缩回窗口尺寸 (见 BONGO_CAT_RENDER_SCALE_*)。
     * 用 float + 可选读 read_float: 老配置里没有 renderScale 这个键时保持默认 1.0,
     * 不会因为"键不存在"判定配置损坏。校验时 clamp 到 [0.1, 4.0] 并按 0.1 取整,
     * 手改文件填 0 / 负数 / 超大值 / 非数字都会在这里被纠正 (读失败按默认值)。
     */
    float render_scale;
    /*
     * 纹理质量: 模型图集上传前的工作尺寸上限档位 (见 BONGO_CAT_TEXTURE_QUALITY_*)。
     * 用 int + 可选读 read_int: 老配置里没有 textureQuality 这个键时保持默认 0, 不会因为
     * "键不存在"判定配置损坏。校验时钳到 [0, 2], 越界/手改错的值按标准档收口。
     */
    int texture_quality;
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
    /* 拖动惯性: 松手后沿着拖动方向继续滑行一段。关闭则拖动结束立即停在原地。 */
    bool slide_animation;
    /*
     * 惯性衰减档位 0-99: 每拍速度乘 (100-值)/100, 数值越大越快停下; 0 = 不衰减 (恒速)。
     * 这是全局唯一的速度衰减输入 —— "一直滑动" (slide_no_decay) 不另起一套, 它只是把本帧
     * 生效的值钉在 0 (见 window_inertia.c 的注释)。放缓值生效为 0 时撞边不再损失速度、
     * 也没有拍数兜底, 所以那时它永不自行停止。缺省 15 (v1.16.3 是 0)。
     */
    int slide_slowdown_percent;
    /*
     * "一直滑动": 松手后不减速, 以恒定速度一直滑下去, 撞到屏幕边缘完整反弹 (速度不损失),
     * 于是永不自行停止 —— 屏保那种"在屏幕上撞来撞去"的效果。
     *
     * 它和上面 slide_slowdown_percent 说的是同一件事的两面, 不是第二个旋钮: 打开时把
     * **本帧生效的**放缓值钉在 0, 用户自己拖的百分比原样留在配置里, 关掉立刻按原值恢复
     * 衰减。实现处 (window_inertia.c) 与设置页提示都写明了这层关系。
     *
     * 只在 slide_animation 打开时有意义: 关掉拖动惯性时这一行在设置页里隐藏、也不生效,
     * 但取值原样保留 —— 重新打开拖动惯性就能接着用, 不偷偷改用户的选择。
     * 缺键时按 false 读 (见 config_io.c 的可选读约定), 老配置因此仍是"会衰减"的旧行为。
     */
    bool slide_no_decay;
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
     * 只对 OpenGL 生效: Vulkan 走原生交换链呈现 (见 windows_layered_window.c 的
     * bongo_cat_windows_layered_requested), 这个开关在那边是 no-op。
     * 持久化键名沿用 1.17.0 的 glTransparency(不是新名): 改名会让老配置里的取值凭空
     * 消失(旧版本读到未知键只会忽略), 而这个开关对用户的意义从来没变过。
     * 为什么是 int 而不是 bool: 配置读取走可选的 read_int, 老配置文件缺键时保持
     * 默认值 0, 不会因为"键不存在"判定配置损坏。写入侧同样按 int 存。
     * 代价: 录屏软件按标题采集"真实窗口"时拿到的是全透明画面, 要采代理窗口
     * (设置界面与文档里都写明了这一点)。
     */
    int gl_transparency;
    bool obs_background;
    bool random_expression;
    bool random_motion;
    bool random_sound;
    bool sequential_model;
    bool rounded_corners;
    BongoCatObsBackgroundColor obs_background_color;
    float hide_delay_seconds;
    float hide_fade_seconds;
    float random_expression_interval_seconds;
    float random_motion_interval_seconds;
    float random_sound_interval_seconds;
    float sequential_model_interval_seconds;
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
    /*
     * 缺 Cubism Core 的一次性引导 (见 preferences_live2d_setup.c)。
     *
     * 为什么记两条而不是一条: "已经自动引导过一次" 与 "用户明确说别再提示" 是两件事 ——
     * 前者是我们自己为了不骚扰而收手, 后者是用户的意愿, 界面上要如实显示成勾选状态。
     * 合成一条会让"不再提示"这个勾在用户没勾过的时候就是勾上的。
     * 两者都是可选读: 老配置里没有这两个键时取默认值 (false), 于是一次性引导照常发生。
     */
    bool live2d_core_prompt_shown;
    bool live2d_core_prompt_dismissed;
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

/*
 * 音效设置 (settings.json 的 "sound" 对象)。
 *
 * 三个字段解决的是同一件事的三个层次, 全部可选读:
 *   volume  总体音量 0-100。100 是默认值, 也是加入本设置之前的行为 —— 老配置缺键时
 *           读出来就是 100, 逐字节等价于"引擎默认满音量"。
 *   muted   "全部静音": 一次关掉所有模型音效, 但保留每个音效原来的开关状态 (关掉
 *           全部静音就能恢复), 所以它是独立字段而不是把 disabled 清空。
 *   disabled 被逐个关掉的音效 id 集合。默认空 = 全开。
 *
 * id 规则见 bongo_cat_sound_id(): "<模型id>:<模型目录内相对路径>", 不用会随排序变化的
 * 下标 —— 模型重新导出后 bindings 顺序可能变, 用下标会让"用户关掉的那个音效"指向别人。
 */
typedef struct BongoCatSoundPreferences {
    int volume;
    int muted;
    char disabled[BONGO_CAT_SOUND_DISABLED_CAP][BONGO_CAT_SOUND_ID_CAP];
    size_t disabled_count;
} BongoCatSoundPreferences;

typedef struct BongoCatSettings {
    BongoCatModelPreferences model;
    BongoCatWindowPreferences window;
    BongoCatApplicationPreferences app;
    BongoCatShortcutPreferences shortcuts;
    BongoCatSoundPreferences sound;
    /* Optional override for the cache directory. Empty means the platform
       default cache location is used. Written to settings.json. */
    char cache_root[BONGO_CAT_PATH_CAP];
    BongoCatBehaviorShortcut behavior_shortcuts[BONGO_CAT_BEHAVIOR_BINDING_CAP];
    size_t behavior_shortcut_count;
    BongoCatModelLabel model_labels[BONGO_CAT_MODEL_CAP];
    size_t model_label_count;
    BongoCatRemovedModel removed_models[BONGO_CAT_MODEL_CAP];
    size_t removed_model_count;
    /* 切换时跳过的模型(隐藏)。结构与 removed_models 相同(一批 id), 但它不删除磁盘上的
       模型, 只让随机/顺序切换跳过它们。 */
    BongoCatRemovedModel hidden_models[BONGO_CAT_MODEL_CAP];
    size_t hidden_model_count;
    /* 随机/顺序切换的参与集合: 一串被排除的行为 id。模型切换的合成 id 是 "switch:<模型id>",
       要排除某模型就把它对应的 switch:<id> 加进来; 默认全参与。 */
    char random_disabled[BONGO_CAT_RANDOM_DISABLED_CAP][BONGO_CAT_BEHAVIOR_ID_CAP];
    size_t random_disabled_count;
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
/*
 * 最近一次 bongo_cat_settings_validate 纠正过的项 (空串 = 没纠正过), 以及清除它。
 * 为什么不在核心层直接打日志: bongo_cat_core 不依赖 SDL, 没有日志设施; 但"手改
 * 配置文件写了个非法值, 程序悄悄改回边界"必须留痕, 所以校验把描述攒在这里,
 * 由运行时加载完配置后打一条 WARN (见 runtime/lifecycle/config_store.c)。
 */
const char *bongo_cat_settings_validation_note(void);
void bongo_cat_settings_clear_validation_note(void);
/* 让读取层 (config_io.c) 也能往那条日志里记一句话 (例如"某个键类型不对, 按默认值处理") */ 
void bongo_cat_settings_note(const char *text);
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
bool bongo_cat_settings_model_hidden(const BongoCatSettings *settings,
    const char *id);
bool bongo_cat_settings_set_model_hidden(BongoCatSettings *settings,
    const char *id, bool hidden);
bool bongo_cat_settings_random_disabled(const BongoCatSettings *settings,
    const char *behavior_id);
bool bongo_cat_settings_set_random_disabled(BongoCatSettings *settings,
    const char *behavior_id, bool disabled);
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
