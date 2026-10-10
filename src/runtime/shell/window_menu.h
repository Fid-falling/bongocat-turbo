/* 主窗口右键径向菜单的内部接口 (仅 shell 内部使用): 菜单的标签、勾选态、实时预览与
 * "选中之后做什么"分散在 actions/preview/behavior 三个实现文件里, 这里只声明契约。
 * 菜单本体由系统侧 (Windows 原生菜单 / dial 窗口) 绘制并返回 BongoCatMenuAction; 预览与
 * restore 是主窗口被回调进去执行的, 发生在平台菜单的模态循环期间, 那段时间主循环停摆。 */
#ifndef BONGO_CAT_WINDOW_MENU_INTERNAL_H
#define BONGO_CAT_WINDOW_MENU_INTERNAL_H

#include "modal_frame.h"

/* 预览事务的会话状态: init 记下菜单打开瞬间的缩放/不透明度/表情, last 是最近一次请求
 * 试穿的动作, applied 才是已经真正落定的动作 —— restore 靠它区分"保留"与"回滚"。 */
typedef struct BongoCatWindowMenuPreview {
    BongoCatApp *app;
    float scale, opacity;
    int expression;
    BongoCatMenuAction last, applied;
    BongoCatModalFrame modal_frame;
} BongoCatWindowMenuPreview;

void bongo_cat_window_menu_preview_init(BongoCatWindowMenuPreview *state,
    BongoCatApp *app);
void bongo_cat_window_menu_preview(void *userdata, BongoCatMenuAction action);
void bongo_cat_window_menu_preview_tick(void *userdata);
void bongo_cat_window_menu_restore(void *userdata, BongoCatMenuAction selected);
bool bongo_cat_window_menu_preview_applied(
    const BongoCatWindowMenuPreview *state, BongoCatMenuAction selected);
void bongo_cat_window_audio_labels(BongoCatApp *app,
    char names[][BONGO_CAT_MENU_LABEL_CAP], bool *checked, size_t *count);
void bongo_cat_window_behavior_labels(BongoCatApp *app,
    char motions[][BONGO_CAT_MENU_LABEL_CAP], bool *motion_checked,
    size_t *motion_count, char expressions[][BONGO_CAT_MENU_LABEL_CAP],
    size_t *expression_count,
    size_t *current_expression);
bool bongo_cat_window_behavior_menu_action(BongoCatMenuAction action);
bool bongo_cat_window_behavior_action(BongoCatApp *app,
    BongoCatMenuAction action);
bool bongo_cat_window_behavior_preview(BongoCatApp *app,
    BongoCatMenuAction action);
bool bongo_cat_window_behavior_commit_preview(BongoCatApp *app,
    BongoCatMenuAction action);
bool bongo_cat_window_behavior_self_test(BongoCatApp *app);

#endif
