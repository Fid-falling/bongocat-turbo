/* runtime-state.txt / 启动阶段的读写接口 (实现见 runtime_state.c)。
   日志系统在启动最早期就用它判断"上次是不是异常退出", 所以这里的函数不能依赖
   SDL 之外的任何子系统, 也不能要求 app 已经完全初始化 (state_root 为空时直接跳过)。 */
#ifndef BONGO_CAT_RUNTIME_STATE_H
#define BONGO_CAT_RUNTIME_STATE_H

#include "bongo_cat/app.h"
#include <stddef.h>

void bongo_cat_runtime_timestamp(char *target, size_t capacity);
void bongo_cat_runtime_state_previous(BongoCatApp *app, char *state,
    size_t capacity);
void bongo_cat_runtime_state_clean(BongoCatApp *app, const char *timestamp);

#endif
