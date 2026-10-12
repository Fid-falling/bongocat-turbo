/*
 * 设置窗口的全部运行时状态 + 内部接口声明。
 *
 * BongoCatPreferences 是"设置窗口"这个单例的全部家当: 窗口/GL 或 Vulkan 上下文、
 * UI 后端、纹理、页面与滚动状态、各种动画时间戳、文本编辑会话、拖拽与缩放状态。
 * 它由 app 持有, 但里面的窗口是**可回收**的: 窗口不显示时 release_idle_window 会把
 * 窗口、UI 后端、纹理全部销毁, 只留下这个结构体; 下次显示再重建。所以任何"只在窗口
 * 存在期间有效"的指针 (ui.context / vk / gl_context / 各种 texture) 用之前都必须
 * 先确认窗口还在。
 *
 * 改这个结构体时有两条纪律:
 * 1. 新增字段如果是"窗口级资源", 要在 release_window 里一起清掉, 否则下次开窗
 * 会用一个已经销毁的句柄;
 * 2. 新增字段如果是"跨窗口会话"(比如用户的选择、动画进度), 就不要在
 * release_window 里清, 那是设计上要保留的。
 */
#ifndef BONGO_CAT_PREFERENCES_STATE_H
#define BONGO_CAT_PREFERENCES_STATE_H

#include "preferences_internal.h"
#include "preferences_scrollbar.h"
#include "preferences_text_session.h"
#include "ui_backend.h"
#include "bongo_cat/vk_backend.h"

/*
 * 设置窗口的 Vulkan 通路连续失败多少帧就回退 OpenGL。
 *
 * 取 3 而不是 1: 交换链过期/重建在拖窗口尺寸时是正常现象, 单帧失败不代表这个窗口
 * 端不出去; 取 3 也不会让用户等太久 (事件驱动的设置窗口一秒钟就能攒够三帧)。
 */
#define BONGO_CAT_PREF_VK_FAILURE_LIMIT 3u

/*
 * 模型加载动画的时长参数 (纳秒): 等待段的总时长、快速爬升段、结束补完段, 以及等待段
 * 的上限 (永远不到 100%)。这四个值配合 preferences_model.c 里的两段曲线使用。
 */
#define BONGO_CAT_MODEL_LOAD_VISUAL_DURATION_NS 5000000000ull
#define BONGO_CAT_MODEL_LOAD_VISUAL_RAMP_NS 3000000000ull
#define BONGO_CAT_MODEL_LOAD_VISUAL_COMPLETE_NS 160000000ull
#define BONGO_CAT_MODEL_LOAD_VISUAL_WAIT_CAP 0.95f
#include "bongo_cat/i18n.h"
#include "bongo_cat/preferences.h"
#include "modal_frame.h"

typedef struct BongoCatPreferenceNotice {
    char message[384];
    uint64_t started_ns;
    uint64_t until_ns;
    bool error;
} BongoCatPreferenceNotice;

