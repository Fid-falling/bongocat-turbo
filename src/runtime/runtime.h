/*
 * runtime 模块的私有 API 清单 (对外只有 bongo_cat/app.h 那一套)。这里只有声明, 实现按
 * 职责分在 lifecycle / shell / model / input / diagnostics / update 下。
 * 建议阅读顺序: app_entry.c (进程入口) -> app.c (装配顺序与各 startup stage) ->
 * app_loop.c (主循环的四个阶段) -> config_store.c 与 memory_policy.c (两条跨模块策略)。
 * 调用时序上的硬约束 (改这些声明之前先看实现):
 *   - bongo_cat_startup_prepare 必须最先调用: 它解析参数并算出存储路径, 之后才能
 *     config_store_load (要读路径); "configuration-ready" 之后才允许依赖设置值;
 *   - 窗口与平台相关的调用都要求 window-ready 之后 (platform_init 依赖 SDL_Window);
 *   - 诊断/自测入口 (*_audit_run / *_self_test) 只在 --ci-* 参数下才真的有动作;
 *   - shutdown 侧的 *_destroy 全部容忍 NULL, 因为启动失败时对象是半初始化的。
 * 这个头被 runtime 内部大量 .c 包含: 只加声明, 不要在这里放实现或全局变量。
 */
#ifndef BONGO_CAT_RUNTIME_INTERNAL_H
#define BONGO_CAT_RUNTIME_INTERNAL_H

#include "bongo_cat/app.h"
#include "bongo_cat/memory_policy.h"
#include "update_service.h"
#include "window_snapshot.h"
#include <SDL3/SDL.h>
#include <stdio.h>

/* --- 生命周期与窗口: 装配、主循环、模型目录扫描与热更新 --- */
BongoCatResult bongo_cat_window_create(BongoCatApp *app, BongoCatError *error);
bool bongo_cat_app_initialize(BongoCatApp *app, int argc,
    char **argv, BongoCatError *error);
void bongo_cat_app_loop(BongoCatApp *app);
BongoCatResult bongo_cat_model_catalog_scan(BongoCatApp *app, bool cleanup,
    const char *nearby_root);
void bongo_cat_model_catalog_finish(BongoCatApp *app);
bool bongo_cat_model_catalog_add_bundled(BongoCatApp *app, bool replace);
void bongo_cat_model_catalog_finish_package(BongoCatApp *app,
    const char *package_id);
void bongo_cat_model_refresh_invalidate(BongoCatApp *app);
bool bongo_cat_model_refresh_event(BongoCatApp *app,
    const SDL_Event *event);
void bongo_cat_model_refresh_update(BongoCatApp *app);
void bongo_cat_model_refresh_shutdown(BongoCatApp *app);
BongoCatResult bongo_cat_app_locate_assets(BongoCatApp *app, BongoCatError *error);
bool bongo_cat_startup_prepare(BongoCatApp *app, int argc, char **argv,
    BongoCatError *error);
bool bongo_cat_startup_arguments(BongoCatApp *app, int argc, char **argv,
    BongoCatError *error);
/* --help/-h: 打印用法并让进程以 0 退出, 不建窗口、不占单实例锁 */
bool bongo_cat_cli_wants_help(int argc, char **argv);
void bongo_cat_cli_print_usage(FILE *out);
void bongo_cat_startup_stage(BongoCatApp *app, const char *stage);
void bongo_cat_startup_ready(BongoCatApp *app);
void bongo_cat_runtime_stage(BongoCatApp *app, const char *stage);
void bongo_cat_runtime_log_stop(void);
/* 自动重启前把当前会话日志留成 BongoCat.previous.log (新进程启动会清空 BongoCat.log,
   不先留一份的话"为什么重启"的证据就没了)。 */
void bongo_cat_runtime_log_preserve(void);
void bongo_cat_runtime_clean_shutdown(BongoCatApp *app, int exit_code);
void bongo_cat_app_shutdown(BongoCatApp *app, const char *stage,
    int exit_code);
