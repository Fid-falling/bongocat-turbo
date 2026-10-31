#ifndef BONGO_CAT_APP_H
#define BONGO_CAT_APP_H

#include "bongo_cat/config.h"
#include "bongo_cat/input.h"
#include "bongo_cat/model.h"
#include "bongo_cat/mver_pointer.h"
#include "bongo_cat/mouse.h"
#include "bongo_cat/platform.h"
#include "bongo_cat/shortcut.h"
#include "bongo_cat/sound_shortcut.h"

typedef struct BongoCatAudio BongoCatAudio;
typedef struct BongoCatTray BongoCatTray;
typedef struct BongoCatOverlay BongoCatOverlay;
typedef struct BongoCatPreferences BongoCatPreferences;
typedef struct BongoCatI18n BongoCatI18n;
typedef struct BongoCatMultiPetRuntime BongoCatMultiPetRuntime;
typedef struct BongoCatModelRefresh BongoCatModelRefresh;
typedef struct BongoCatUpdateService BongoCatUpdateService;
typedef struct BongoCatVk BongoCatVk;
/* 可分步的 Live2D 加载会话: 只在模型切换期间存在, 定义在 live2d 层
   (bongo_cat/model.h 里的不透明句柄)。这里只需要指针。 */
typedef struct BongoCatLive2DLoadSession BongoCatLive2DLoadSession;

#define BONGO_CAT_MODEL_COVER_PENDING_CAP 8
/* --ci-model-switch= 一次能登记的模型数。它只是自测驱动器的输入, 取 8 足够表达
   "连点好几个模型" (真实用户连点也远少于这个数)。 */
#define BONGO_CAT_SMOKE_SWITCH_CAP 8

/* 模型切换前后要保持的"内容锚点" (画面里真正有内容的矩形), 由
   runtime/model/model_geometry.h 提供实现。它出现在这里是因为它是影子状态的一部分。 */
typedef struct BongoCatModelContentAnchor {
    int x, y;
    bool valid;
} BongoCatModelContentAnchor;

/*
 * 模型切换的"影子状态": 切换开始那一刻就必须定下来、但**不能**立刻生效的那些东西。
 * 它随切换流程一起存在 (见 runtime/model/model_switch.c), 提交成功或失败回退之后作废。
 *
 * 为什么要有它: 一次切换被拆成"影子准备 -> 分步加载 -> 提交"三段, 中间那段要花几秒, 而
 * 主循环必须在这期间持续出帧。拆分要求把"加载开始前定下的输入"存下来跨帧使用, 于是有了
 * 这个结构。它同时记录进入时的 GL 上下文, 用来在两条出口还原调用方的 GL 状态。
 */
typedef struct BongoCatModelSwitchShadow {
    /* 旧模型是否可用 (假表示这是启动后的第一次加载, 没有"原模型"可回退)。 */
    bool replacing;
    /* 新模型的行为目录: 由影子准备分配, 由提交接手所有权或由释放路径 free。 */
    BongoCatBehaviorCatalog *behaviors;
    bool behavior_catalog_valid;
    BongoCatLive2DRenderOptions render_options;
    BongoCatModelContentAnchor content_anchor;
    int pixel_width, pixel_height;
    /* 进入时当前是哪个上下文 (用于还原), 以及"是否真的切过"。 */
    SDL_Window *previous_window;
    void *previous_context;
    bool context_activated;
} BongoCatModelSwitchShadow;

/*
 * "自动回退 OpenGL" 的持久化记忆 (状态文件由 runtime/lifecycle/renderer_fallback_state.c
 * 负责读写, 不写进 settings.json: 它是这台机器上的运行证据, 不是用户偏好)。
 *
 * 为什么必须有它: 看门狗回退一次之后, 用户下次手动改回 Vulkan 还会再卡一次, 而界面上
 * 除了日志没有任何线索。把连续失败次数、最近原因与"是否已暂停自动尝试"留下来, 才能让
 * 设置页给出可见提示、也才能避免无休止地重试一个已知会卡死的后端。
 *
 * 字段全部是自包含的定长缓冲: 这份状态要能在启动最早期 (设置还未加载完) 被读出来, 不能
 * 依赖堆分配, 也不能因为一次读写失败影响启动。
 */
