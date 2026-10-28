/*
 * 设置窗口内部的跨文件接口 (只在 src/ui/preferences/ 内部使用)。
 * 按用途分成几组: 页面绘制 (page_settings / page_model / page_shortcuts / page_about)、
 * 两个"会话型"子状态 (文本重命名、快捷键录制)、导入通路, 以及缓存目录那一行。
 *
 * 有一条容易忽略的契约: 这里所有 *_event 函数返回 true 都表示"事件已被消费",
 * 调用方 (preferences.c 的 event 分发) 必须立刻停止往下传 —— 尤其是异步对话框的
 * dialog_event, 它必须跑在"按窗口 ID 过滤"之前, 否则没有窗口 ID 的用户事件会被丢掉。
 */
#ifndef BONGO_CAT_PREFERENCES_INTERNAL_H
#define BONGO_CAT_PREFERENCES_INTERNAL_H

#include "bongo_cat/app.h"
#include "nuklear_config.h"
#include "ui_catime.h"
#include <SDL3/SDL.h>

void bongo_cat_preferences_page_settings(BongoCatPreferences *value,
    struct nk_context *context);
/* 偏好设置页两个子页共用的分区: 左子页 = 应用 + 外观 + 性能, 右子页 = 窗口 + 模型 +
   权限。这三个定义在 preferences_general_page.c, 与 [窗口]/[模型] 区没有共享状态, 拆开
   是为了 500 行的行数策略 (见 cmake/CheckLines.cmake)。入参是 BongoCatPreferences 而
   不是 BongoCatApp: 分区折叠状态 (options_sections) 挂在 preferences 上。
   [权限] 区目前只有 macOS 有内容, 其它平台是空实现。 */
void bongo_cat_preferences_application_section(BongoCatPreferences *value,
    struct nk_context *context);
void bongo_cat_preferences_appearance_section(BongoCatPreferences *value,
    struct nk_context *context);
void bongo_cat_preferences_permissions_section(BongoCatPreferences *value,
    struct nk_context *context);
/* 互动页的 [音效] 区: 总音量 / 全部静音 / 当前模型逐条开关 (实现见
   preferences_sound_section.c)。 */
void bongo_cat_preferences_sound_section(BongoCatPreferences *value,
    struct nk_context *context);
void bongo_cat_preferences_page_model(BongoCatPreferences *value,
    struct nk_context *context);
void bongo_cat_preferences_page_model_random(BongoCatPreferences *value,
    struct nk_context *context);
bool bongo_cat_preferences_behavior_dialog_active(
    const BongoCatPreferences *value);
void bongo_cat_preferences_behavior_dialog_open(
    BongoCatPreferences *value);
void bongo_cat_preferences_behavior_dialog_draw(
    BongoCatPreferences *value, struct nk_context *context);
void bongo_cat_preferences_behavior_dialog_close(
    BongoCatPreferences *value);
bool bongo_cat_preferences_behavior_rename_event(
    BongoCatPreferences *value, const SDL_Event *event);
void bongo_cat_preferences_behavior_rename_begin(BongoCatPreferences *value,
    const BongoCatBehaviorEntry *entry, BongoCatBehaviorShortcut *binding,
    struct nk_rect bounds);
void bongo_cat_preferences_behavior_rename_finish(
    BongoCatPreferences *value, bool save);
bool bongo_cat_preferences_model_rename_event(
    BongoCatPreferences *value, const SDL_Event *event);
void bongo_cat_preferences_model_rename_begin(BongoCatPreferences *value,
    const BongoCatModelEntry *entry, struct nk_rect bounds);
void bongo_cat_preferences_model_rename_finish(
    BongoCatPreferences *value, bool save);
bool bongo_cat_preferences_model_name_draw(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    const BongoCatModelEntry *entry, struct nk_rect bounds,
    BongoCatUIPalette palette);
void bongo_cat_preferences_behavior_row_draw(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect row, BongoCatBehaviorEntry *entry, BongoCatUIPalette palette,
    float opacity, bool enabled);
void bongo_cat_preferences_behavior_row_participation_draw(
    BongoCatPreferences *value, struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect row,
    BongoCatBehaviorEntry *entry, BongoCatUIPalette palette, float opacity,
    bool enabled);
void bongo_cat_preferences_page_shortcuts(BongoCatPreferences *value,
    struct nk_context *context);
void bongo_cat_preferences_page_about(BongoCatPreferences *value,
    struct nk_context *context);
bool bongo_cat_preferences_shortcut_active(const BongoCatPreferences *value,
    const char *id);
void bongo_cat_preferences_shortcut_begin(BongoCatPreferences *value,
    const char *id, char *target, int capacity);
bool bongo_cat_preferences_shortcut_event(BongoCatPreferences *value,
    const SDL_Event *event);
void bongo_cat_preferences_shortcut_cancel(BongoCatPreferences *value);
void bongo_cat_preferences_shortcut_smoke(BongoCatPreferences *value);
void bongo_cat_preferences_import_path(BongoCatApp *app, SDL_Window *window,
    const char *path);
