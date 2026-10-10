#ifndef BONGO_CAT_WINDOWS_INPUT_H
#define BONGO_CAT_WINDOWS_INPUT_H

#include "bongo_cat/platform.h"

#ifdef _WIN32
#include <windows.h>

/*
 * Windows 全局输入采集的对外接口, 平台层只调用这几个函数。
 *
 * 内部结构见 windows_input_internal.h: 一条专用接收线程加两个 SRWLOCK 保护的共享
 * 缓冲区。take / reset / release_relative 是给"鼠标被别的程序独占或锁定"场景准备的
 * 相对位移通道, 与键鼠事件走的是不同路径。
 * start 与 stop 必须配对; start 失败时不需要再调 stop。
 */
bool bongo_cat_windows_input_start(BongoCatPlatform *platform);
void bongo_cat_windows_input_stop(BongoCatPlatform *platform);
bool bongo_cat_windows_input_take_relative(BongoCatPlatform *platform,
    double *x, double *y, unsigned long long *sample_count);
void bongo_cat_windows_input_reset_relative(BongoCatPlatform *platform);
void bongo_cat_windows_input_release_relative(BongoCatPlatform *platform);
bool bongo_cat_windows_input_pointer_locked(BongoCatPlatform *platform);
#endif

#endif