typedef struct BongoCatRendererFallbackState {
    /* 连续自动回退次数: 只在自动回退时 +1, 成功启动 Vulkan 或用户手动重试时归零 */
    int consecutive_failures;
    /* 达到阈值后置位: 之后的启动不再自动尝试 Vulkan (直接 OpenGL), 直到用户明确重试 */
    bool auto_disabled;
    /* 最近一次失败的原因枚举字符串 (first-frame-timeout / init-failed), 不本地化 */
    char last_reason[48];
    /* 给界面/自检看的一句原因摘要 (阶段/驱动返回码等), 可以本地化, 允许为空 */
    char last_detail[192];
    /* 失败时的显卡/驱动摘要 (首帧超时时能拿到; 初始化失败时通常为空) */
    char last_gpu[160];
    /* 失败时间戳, 格式与日志一致 (带 UTC 偏移), 允许为空 */
    char last_failure_at[64];
} BongoCatRendererFallbackState;

typedef struct BongoCatApp {
    BongoCatSettings settings;
    BongoCatSessionState session;
    BongoCatInputState input;
    /* Main-thread-only cumulative input diagnostics. */
    struct {
        uint64_t log_ms, drained_keys, ignored_keys, mapped_keys;
        uint64_t unmapped_keys, no_model_keys, presented_frames;
    } input_diagnostics;
    BongoCatShortcutState shortcut_state;
    BongoCatSoundShortcutState sound_shortcut_state;
    bool sound_shortcut_active[BONGO_CAT_BEHAVIOR_BINDING_CAP];
    BongoCatModelCatalog models;
    BongoCatBehaviorCatalog behaviors;
    /* One immutable installed package's behavior catalog can be reused when
       the user toggles back to it. Nearby source models are excluded because
       their files may change outside the application. Allocate it lazily so
       single-model child processes do not reserve the catalog. */
    BongoCatBehaviorCatalog *behavior_cache;
    char behavior_cache_model_id[BONGO_CAT_ID_CAP];
    char behavior_cache_digest[65];
    bool behavior_cache_valid;
    bool behavior_catalog_valid;
    BongoCatLive2DRenderOptions model_render_options;
    BongoCatI18n *i18n;
    BongoCatPlatform platform;
    BongoCatLive2D *live2d;
    BongoCatAudio *audio;
    BongoCatTray *tray;
    BongoCatOverlay *overlay;
    BongoCatPreferences *preferences;
    BongoCatMultiPetRuntime *multi_pet;
    BongoCatModelRefresh *model_refresh;
    BongoCatUpdateService *update;
    SDL_Window *window;
    void *gl_context;
    /* 只有 Vulkan 后端被选中时非空; 两条路互斥, 不会同时存在。 */
    BongoCatVk *vk;
    char settings_path[BONGO_CAT_PATH_CAP];
    char session_path[BONGO_CAT_PATH_CAP];
    char config_root[BONGO_CAT_PATH_CAP];
    char data_root[BONGO_CAT_PATH_CAP];
    char models_root[BONGO_CAT_PATH_CAP];
    char cache_root[BONGO_CAT_PATH_CAP];
    char cache_root_override[BONGO_CAT_PATH_CAP];
    char state_root[BONGO_CAT_PATH_CAP];
    char log_root[BONGO_CAT_PATH_CAP];
    char storage_root[BONGO_CAT_PATH_CAP];
    char asset_root[BONGO_CAT_PATH_CAP];
    char locale_root[BONGO_CAT_PATH_CAP];
    char nearby_root[BONGO_CAT_PATH_CAP];
    char executable_path[BONGO_CAT_PATH_CAP];
    char primary_state_root[BONGO_CAT_PATH_CAP];
    char secondary_model_id[BONGO_CAT_ID_CAP];
    char smoke_import_path[BONGO_CAT_PATH_CAP];
    char smoke_model[BONGO_CAT_ID_CAP];
    char smoke_runtime_model[BONGO_CAT_ID_CAP];
    char smoke_live2d_scenario[BONGO_CAT_ID_CAP];
    char smoke_viewer_trace[BONGO_CAT_PATH_CAP];
    char pending_model_cover_ids[BONGO_CAT_MODEL_COVER_PENDING_CAP][BONGO_CAT_ID_CAP];
    char pending_model_cover_paths[BONGO_CAT_MODEL_COVER_PENDING_CAP][BONGO_CAT_PATH_CAP];
    uint64_t pending_model_cover_retry_ns[BONGO_CAT_MODEL_COVER_PENDING_CAP];
    unsigned pending_model_cover_attempts[BONGO_CAT_MODEL_COVER_PENDING_CAP];
    size_t pending_model_cover_count;
    char loaded_model[BONGO_CAT_ID_CAP];
    /* 已经设到窗口上的标题: 多桌宠时窗口标题是模型名, 只有变了才真的 SDL_SetWindowTitle
       (每次"应用设置"都发一次窗口列表通知没必要)。 */
    char applied_window_title[128];
    char loading_model[BONGO_CAT_ID_CAP];
    /*
     * "换桌宠"这条路径的队列状态 (实现见 runtime/model/model_switch.c)。
     *
     * 为什么按值内嵌而不是堆分配: 同一时刻只可能有一个切换, 而启动与关闭路径都要求它零成本
     * —— 堆分配会多出一条"分配失败怎么办"的分支, 而这里根本没有这个需求。
     *
     * 语义要点: active 期间 target_id 是"用户最后选的那个模型", shadow 与 session 只在
     * active 期间有效。提交之前 loaded_model / session.active_model_id 一个字节都不改, 所以
     * 失败时"回退到原模型"不需要任何额外动作。
     */
    struct {
        bool active;
        char target_id[BONGO_CAT_ID_CAP];
        BongoCatModelSwitchShadow shadow;
        BongoCatLive2DLoadSession *session;
        uint64_t started_ns;
        uint64_t deadline_ns;
        /* 诊断计数: 这次切换被推进了多少次 (step_calls)、其中做了多少步 (steps_taken)、
         * 最长的一步多久 (slowest_step_ns)。它们用来验证"切换期间主循环确实在转" —— 帧
         * 间隔大到接近切换总时长时, 第一件事就是看这三个数。 */
        uint64_t step_calls;
        uint64_t steps_taken;
        uint64_t slowest_step_ns;
        BongoCatError error;
    } model_switch;
    /* 分步加载的进度 (0..1): 设置页用它画"正在切换"的中间态, 加载期间有效。 */
    float model_load_progress;
    BongoCatModelMode loaded_mode;
    bool running;
    bool settings_store_valid;
    bool session_store_valid;
    bool settings_store_blocked;
    bool session_store_blocked;
    bool autostart_launch;
    bool secondary_pet;
    bool secondary_origin_known;
    bool secondary_control_known;
    bool secondary_control_visible;
    bool secondary_control_pass_through;
    /* Keep the initial transparent window hidden until its first complete
       model frame is ready. This prevents a platform-dependent black flash. */
    bool startup_visibility_pending;
    int secondary_origin_x, secondary_origin_y;
    uint64_t secondary_control_check_ns;
    uint64_t secondary_control_failure_ns;
    uint64_t secondary_settings_check_ns;
    uint64_t startup_raise_due_ns;
    uint64_t model_load_last_frame_ns;
    bool smoke;
    bool smoke_preferences;
    bool smoke_preference_shortcut;
    bool smoke_preference_model_select;
    bool smoke_remove_imported;
    bool smoke_shortcuts;
    bool smoke_menu;
    bool smoke_ignore_global_input;
    bool smoke_pass_through;
    bool smoke_context_menu;
    bool smoke_frame_audited;
    bool smoke_frame_series;
    bool smoke_runtime_flow;
    bool smoke_freeze_model;
    /*
     * --ci-sound-test / --ci-sound-toggle=N: 音效管理的可测性钩子。
     * 启动期模型加载完成后按真实播放路径走一遍所有音效, 用 INFO 日志输出
     * "id / 音量 / 有效增益 / 是否被静音或禁用" —— 不需要真的听声音, 也不依赖
     * 调试构建的 DEBUG 级别。toggle 是 1 基的清单序号, 用来验证"改了会落盘"。
     */
    bool smoke_sound_test;
    unsigned smoke_sound_toggle;
    unsigned smoke_runtime_stage;
    unsigned model_load_runtime_stage;
    unsigned model_selection_serial;
    uint64_t smoke_runtime_flow_ns;
    /*
     * --ci-model-switch= 的自测驱动器状态 (实现见 runtime/lifecycle/smoke_model_switch.c)。
     *
     * 为什么要有它: "连点好几个模型, 最终只应用最后一次"以及"切到坏模型时回退原模型"这两条
     * 结论只能由真实主循环跑出来 —— 单测里手工调一次 request/step 无法覆盖"请求落在上一
     * 次加载进行到一半时"这条路径。它只登记请求、不碰模型状态, 与设置页点卡片走的是同一个
     * 入口 (bongo_cat_app_request_model_switch)。
     */
    struct {
        bool enabled;
        bool armed;
        bool issued;
        /* 已经登记过的请求数; 等于 count 时表示这一轮连点已经发完, 剩下只等队列排空。 */
        unsigned count;
        unsigned next;
        uint64_t delay_ns;
        uint64_t gap_ns;
        uint64_t due_ns;
        char ids[BONGO_CAT_SMOKE_SWITCH_CAP][BONGO_CAT_ID_CAP];
    } smoke_switch;
    /*
     * --ci-keys=KeyA,KeyS,... 的启动注入清单: 初始化的最后推进输入队列 (注入点见
     * lifecycle/app.c), 与真实按键走同一条 drain -> apply -> overlay 路径。
     * 存在的理由: "同侧多键按住"这类场景依赖物理键盘, 脚本合成的系统级注入又会被
     * Raw Input 接收层的设备去重/对账吞掉, 无人值守下复现不了 —— 从队列入口注入
     * 才能稳定地端到端验证下游渲染。
     */
    char smoke_keys[BONGO_CAT_SMOKE_SWITCH_CAP][BONGO_CAT_ID_CAP];
    unsigned smoke_keys_count;
    int smoke_language;
    int smoke_theme;
    int smoke_preference_page;
    /*
     * 命令行 --renderer= / --opengl / --vulkan 的后端覆盖值 (BongoCatRenderer),
     * -1 表示没有覆盖、按 settings.json 走。
     *
     * 为什么要有它: 设置窗口自己就是用当前后端画的, 那个后端一旦在这台机器上画不出
     * 内容, 用户就没有任何 GUI 入口把后端切回去 —— 命令行是唯一不依赖界面的逃生
     * 通道, 所以它的优先级必须高于配置。覆盖只作用于本次运行, 不写回配置。
     */
    int renderer_override;
    /* 自动回退记忆 (见上面的 BongoCatRendererFallbackState): 由 config 加载之后读入,
       看门狗/初始化失败/手动重试/成功启动时更新。零值 = 没有回退记录。 */
    BongoCatRendererFallbackState renderer_fallback;
    /*
     * 这次运行带了 --ci-* 参数 (无人值守/自测)。与 smoke 分开是因为 --ci-menu 这类
     * 参数并不置 smoke: 它们照样会走真实菜单回调, 而模态弹窗 (SDL_ShowSimpleMessageBox)
     * 在无人值守运行里会把进程挂住 —— 那些回调必须能看出"现在没有人在看屏幕"。
     */
    bool automated;
    /*
     * "改完设置后自动重启" 的请求位 (切换渲染后端、启停便携模式都会置它)。
     *
     * 为什么用标志位而不是当场重启: 新设置由 config_store 延迟落盘, 而且重启前必须把
     * 单实例互斥体还回去 —— 这两件事都只能在主循环末尾做, 见
     * runtime/lifecycle/application_restart.c。
     */
    bool restart_requested;
    /* restart_requested 开启时, 置位表示这次重启要**提权** (开启 run_as_admin 且
       当前未提权)。走 bongo_cat_platform_restart_application_elevated 拉起新进程;
       否则按普通重启保持当前权限。 */
    bool restart_elevated;
    /*
     * restart_requested 开启时, 置位表示这次不是"重启自己"而是"换成刚下载好的新版本":
     * 主循环末尾会拉起同一个 exe 的隐藏助手模式 (--apply-update), 由它在旧进程退出后
     * 替换文件并重新启动 (见 runtime/update/update_install.c)。
     * 与 restart_elevated 的区别: 那条路是立刻用**当前**的 exe 起一个新进程, 这条路的
     * 新进程是替换之后的新文件, 所以必须等本进程真的退出。
     */
    bool update_install_pending;
    int exit_code;
    bool dirty;
    /* 已经下发给后端的 max_fps / 垂直同步, -1 表示还没下发过(两个都是"先算再比",
       所以初值只要不等于任何合法结果即可)。垂直同步单独记: 开关一翻, 即使帧率没变
       也要重新下发一次。 */
    int applied_max_fps;
    int applied_vsync;
    uint64_t last_frame_ns;
    uint64_t render_retry_ns;
    uint64_t smoke_deadline_ns;
    uint64_t hover_deadline_ns;
    uint64_t pointer_hit_deadline_ns;
    uint64_t display_recovery_due_ns;
    uint64_t mouse_last_ns;
    uint64_t frame_audit_bmp_ns;
    uint64_t random_expression_due_ns;
    float random_expression_interval_seconds;
    uint32_t random_expression_state;
    uint64_t random_motion_due_ns;
    float random_motion_interval_seconds;
    uint64_t random_sound_due_ns;
    float random_sound_interval_seconds;
    uint64_t sequential_model_due_ns;
    float sequential_model_interval_seconds;
    uint64_t settings_saved_hash, settings_observed_hash;
    uint64_t session_saved_hash, session_observed_hash;
    uint64_t settings_save_due_ns, session_save_due_ns;
    BongoCatMouseTracking mouse_tracking;
    BongoCatMverPointerState mver_pointer;
    bool hover_inside;
    bool hover_hidden;
    bool hover_fade_active;
    float hover_fade_phase, hover_fade_target, hover_fade_opacity;
    uint64_t hover_fade_tick_ns, hover_fade_next_ns;
    bool pointer_known;
    bool pointer_hit_dirty;
    bool pointer_transparent;
    bool click_through_valid;
    bool click_through_applied;
    bool click_through_forced_applied;
    bool left_mouse_down;
    bool right_mouse_down;
    bool side_mouse_down;
    bool back_mouse_down, forward_mouse_down;
    bool mouse_button_event_pending;
    bool model_pointer_anchor_ready;
    float model_pointer_anchor_x, model_pointer_anchor_y;
    bool pointer_relative_active;
    bool pointer_cursor_locked;
    bool window_minimized;
    double pointer_x, pointer_y;
    bool resize_gesture;
    float resize_scale_start, resize_scale_target;
    int resize_base_width, resize_base_height;
    bool resize_pending;
    bool resize_render_target_pending;
    /* Native preview ownership and gesture lifetime belong to window_snapshot.c. */
    void *window_snapshot;
    bool snapshot_blocked;
    uint64_t snapshot_deadline_ns;
    float snapshot_pointer_x, snapshot_pointer_y;
    bool wheel_animation_active;
    bool wheel_gesture_active;
    int resize_pixel_width, resize_pixel_height;
    uint64_t wheel_animation_ns, wheel_input_ns, wheel_event_ns;
    float wheel_opacity_target, wheel_scale_target;
    float wheel_center_x, wheel_center_y;
    float wheel_geometry_scale;
    int wheel_base_width, wheel_base_height;
    bool drag_candidate;
    bool window_drag_active;
    float drag_start_x, drag_start_y;
    int drag_window_x, drag_window_y;
    /* 拖动惯性: 快照存窗口原点, 3 槽 + 100ms 桶轮转。 */
    uint64_t drag_snap_tick_ns[3];
    int drag_snap_x[3], drag_snap_y[3];
    int drag_snap_slot[3]; /* 存储槽排布: [0] 最新, [1]/[2] 更老; -1 表示空槽 */
    /* 滑行态: 由 bongo_cat_window_update_inertia 每帧驱动, 沿初速方向衰减移动。 */
    bool slide_active;
    double slide_velocity_x, slide_velocity_y;
    double slide_x, slide_y;
    uint64_t slide_last_ns;
    int slide_frame;
    /* 不足一拍的余量: 按真实时间累积成 15.6ms 的拍, 见 update_inertia。 */
    double slide_accum_s;
    /* 上一拍设下的窗口位置: 下一拍照面不是这个值, 就说明窗口被外力动了, 滑行停止。 */
    int slide_last_set_x, slide_last_set_y;
    void *drag_display_bounds;
    int drag_display_count;
    float left_stick_x, left_stick_y;
    float right_stick_x, right_stick_y;
    bool left_stick_pressed, right_stick_pressed;
    BongoCatInputEvent active_inputs[
        BONGO_CAT_INPUT_KEY_STATE_CAP + BONGO_CAT_INPUT_RECOVERY_CAP];
    size_t active_input_count;
    uint32_t active_gamepad;
} BongoCatApp;