bool bongo_cat_preferences_imports_busy(const BongoCatPreferences *value);
void bongo_cat_preferences_stop_imports(BongoCatPreferences *value);
void bongo_cat_preferences_poll_imports(BongoCatPreferences *value);
void bongo_cat_preferences_model_cache_clear(BongoCatApp *app);
bool bongo_cat_preferences_remove_dialog_active(const BongoCatApp *app);
void bongo_cat_preferences_remove_dialog_open(BongoCatApp *app, const char *id);
void bongo_cat_preferences_remove_dialog_draw(BongoCatApp *app,
    struct nk_context *context);
void bongo_cat_preferences_remove_dialog_clear(const BongoCatApp *app);
void bongo_cat_preferences_remove_dialog_close(BongoCatApp *app);
bool bongo_cat_preferences_chrome_drag_allowed(
    const BongoCatPreferences *value);
void bongo_cat_preferences_cache_path_draw(BongoCatPreferences *value,
    struct nk_context *context);
bool bongo_cat_preferences_cache_path_event(BongoCatPreferences *value,
    const SDL_Event *event);
/* 便携模式 (BongoCatData) 那一行: 开关 + 迁移反馈。判定与搬迁都在 runtime 层
   (portable_mode.c), 这里只做交互与多帧反馈。 */
void bongo_cat_preferences_portable_draw(BongoCatPreferences *value,
    struct nk_context *context);
/* 设置 → 应用 → [运行自检]: 那一行 + 结果面板 (实现见 preferences_selfcheck.c)。
   自检本身是只读的运行时诊断, 实现在 runtime/diagnostics/selfcheck*.c。
   面板是窗口内模态浮层: 打开期间 chrome 拖拽与页面输入都要让路 (见 preferences.c 与
   preferences_render.c 的 modal 判定)。 */
void bongo_cat_preferences_selfcheck_row(BongoCatPreferences *value,
    struct nk_context *context);
void bongo_cat_preferences_selfcheck_dialog_draw(BongoCatPreferences *value,
    struct nk_context *context);
bool bongo_cat_preferences_selfcheck_active(const BongoCatApp *app);
void bongo_cat_preferences_selfcheck_close(BongoCatApp *app);
/* 窗口回收时清空面板状态 (报告不跨窗口保留, 下次重新跑) */
void bongo_cat_preferences_selfcheck_reset(BongoCatPreferences *value);
/* 异步文件夹对话框的结果是 SDL 用户事件, 没有窗口 ID, 必须在按窗口过滤之前处理 */
bool bongo_cat_preferences_cache_path_dialog_event(BongoCatPreferences *value,
    const SDL_Event *event);
/* [窗口] → [背景] 区的"透明背景"开关 (window.glTransparency)。定义只在 Windows 编译
   (preferences_window_transparency.c), 其它平台不显示这一行, 调用点也要用
   #ifdef _WIN32 包起来。只对 OpenGL 后端显示 (Vulkan 走原生交换链, 这一项是 no-op)。 */
void bongo_cat_preferences_window_transparency(BongoCatApp *app,
    struct nk_context *context);
/* [模型] 区的"渲染后端"分段选择器 (Auto/OpenGL/Vulkan)。定义在
   preferences_renderer_row.c: 它把点击翻译成一次共享的后端切换请求
   (runtime/lifecycle/renderer_switch.c, 与托盘菜单同一个实现)。 */
void bongo_cat_preferences_renderer_row(BongoCatApp *app,
    struct nk_context *context);
/* [性能] 区"渲染后端"上方的自动回退提示条 (定义在
   preferences_renderer_fallback.c): 只在发生过自动回退时出现, 带一个
   「再试一次 Vulkan」按钮 (走同一条共享切换路径)。 */
void bongo_cat_preferences_renderer_fallback(BongoCatApp *app,
    struct nk_context *context);
/* [性能] 区的"纹理质量"下拉 (model.texture_quality)。定义在
   preferences_texture_quality_row.c: 图集上传前的工作尺寸上限, 默认标准档 (2048)。 */
void bongo_cat_preferences_texture_quality_row(BongoCatApp *app,
    struct nk_context *context);
void bongo_cat_preferences_cache_path_browse(BongoCatPreferences *value);
void bongo_cat_preferences_cache_path_cancel(BongoCatPreferences *value);

typedef struct BongoCatImportDialog BongoCatImportDialog;
BongoCatImportDialog *bongo_cat_preferences_import_create(void);
void bongo_cat_preferences_import_destroy(BongoCatImportDialog *dialog);
bool bongo_cat_preferences_import_open(BongoCatImportDialog *dialog,
    SDL_Window *window);
bool bongo_cat_preferences_import_is_open(const BongoCatImportDialog *dialog);
bool bongo_cat_preferences_import_status(const BongoCatImportDialog *dialog,
    uint64_t *started_ns, size_t *completed, size_t *total);
bool bongo_cat_preferences_import_event(BongoCatImportDialog *dialog,
    BongoCatApp *app, const SDL_Event *event);

#endif