void bongo_cat_startup_failure(BongoCatApp *app, const BongoCatError *error);
void bongo_cat_startup_ci_failure(BongoCatApp *app, const BongoCatError *error);
/* --- 窗口: 应用设置、事件、几何/缩放、拖拽与命中 --- */
void bongo_cat_window_destroy(BongoCatApp *app);
void bongo_cat_window_apply(BongoCatApp *app);
/* 应用"只在录屏软件里显示"设置 (Windows: DWM 隐藏窗口, 桌面不显示但可采集) */
void bongo_cat_window_apply_capture_only(BongoCatApp *app);
bool bongo_cat_window_event(BongoCatApp *app, const SDL_Event *event);
/* 主窗口背景色 (OBS 背景模式), 两条渲染路径共用 */
void bongo_cat_window_background_rgba(const BongoCatApp *app, float rgba[4]);
/* Vulkan 后端的主窗口一帧: 开始 -> 绘制 -> 提交呈现 */
bool bongo_cat_vk_render_main(BongoCatApp *app, bool present);
bool bongo_cat_window_visible_at_pointer(BongoCatApp *app, float x, float y);
void bongo_cat_window_capture_pointer_hit(BongoCatApp *app);
void bongo_cat_window_mark_hit_dirty(BongoCatApp *app);
void bongo_cat_window_set_visible(BongoCatApp *app, bool visible);
void bongo_cat_window_raise_when_due(BongoCatApp *app, uint64_t now);
void bongo_cat_window_schedule_pointer_hit(BongoCatApp *app);
void bongo_cat_window_schedule_hit_check(BongoCatApp *app);
int bongo_cat_window_wait_timeout(const BongoCatApp *app, uint64_t now);
bool bongo_cat_window_wait_timeout_self_test(void);
bool bongo_cat_modal_frame_self_test(void);
bool bongo_cat_model_frame_due(const BongoCatApp *app, uint64_t now);
/* 把"最大帧率"设置下发给呈现路径 (无限 -> 关闭垂直同步); 值变化时才生效 */
void bongo_cat_app_apply_frame_pacing(BongoCatApp *app);
bool bongo_cat_wait_event(SDL_Event *event, int timeout_ms);
bool bongo_cat_app_step_live2d(BongoCatApp *app, float elapsed_seconds);
void bongo_cat_window_sync_click_through(BongoCatApp *app);
/* 把"透明背景"设置下发给平台 (代理分层呈现的总开关); 只在设置/采集模式变化时调用,
   不要放进逐帧路径 —— 见 window_hit.c 的实现说明。 */
void bongo_cat_window_apply_transparency(BongoCatApp *app);
void bongo_cat_window_apply_pending_resize(BongoCatApp *app);
void bongo_cat_window_wheel(BongoCatApp *app, const SDL_MouseWheelEvent *event);
void bongo_cat_window_update_wheel_animation(BongoCatApp *app, uint64_t now);
void bongo_cat_window_cancel_wheel_animation(BongoCatApp *app);
bool bongo_cat_window_wheel_self_test(BongoCatApp *app);
bool bongo_cat_window_scaled_size(int base_width, int base_height, float base_scale,
    float requested_scale, float *actual_scale, int *width, int *height);
bool bongo_cat_window_frame_size(BongoCatApp *app,
    int content_width, int content_height, int *width, int *height,
    int *left, int *top);
bool bongo_cat_window_content_size(BongoCatApp *app,
    int width, int height, int *content_width, int *content_height);
bool bongo_cat_window_apply_geometry(BongoCatApp *app, int x, int y,
    float scale, int width, int height);
bool bongo_cat_window_set_scale(BongoCatApp *app, float scale);
bool bongo_cat_window_apply_scale_centered(BongoCatApp *app, float scale,
    int base_width, int base_height, float base_scale,
    float center_x, float center_y);
void bongo_cat_window_clamp_to_display(BongoCatApp *app);
/* 该按哪块显示器算边界: 拖动/缩放中看指针所在的那块, 其余按窗口、矩形、主显示器逐级回退。
   夹取 (clamp_to_display) 与滑行撞边 (window_inertia.c) 共用这一份实现。 */
