#ifndef BONGO_CAT_PREFERENCES_TEXT_EDIT_H
#define BONGO_CAT_PREFERENCES_TEXT_EDIT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * 单行文本编辑的纯逻辑接口 (无绘制、无 SDL 依赖, 实现在 preferences_text_edit.c)。
 * 约定: cursor 是**字节偏移**, 但所有对外操作都保证落在 UTF-8 字符边界上;
 * 所有函数都接受空指针 (text/cursor/select_all 可以部分缺失), 因为同一套逻辑既服务
 * 完整的编辑会话, 也服务于只需要其中一两项状态的调用方。
 * 返回 bool 的是"内容/光标变了, 需要重绘"。
 */
typedef enum BongoCatTextEditMove {
    BONGO_CAT_TEXT_EDIT_LEFT,
    BONGO_CAT_TEXT_EDIT_RIGHT,
    BONGO_CAT_TEXT_EDIT_HOME,
    BONGO_CAT_TEXT_EDIT_END
} BongoCatTextEditMove;

void bongo_cat_text_edit_begin(const char *text, size_t *cursor,
    bool *select_all);
bool bongo_cat_text_edit_insert(char *text, size_t capacity, size_t *cursor,
    bool *select_all, const char *inserted);
bool bongo_cat_text_edit_erase(char *text, size_t *cursor,
    bool *select_all, bool forward);
bool bongo_cat_text_edit_move(const char *text, size_t *cursor,
    bool *select_all, BongoCatTextEditMove move);
void bongo_cat_text_edit_select_all(const char *text, size_t *cursor,
    bool *select_all);
void bongo_cat_text_edit_clear(char *text, size_t *cursor, bool *select_all);
void bongo_cat_text_edit_trim(char *text);
size_t bongo_cat_text_edit_nearest(const char *text, float target,
    float (*measure)(const void *userdata, const char *text, size_t length),
    const void *userdata);

#endif
