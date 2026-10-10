#ifndef BONGO_CAT_UI_TOOLTIP_H
#define BONGO_CAT_UI_TOOLTIP_H

#include "nuklear_config.h"

/* 画一个可点击的问号提示: question 常显, 鼠标悬停时才显示 reply 气泡。
   reply 里的 '\n' 是硬换行, 过长会自动折行。两个字符串都必须以 '\0' 结尾。 */
void bongo_cat_ui_question_tooltip(struct nk_context *context,
    const char *question, const char *reply);

#endif