int bongo_cat_app_run(int argc, char **argv);
void bongo_cat_app_apply_input(BongoCatApp *app, const BongoCatInputEvent *event);
void bongo_cat_app_reapply_input(BongoCatApp *app);
void bongo_cat_app_reset_gamepad(BongoCatApp *app);
void bongo_cat_gamepad_event(BongoCatApp *app, const void *sdl_event);
void bongo_cat_app_shortcuts(BongoCatApp *app, const BongoCatInputEvent *event);
bool bongo_cat_app_select_model(BongoCatApp *app, const char *id);
bool bongo_cat_app_select_model_with_error(BongoCatApp *app,
    const char *id, BongoCatError *error);
bool bongo_cat_app_model_active(const BongoCatApp *app, const char *id);
size_t bongo_cat_app_active_model_count(const BongoCatApp *app);
bool bongo_cat_app_set_model_active(BongoCatApp *app, const char *id,
    bool active, BongoCatError *error);
void bongo_cat_app_set_multiple_pets(BongoCatApp *app, bool enabled);
bool bongo_cat_app_run_behavior(BongoCatApp *app,
    const BongoCatBehaviorEntry *behavior);
size_t bongo_cat_app_selected_motion_count(const BongoCatApp *app);
void bongo_cat_app_capture_behavior_state(BongoCatApp *app);
size_t bongo_cat_app_saved_behavior_count(const BongoCatApp *app,
    const char *model_id);
