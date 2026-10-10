#ifndef BONGO_CAT_PREFERENCES_GL_H
#define BONGO_CAT_PREFERENCES_GL_H

#include <stdbool.h>

typedef struct BongoCatPreferences BongoCatPreferences;

/*
 * 设置窗口图形上下文的创建/销毁。
 * 契约: 成功后 "current" 上下文属于本窗口, 调用方用完要把主窗口的上下文设回去;
 * 失败时保证不改变主窗口的上下文状态 (由 preferences_gl.c 的 fail() 负责)。
 */
bool bongo_cat_preferences_gl_create(BongoCatPreferences *value);
void bongo_cat_preferences_gl_destroy(BongoCatPreferences *value);

#endif