struct BongoCatPreferences {
    BongoCatApp *app;
    SDL_Window *window;
    SDL_GLContext gl_context;
    bool owns_gl_context;
    /* 设置窗口自己的 Vulkan 上下文 (renderer=vulkan 时非空) */
    BongoCatVk *vk;
    bool prefer_vulkan;
    /*
     * 本进程内"设置窗口的 Vulkan 通路已经被判定不可用"。
     *
     * 为什么需要它而不是复用 prefer_vulkan: prefer_vulkan 在每次
     * bongo_cat_preferences_open_window 开头都会被重新按渲染后端算一遍, 所以它只能
     * 表达"这一次开窗想用哪个后端"; 而"这个后端已经在这台机器上连续提交失败了"是
     * 一条跨窗口的结论, 必须粘住 —— 否则回退到 OpenGL 之后下一次开窗又会回到
     * Vulkan, 用户看到的就是"界面时好时坏"。
     */
    bool vk_disabled;
    /* 置位表示"本帧结束后请把设置窗口重建成 OpenGL 窗口"(见 preferences_present.c) */
    bool vk_fallback_requested;
    /*
     * Vulkan 帧失败记账: consecutive 用于判定回退阈值 (成功一帧即清零),
     * total 只用于日志 —— 用户回传的日志里"失败了多少次"比"是否失败"更有说服力。
     */
    unsigned vk_consecutive_failures;
    uint64_t vk_failures;
    bool transparent_window;
    bool visible;
    bool ui_initialized;
    BongoCatUIBackend ui;
    unsigned int logo_texture;
    unsigned int icon_texture;
    unsigned int icon_texture_hidpi;
    bool icon_hidpi_attempted;
    int logo_width;
    int logo_height;
    unsigned int catime_texture;
    unsigned int vlaina_texture;
    int catime_width, catime_height;
    int vlaina_width, vlaina_height;
    bool support_assets_loaded;
    struct nk_user_font support_logs_font;
    int page;
    int style_theme;
    BongoCatLanguage font_language;
    nk_rune glyph_ranges[2048];
    bool input_active;
    bool import_requested;
    bool import_drop_active;
    bool import_render_active;
    BongoCatImportDialog *import_dialog;
    bool frame_checked;
    /*
     * 事件驱动渲染的总开关: 为真才画这一帧, render 一开始就把它清掉, 帧末再按
     * "还有动画在跑吗"决定要不要重新置真。所有输入/状态变化都必须经过标脏,
     * 漏标的表现就是"界面卡住不动"。
     */
    bool render_dirty;
    bool font_reload_pending;
    bool font_reload_defer_once;
    size_t pending_import_name_count;
    char pending_import_names[BONGO_CAT_MODEL_CAP][BONGO_CAT_ID_CAP];
    bool model_directory_watch_active;
    bool model_directory_watch_known;
    bool model_directory_watch_refresh;
    bool model_directory_watch_available;
    size_t model_directory_watch_count;
    uint64_t model_directory_watch_sum;
    uint64_t model_directory_watch_xor;
    uint64_t model_directory_watch_due_ns;
    bool smoke_behavior_open_pending;
    bool model_selection_pending;
    bool pending_model_multiple;
    bool pending_model_active;
    bool model_loading;
    bool model_load_visual_active;
    float model_load_progress;
    float model_load_render_progress;
    uint64_t model_load_render_ns;
    uint64_t model_load_visual_started_ns;
    uint64_t model_load_visual_completion_ns;
    char pending_model_id[BONGO_CAT_ID_CAP];
    char loading_model_id[BONGO_CAT_ID_CAP];
    char model_load_visual_id[BONGO_CAT_ID_CAP];
    uint64_t last_render_ns;
    uint64_t shown_ns;
    float pending_raster_scale;
    /*
     * 重建失败后的重试时间 (1s 后); render_retry_ns 则是整帧失败 (上下文/呈现失败) 的
     * 重试时间, 避免失败时每帧都傻等一次驱动调用。
     */
    uint64_t raster_retry_ns;
    uint64_t render_retry_ns;
    /*
     * 滚动状态机, 下标就是页号 (0..3), 每页各存一份:
     * current 当前实际偏移 (由指数平滑逼近 target);
     * target  滚轮/滚动条给出的目标偏移;
     * ready   该页是否已经读到过 Nuklear 组的真实滚动值 —— 没 ready 之前回写会
     * 把用户的滚动位置冲掉, 所以首帧只读不写。
     * 切页时这三者刻意都不重置 (见 page_cache_clear 的注释)。
     */
    float scroll_current[4];
    float scroll_target[4];
    bool scroll_ready[4];
    bool page_seen;
    int last_page;
    uint64_t page_transition_ns;
    BongoCatPreferenceNotice notices[4];
    bool behavior_dialog;
    bool behavior_dialog_input_armed;
    int behavior_tab;
    uint64_t behavior_dialog_opened_ns;
    uint64_t behavior_dialog_closing_ns;
    uint64_t behavior_tab_transition_ns;
    float behavior_scroll[3];
    bool behavior_audio_playing[BONGO_CAT_BEHAVIOR_CAP];
    BongoCatPreferencesScrollbar behavior_scrollbar;
    BongoCatPreferencesTextSession behavior_rename;
    BongoCatPreferencesTextSession model_rename;
    BongoCatPreferencesTextSession cache_path;
    /*
     * 便携模式开关的迁移状态机 (0 = 空闲, ±1..±3 = 进行中, 正数开启、负数关闭)。
     * 是"跨窗口会话"而不是窗口级资源: 迁移跑到一半窗口被回收的话, 下一次开窗还得接着
     * 把它走完 (见 preferences_portable.c 的状态机说明)。
     */
    int portable_migration_pending;
    bool native_drag;
    bool chrome_dragging;
    bool live_resize_active;
    bool live_resize_rendering;
    bool live_resize_pending;
    bool live_resize_timer;
    bool live_resize_modal_ready;
    BongoCatModalFrame live_resize_modal_frame;
    unsigned live_resize_layout_frames;
    int drag_window_x;
    int drag_window_y;
    float drag_pointer_x;
    float drag_pointer_y;
    char shortcut_id[BONGO_CAT_ID_CAP + 16];
    char *shortcut_target;
    int shortcut_capacity;
    char shortcut_original[BONGO_CAT_SHORTCUT_CAP];
    SDL_Keycode shortcut_key;
    bool shortcut_recording;
    uint64_t shortcut_suppress_until_ns;
#ifdef __APPLE__
    /* The permission changes while the user is in System Settings, so the page
       reads it again when it becomes visible and when the window regains focus
       instead of querying macOS per frame. */
    bool input_monitoring_authorized;
    bool input_monitoring_valid;
#endif
};

