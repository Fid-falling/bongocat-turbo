#ifndef BONGO_CAT_PREFERENCES_MODEL_CARD_H
#define BONGO_CAT_PREFERENCES_MODEL_CARD_H

#include "bongo_cat/model.h"
#include "bongo_cat/preferences.h"

struct nk_context;

/*
 * 模型页的卡片构件。
 * 契约: 这三个绘制函数都从"当前 nk 布局行"里领走一格 (nk_widget), 所以调用方必须
 * 先用 nk_layout_row_dynamic 排好列数; import_card 与 model_card 的返回值是"用户点了
 * 它", 而 model_select 只登记 pending 请求, 真正的切换发生在帧末
 * (bongo_cat_preferences_process_model_selection)。
 */
bool bongo_cat_preferences_model_import_card(BongoCatPreferences *value,
    struct nk_context *context);
bool bongo_cat_preferences_model_section(BongoCatPreferences *value,
    struct nk_context *context);
void bongo_cat_preferences_model_card(BongoCatPreferences *value,
    struct nk_context *context, const BongoCatModelEntry *entry,
    bool storage_busy);
void bongo_cat_preferences_model_select(BongoCatPreferences *value,
    const BongoCatModelEntry *entry);

#endif
