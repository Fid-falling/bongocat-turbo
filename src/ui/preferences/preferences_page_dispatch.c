#include "preferences_page_dispatch.h"
#include "preferences_theme.h"
#include "preferences_internal.h"
#include "bongo_cat/i18n.h"

/*
 * 页面下标就是侧栏顺序, 也是 scroll_current/scroll_target 的索引, 以及
 * page_cache_clear 判断"离开的是哪一页"的依据 —— 四个地方共用同一个编号约定,
 * 加页/换序时必须一起改。default 兜底到"关于"页 (下标 3)。
 */
void bongo_cat_preferences_draw_page(BongoCatPreferences *value,
    struct nk_context *context) {
    switch (value->page) {
    case 0: bongo_cat_preferences_page_settings(value, context); break;
    case 1: bongo_cat_preferences_page_model(value, context); break;
    case 2: bongo_cat_preferences_page_shortcuts(value, context); break;
    default: bongo_cat_preferences_page_about(value, context); break;
    }
}

/*
 * 偏好设置页顶部的切页条: 两个子页 —— 左 (应用/外观/性能) 与 右 (窗口/模型/权限)。
 *
 * 它只切 value->preferences_tab 这一个子页下标, 刻意不改 value->page: 侧栏仍然是原来的
 * 四项, 点一下切页条也不会跳到侧栏别的页的设置内容上 (这正是最初要解除的行为)。
 * 样式与模型页顶部那条一致 (见 bongo_cat_pref_page_tabs)。左子页的文案沿用这一页自己的
 * 标题 key, 右子页是新增的一条 (pages.preference.cat.tabs.interaction)。
 *
 * 返回 true 表示"占了一行 (44 + 余量)", 调用方要把内容区高度减掉 50。
 * interactive 为假 (有模态弹窗时) 只画不响应点击, 免得点穿到弹窗底下的页面上。
 *
 * 它只影响 preferences_tab, 所以不必像以前那样抢在"按页号取值"的代码之前调用:
 * 滚动状态、页面组的 id 用的仍是 value->page, 这一帧不会变。
 */
bool bongo_cat_preferences_subnavigation(BongoCatPreferences *value,
    struct nk_context *context, bool interactive) {
    if (value->page != 0) {
        /*
         * 不在偏好设置页时把子页收回默认值: 否则用户切去别的页再回到这一页, 看到的还是
         * 上次那个子页, 与刚打开时的初始状态不一致。
         */
        value->preferences_tab = 0;
        return false;
    }
    const char *labels[] = {
        bongo_cat_i18n_get(value->app->i18n, "pages.preference.cat.title",
            "Preferences"),
        bongo_cat_i18n_get(value->app->i18n,
            "pages.preference.cat.tabs.interaction", "Interaction")};
    int selected = bongo_cat_pref_page_tabs(context, "preferences-sections",
        labels, 2, value->preferences_tab, interactive);
    if (interactive && selected != value->preferences_tab) {
        value->preferences_tab = selected;
        /* 换了子页就从顶部看起, 否则会把上一子页的滚动位置带过去 */
        value->scroll_target[0] = 0.0f;
        value->render_dirty = true;
    }
    return true;
}
