/*
 * "关于"页 (page 3) 的组装: 支持者素材 -> 主视觉 -> 社区 -> 更多应用。
 * 素材必须先加载: 主视觉里的 logo 和推广卡片用的都是 support assets, 反了就会
 * 先画出一帧空白再补上。
 * 这三段各自用 nk_layout_row_dynamic 领自己的矩形并在里面自由绘制, 所以调用顺序
 * 就是从上到下的视觉顺序, 不能随意调换。
 */
#include "preferences_about_community.h"
#include "preferences_state.h"

void bongo_cat_preferences_about_hero(BongoCatPreferences *value,
    struct nk_context *context);
void bongo_cat_preferences_about_projects(BongoCatPreferences *value,
    struct nk_context *context);

void bongo_cat_preferences_page_about(BongoCatPreferences *value,
    struct nk_context *context) {
    bongo_cat_preferences_support_assets_load(value);
    bongo_cat_preferences_about_hero(value, context);
    bongo_cat_preferences_about_community(value, context);
    bongo_cat_preferences_about_projects(value, context);
}
