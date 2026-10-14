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
void bongo_cat_preferences_page_model(BongoCatPreferences *value,
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
/* 异步文件夹对话框的结果是 SDL 用户事件, 没有窗口 ID, 必须在按窗口过滤之前处理 */
bool bongo_cat_preferences_cache_path_dialog_event(BongoCatPreferences *value,
    const SDL_Event *event);
/* [窗口] 区的"透明背景"开关 (window.glTransparency)。定义只在 Windows 编译
   (preferences_window_transparency.c), 其它平台不显示这一行, 调用点也要用
   #ifdef _WIN32 包起来。 */
void bongo_cat_preferences_window_transparency(BongoCatApp *app,
    struct nk_context *context);
/* [模型] 区的"渲染后端"分段选择器 (Auto/OpenGL/Vulkan)。定义在
   preferences_renderer_row.c: 它把点击翻译成一次共享的后端切换请求
   (runtime/lifecycle/renderer_switch.c, 与托盘菜单同一个实现)。 */
void bongo_cat_preferences_renderer_row(BongoCatApp *app,
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