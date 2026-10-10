#ifndef BONGO_CAT_PREFERENCES_NUMBER_EDIT_H
#define BONGO_CAT_PREFERENCES_NUMBER_EDIT_H

#include "preferences_controls.h"

/*
 * 数字输入框 (步进器中间那个可编辑区域)。
 * 契约: 返回 true 表示"本控件现在处于编辑态", 调用方这时不要再自己把数值画一遍
 * (会重影); value 可能被就地更新, 所以传的是 double*;
 * formatted 是"未编辑时应该显示的文本", 每次进入编辑都会以它作为初始内容。
 * 全局只有一个编辑槽位, 同一帧内不要对两个数字框调用它。
 */
bool bongo_cat_pref_number_edit(struct nk_context *context, const char *id,
    struct nk_rect bounds, const char *formatted, bool integer,
    double minimum, double maximum, double *value);
void bongo_cat_pref_number_edit_reset(struct nk_context *context);

#endif
