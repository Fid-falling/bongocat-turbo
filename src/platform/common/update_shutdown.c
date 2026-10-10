#include "bongo_cat/platform.h"

/*
 * 更新交棒所需的两个平台钩子, 在非 Windows 平台上的空实现。
 *
 * 必须存在而不是塞进 #ifdef: 启动流程(app_entry.c)会无条件调用
 * bongo_cat_platform_update_shutdown_argument, 主循环(app_loop.c)会无条件调用
 * bongo_cat_platform_single_instance_take_update_shutdown。
 * 非 Windows 没有"更新时让旧进程退出、新进程接管"这套机制, 所以恒返回 false,
 * 让调用方按"不需要交棒"继续。
 */
#ifndef _WIN32
bool bongo_cat_platform_update_shutdown_argument(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return false;
}

bool bongo_cat_platform_single_instance_take_update_shutdown(void) {
    return false;
}
#endif