void bongo_cat_app_forget_behavior_state(BongoCatApp *app,
    const char *model_id);
void bongo_cat_app_restore_behavior_state(BongoCatApp *app,
    const char *model_id);
BongoCatResult bongo_cat_app_import_model(BongoCatApp *app, const char *source, BongoCatError *error);
BongoCatResult bongo_cat_app_remove_model(BongoCatApp *app, const char *id, BongoCatError *error);
void bongo_cat_app_rescan_models(BongoCatApp *app);
void bongo_cat_app_refresh_installed_models(BongoCatApp *app);
void bongo_cat_app_refresh_nearby_models(BongoCatApp *app);
void bongo_cat_app_request_model_refresh(BongoCatApp *app);
bool bongo_cat_app_model_refresh_busy(const BongoCatApp *app);
void bongo_cat_app_request_model_package_refresh(BongoCatApp *app,
    const char *package_id);
void bongo_cat_app_request_nearby_model_refresh(BongoCatApp *app);
void bongo_cat_config_store_flush(BongoCatApp *app);
/*
 * 请求"改完设置后自动重启本程序"(切换渲染后端、启停便携模式)。
 * 返回 true = 已经排上, 主循环末尾会执行; false = 本次运行不允许自动重启 (处于
 * --ci-* 无人值守运行), 调用方应当提示用户手动重启。
 * 本函数**只能**由用户主动操作调用 (初始化、命令行、周期任务等任意自动来源都不得
 * 调用, 见 runtime/lifecycle/application_restart.c 文件头的"无防循环标记"一段):
 * 这条不变量靠代码审查强制, 不设运行时守卫。
 * 参数与互斥体的处理都在 runtime/lifecycle/application_restart.c。
 */
bool bongo_cat_app_request_restart(BongoCatApp *app);
/*
 * 同 bongo_cat_app_request_restart, 但 elevated=true 时这次重启会**以管理员身份**
 * 拉起新进程 (用于给"开启以管理员身份运行"立即生效)。仅在 Windows 上有实际差别;
 * 排上的普通重启可被后续的提权重启升级。
 */
bool bongo_cat_app_request_restart_elevated(BongoCatApp *app, bool elevated);
/*
 * 请求"关掉自己, 换成刚下载并校验通过的新版本" (一键更新的最后一步)。
 * 与 bongo_cat_app_request_restart 共用主循环末尾的收尾 (落盘 -> 还回单实例锁 -> 退出),
 * 区别只在退出前做的是"拉起隐藏的更新助手"而不是"重启当前 exe"。
 * 返回 false = 本次运行不允许自动替换 (--ci-* / 冒烟): CI 只能检查, 不能动二进制。
 * 调用方随即将更新状态改成"请手动替换"。
 */
bool bongo_cat_app_request_update_restart(BongoCatApp *app);

#endif
