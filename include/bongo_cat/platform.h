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
     * "让背景真正透明"这个开关的镜像 (持久化键 window.transparentBackground 与之同名):
     * Windows 的 OpenGL 与 Vulkan 后端共用 (两者都走代理分层呈现)。平台层
     * 看不到 BongoCatSettings, 而每帧的穿透同步 (bongo_cat_window_sync_click_through)
     * 又要用到它, 所以由运行时把它同步进来。其它平台忽略这个字段。
     */
    bool transparent_background;
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
 * 下发"透明背景"设置 (平台层读 platform->transparent_background 这个镜像)。
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
/*
 * Vulkan 后端的代理分层呈现 (仅 Windows, 其它平台 no-op)。
 *  - layered_active: 是否在走"透明背景"设置的代理分层路径。Vulkan 没有可读的 GL 前
 *    缓冲, 画面只能靠整帧回读喂给平台; vk_scene.c 用它在"开透明时才排整帧回读",
 *    不给普通 Vulkan 模式添负担。
 *  - submit_layered_rgba: 把一帧 RGBA (第 0 行为视觉上方) 提交给代理分层呈现。
 *    未启用代理时是 no-op 返回 false。
 */
bool bongo_cat_platform_layered_active(BongoCatPlatform *platform);
bool bongo_cat_platform_submit_layered_rgba(BongoCatPlatform *platform,
    int width, int height, const unsigned char *rgba);
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
/* 建窗之后调一次: 提权进程要显式放行拖放相关的窗口消息, 否则 UIPI 会把资源管理器的
   拖放整个挡在门外 (详见 windows_elevate.c 的实现注释)。非 Windows 平台是空操作。 */
void bongo_cat_platform_allow_file_drop(SDL_Window *window);
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
/*
 * 单实例唤醒请求: 第二个实例被互斥体拒绝之后, 用它告诉正在运行的那个实例"这一次想干
 * 什么"。载荷只有这一个原因, 所以直接在既有的唤醒通道上加, 不另起一套 IPC。
 * 取走 (take_wake) 之后请求即被清除, 不会重复生效。
 */
typedef enum BongoCatWakeRequest {
    /* 没有待处理的唤醒请求 (也是主循环每帧的默认结果)。 */
    BONGO_CAT_WAKE_NONE = 0,
    /* 只把宠物窗口唤到前台并置前。这是"拿不到唤醒原因"时的降级行为 (旧版本实例、
       Linux/macOS 实现), 与加这个原因之前的行为一致。 */
    BONGO_CAT_WAKE_REVEAL = 1,
    /* 用户又启动了一次同一个可执行文件: 打开/置前设置窗口。重复启动的意图基本就是
       "我要改设置", 而这个动作在第一次启动时已经做过一遍了。 */
    BONGO_CAT_WAKE_OPEN_PREFERENCES = 2
} BongoCatWakeRequest;

bool bongo_cat_platform_single_instance_begin(void);
/* 非阻塞取一次唤醒请求: 主循环每轮调用。返回 NONE 表示没有请求; 取走之后请求被清除,
   所以调用方每收到一次就只处理一次。 */
BongoCatWakeRequest bongo_cat_platform_single_instance_take_wake(void);
bool bongo_cat_platform_update_shutdown_argument(int argc, char **argv);
/*
 * 隐藏的自动更新模式: "同一个 exe 在旧进程退出后完成替换并重启自己"。
 * 命令行形如
 *   BongoCatTurbo.exe --apply-update --wait-pid=<pid> --src=<新文件> --dst=<当前 exe>
 *     [--sha256=<hex>] [--restart-args=<原样带给新进程的参数>]
 *     [--result-file=<失败回执>] [--log-file=<助手日志>]
 * 返回 true = 本次启动就是助手进程, 调用方必须立刻退出 (**不建窗口、不初始化 SDL、不碰单
 * 实例锁**), 退出码写进 *exit_code (0 成功, 非 0 失败)。
 * 返回 false = 不是这个模式, 正常启动流程继续 (非 Windows 平台恒为 false)。
 * 细节与不变量见 src/platform/windows/windows_update_apply.c。
 */