#ifdef __APPLE__
void bongo_cat_preferences_input_monitoring_refresh(BongoCatPreferences *value);
#endif

int bongo_cat_preferences_resolved_theme(const BongoCatPreferences *value);
void bongo_cat_preferences_apply_theme(BongoCatPreferences *value);
bool bongo_cat_preferences_open_window(BongoCatPreferences *value);
/*
 * 把设置窗口从 Vulkan 换成 OpenGL 并重建。只在窗口"帧外"调用 (目前是 render 的开头),
 * 因为它会销毁并重建窗口、UI 后端与全部纹理 —— 在绘制中途调用会留下一堆悬空句柄。
 * 触发条件见 preferences_present.c: 初始化失败, 或连续 N 帧提交不出去。
 */
void bongo_cat_preferences_vulkan_fallback(BongoCatPreferences *value);
void bongo_cat_preferences_release_idle_window(BongoCatPreferences *value);
bool bongo_cat_preferences_scale_event(BongoCatPreferences *value,
    const SDL_Event *event);
bool bongo_cat_preferences_refresh_raster(BongoCatPreferences *value);
bool bongo_cat_preferences_reload_fonts(BongoCatPreferences *value);
bool bongo_cat_preferences_reload_language(BongoCatPreferences *value);
void bongo_cat_preferences_drag_tick(BongoCatPreferences *value);
void bongo_cat_preferences_live_resize_install(BongoCatPreferences *value);
void bongo_cat_preferences_live_resize_uninstall(BongoCatPreferences *value);
void bongo_cat_preferences_record_frame(BongoCatPreferences *value);
void bongo_cat_preferences_assets_load(BongoCatPreferences *value);
void bongo_cat_preferences_support_assets_load(BongoCatPreferences *value);
void bongo_cat_preferences_support_assets_clear(BongoCatPreferences *value);
void bongo_cat_preferences_process_model_selection(BongoCatPreferences *value);
void bongo_cat_preferences_model_visual_begin(BongoCatPreferences *value,
    const char *model_id);
void bongo_cat_preferences_model_load_progress(BongoCatPreferences *value,
    float progress);
float bongo_cat_preferences_model_visual_progress(BongoCatPreferences *value,
    const char *model_id);
void bongo_cat_preferences_assets_clear(BongoCatPreferences *value);
void bongo_cat_preferences_assets_abandon(BongoCatPreferences *value);
void bongo_cat_preferences_model_cover_cache_clear(BongoCatApp *app);
void bongo_cat_preferences_page_cache_clear(BongoCatPreferences *value,
    int previous_page, int next_page);
void bongo_cat_preferences_smoke_frame(BongoCatPreferences *value);
void bongo_cat_preferences_icon_draw(BongoCatPreferences *value,
    struct nk_command_buffer *canvas, int icon, struct nk_rect bounds,
    struct nk_color color);

#endif
