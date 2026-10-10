#include "preferences_page_dispatch.h"

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