SDL_DisplayID bongo_cat_window_target_display(BongoCatApp *app,
    const SDL_Rect *rect);
void bongo_cat_window_drag_to(BongoCatApp *app, int x, int y);
void bongo_cat_window_drag_bounds_refresh(BongoCatApp *app);
void bongo_cat_window_drag_bounds_clear(BongoCatApp *app);
bool bongo_cat_window_recover_to_display(BongoCatApp *app);
void bongo_cat_window_display_event(BongoCatApp *app, const SDL_Event *event);
void bongo_cat_window_update_display_recovery(BongoCatApp *app, uint64_t now);
void bongo_cat_window_update_inertia(BongoCatApp *app, uint64_t now);
/* 立刻结束松手滑行 (幂等): 抓住宠物、关掉"拖动惯性"或"关闭动量衰减"、窗口隐藏/最小化、
   程序退出都要调。"关闭动量衰减"的滑行不会自己停下, 这些停止条件必须显式收口。 */
void bongo_cat_window_stop_inertia(BongoCatApp *app);
bool bongo_cat_window_display_self_test(BongoCatApp *app);
void bongo_cat_window_drag_begin(BongoCatApp *app,
    const SDL_MouseButtonEvent *event);
void bongo_cat_window_poll_global_drag(BongoCatApp *app);
void bongo_cat_window_drag_motion(BongoCatApp *app,
    const SDL_MouseMotionEvent *event);
void bongo_cat_window_drag_end(BongoCatApp *app);
void bongo_cat_window_resize_by_pointer(BongoCatApp *app, const SDL_Event *event);
/* --- 输入: 手柄、鼠标映射与跟踪、快捷键、悬停淡出 --- */
const char *bongo_cat_gamepad_axis_name(Uint8 axis);
const char *bongo_cat_gamepad_button_name(Uint8 button);
void bongo_cat_gamepads_set_enabled(BongoCatApp *app, bool enabled);
void bongo_cat_app_reset_gamepad(BongoCatApp *app);
void bongo_cat_app_apply_mouse(BongoCatApp *app);
void bongo_cat_app_reset_pointer_tracking(BongoCatApp *app);
void bongo_cat_app_drain_input(BongoCatApp *app, bool allow_shortcuts);
void bongo_cat_app_apply_mouse_position(BongoCatApp *app, double x, double y,
    float elapsed_seconds);
bool bongo_cat_app_audit_screen_pointer(BongoCatApp *app);
bool bongo_cat_app_audit_display_pointer(BongoCatApp *app);
void bongo_cat_app_track_hover(BongoCatApp *app, double x, double y);
void bongo_cat_app_update_hover(BongoCatApp *app, uint64_t now);
void bongo_cat_app_update_hover_fade(BongoCatApp *app, uint64_t now);
void bongo_cat_app_cancel_hover_fade(BongoCatApp *app);
bool bongo_cat_app_shortcuts_self_test(BongoCatApp *app);
bool bongo_cat_app_sound_shortcuts(BongoCatApp *app, const BongoCatInputEvent *event,
    bool *changed);
void bongo_cat_window_menu_action(BongoCatApp *app, BongoCatMenuAction action);
bool bongo_cat_window_menu_self_test(BongoCatApp *app);
bool bongo_cat_window_geometry_self_test(BongoCatApp *app);
void bongo_cat_window_show_context_menu(BongoCatApp *app);
/* --- 诊断与冒烟: 只在 --ci-* 下真的有动作, 实现见 diagnostics/ --- */
void bongo_cat_live2d_audit_run(BongoCatApp *app);
bool bongo_cat_live2d_visual_audit_run(BongoCatApp *app);
bool bongo_cat_live2d_viewer_audit_run(BongoCatApp *app);
void bongo_cat_frame_audit(BongoCatApp *app, int width, int height);
void bongo_cat_frame_presentation_prepare(BongoCatApp *app,
    const unsigned char *pixels, int width, int height, bool visible);