bool bongo_cat_platform_update_apply_argument(int argc, char **argv,
    int *exit_code);
bool bongo_cat_platform_single_instance_take_update_shutdown(void);
void bongo_cat_platform_single_instance_end(void);
BongoCatResult bongo_cat_platform_set_autostart(bool enabled, bool elevated,
    BongoCatError *error);
/* 已提权会话的"开机自启收口": 当前进程已是管理员且开好了 run_as_admin + autostart 时,
   把自启从普通快捷方式升级/刷新为 Task Scheduler 最高权限任务, 让下次开机免 UAC。
   普通权限下不会弹 UAC (它假定调用方已经在提权状态)。 */
BongoCatResult bongo_cat_platform_autostart_enforce_elevated(void);
BongoCatMenuAction bongo_cat_platform_context_menu(BongoCatPlatform *platform,
    const BongoCatMenuLabels *labels);
BongoCatResult bongo_cat_platform_embedded_assets(const char *target, BongoCatError *error);
/*
 * 把内嵌在 exe 里的 Live2D 渲染组件按需释放到 directory(即 <存储根>/cores/)。
 *
 * 存在的理由: 发行形态要回到"只发单个 exe", 而组件必须落在 <存储根>/cores/ 才会被现有的
 * 目录解析看见。已存在且与内嵌字节一致时不做任何写入; 不一致时先写临时文件再原子改名。
 * 返回 BONGO_CAT_ERROR_PLATFORM 表示"这个平台没有内嵌组件"(Linux/macOS 资源随包落盘,
 * 或者这是没有 SDK 的诊断构建), 调用方按"没什么可释放"处理, **不是**错误。
 * 任何真实失败都只是降级: 组件缺失时 Live2D 走诊断兜底, 绝不能让启动失败。
 */
BongoCatResult bongo_cat_platform_embedded_assets_release_component(
    const char *directory, BongoCatError *error);
/*
 * 用同一份可执行文件重新启动本进程 (切换渲染后端 / 启停便携模式之后)。
 * arguments 是附加的命令行参数 (可为 NULL 或空串), 调用方保证它安全。
 * 调用方必须**先**把单实例互斥体还回去 (bongo_cat_platform_single_instance_end),
 * 否则新进程会被判定成"已有实例在运行"而自己退出。
 * 实现负责把提权状态原样带过去 (见 src/platform/common/application_restart.c),
 * 保证"提权 -> 重启 -> 再提权"这种循环不可能发生。
 * 返回非 OK 时进程状态不变, 调用方应继续运行并提示用户手动重启。
 */
BongoCatResult bongo_cat_platform_restart_application(const char *arguments,
    BongoCatError *error);

/*
 * 以管理员身份重启当前程序 (Windows: ShellExecuteExW 的 runas, 会弹一次 UAC)。
 * 用于"开启以管理员身份运行" (run_as_admin) 时从普通权限进程立刻提权重启。
 * 与 bongo_cat_platform_restart_application 一样: 调用方必须先还回单实例互斥体;
 * 传 arguments 时新进程已带 BONGO_CAT_ELEVATION_TRIED=1 幂等标记, 不会再次弹 UAC;
 * 返回非 OK 时进程状态不变。
 * 非 Windows 平台不支持, 恒返回 BONGO_CAT_ERROR_PLATFORM。
 */
BongoCatResult bongo_cat_platform_restart_application_elevated(
    const char *arguments, BongoCatError *error);

#ifdef __APPLE__
/* Read the permission macOS grants the app right now, without prompting.
   Callers refresh on a user action or a focus change, never per frame. */
bool bongo_cat_platform_input_monitoring_authorized(void);
/* Ask macOS for Input Monitoring; only an explicit user action may call it. */
bool bongo_cat_platform_input_monitoring_request(void);
#endif

#endif