void bongo_cat_frame_presented_audit(BongoCatApp *app);
void bongo_cat_window_clear_background(BongoCatApp *app);
void bongo_cat_window_mask_corners(BongoCatApp *app, int width, int height);
void bongo_cat_window_destroy_corner_mask(void);
void bongo_cat_app_render_now(BongoCatApp *app);
/*
 * 切换渲染后端的唯一入口 (设置页的分段选择器与托盘菜单共用)。
 * 顺序: 校验可用性 -> 写配置 -> **立刻落盘** -> 记日志 -> 确认框 -> 按选择重启或只提示;
 * 目标后端不可用时只记日志 + 给用户可见提示, 绝不写入配置。
 * entry 只影响日志与提示的写法 (排查时要能分辨用户点的是哪一个入口)。
 * 返回 true = 这次点击被处理了 (不等于"已经切过去了": 那要等重启)。
 */
typedef enum BongoCatRendererEntry {
    BONGO_CAT_RENDERER_ENTRY_SETTINGS = 0,
    BONGO_CAT_RENDERER_ENTRY_TRAY
} BongoCatRendererEntry;
bool bongo_cat_app_switch_renderer(BongoCatApp *app, BongoCatRenderer target,
    BongoCatRendererEntry entry);
bool bongo_cat_app_capture_pending_model_cover(BongoCatApp *app);
/* --- 运行时策略: 流程脚本、系统语言、配置落盘、多宠协同 --- */
void bongo_cat_runtime_flow_update(BongoCatApp *app, uint64_t now);
void bongo_cat_random_expression_update(BongoCatApp *app, uint64_t now);
void bongo_cat_random_expression_reset(BongoCatApp *app);
void bongo_cat_random_motion_update(BongoCatApp *app, uint64_t now);
void bongo_cat_random_motion_reset(BongoCatApp *app);
void bongo_cat_random_sound_update(BongoCatApp *app, uint64_t now);
void bongo_cat_random_sound_reset(BongoCatApp *app);
void bongo_cat_sequential_model_update(BongoCatApp *app, uint64_t now);
void bongo_cat_sequential_model_reset(BongoCatApp *app);
bool bongo_cat_system_language(BongoCatLanguage *language);
void bongo_cat_config_store_load(BongoCatApp *app);
void bongo_cat_config_store_update(BongoCatApp *app, uint64_t now);
void bongo_cat_config_store_flush(BongoCatApp *app);
/* 主循环末尾执行 app->restart_requested: 先落盘设置, 再还回单实例锁并拉起新进程;
   失败就抢回锁、保持原样并提示手动重启 (见 application_restart.c)。 */
void bongo_cat_app_update_restart(BongoCatApp *app);
bool bongo_cat_multi_pet_secondary_argument(int argc, char **argv);
bool bongo_cat_multi_pet_state_directory(char *target, size_t capacity,
    const char *root, const char *model_id);
void bongo_cat_multi_pet_update(BongoCatApp *app, uint64_t now);
void bongo_cat_multi_pet_prune_selection(BongoCatApp *app);
void bongo_cat_multi_pet_primary_update(BongoCatApp *app, uint64_t now);
bool bongo_cat_multi_pet_request_remove(BongoCatApp *app);
bool bongo_cat_multi_pet_request_preferences(BongoCatApp *app);
bool bongo_cat_multi_pet_request_exit(BongoCatApp *app);
bool bongo_cat_multi_pet_request_pass_through(BongoCatApp *app,
    bool enabled);
void bongo_cat_multi_pet_primary_requests_update(BongoCatApp *app);
void bongo_cat_multi_pet_clear_primary_request(BongoCatApp *app,
    const char *model_id);
void bongo_cat_multi_pet_pass_through_requests_update(BongoCatApp *app);
void bongo_cat_multi_pet_clear_pass_through_request(BongoCatApp *app,
    const char *model_id);
bool bongo_cat_multi_pet_take_remove_request(BongoCatApp *app,
    const char *model_id);
void bongo_cat_multi_pet_clear_remove_request(BongoCatApp *app,
    const char *model_id);
void bongo_cat_multi_pet_shutdown(BongoCatApp *app);

#endif