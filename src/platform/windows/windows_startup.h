#ifndef BONGO_CAT_WINDOWS_STARTUP_H
#define BONGO_CAT_WINDOWS_STARTUP_H

#ifdef _WIN32
#include "bongo_cat/common.h"
#include <wchar.h>

/*
 * 单实例、更新交棒与开机自启(Windows)。
 *
 * 单实例靠一个命名互斥体(Local\ 命名空间 = 当前登录会话), 配套三个事件和一块共享
 * 内存:
 *   - wake 事件: 第二个实例请求把自己唤到前台;
 *   - update-shutdown 事件: "请退出, 我要更新";
 *   - stopped 事件: 旧实例已退出的通知(新实例会等它);
 *   - 共享内存: 写入可执行文件路径与版本, 让新实例知道旧实例是谁。
 * 这些名字都会被 initialize_identity 加上可选后缀(BONGO_CAT_TEST_INSTANCE_ID), 好让
 * 并行跑的测试互不干扰 —— 改名字必须同时改所有 CreateEvent / OpenEvent 的调用点。
 */
#define BONGO_CAT_WINDOWS_INSTANCE_PATH_CAP 2048

typedef struct BongoCatWindowsInstanceInfo {
    uint32_t magic;
    wchar_t executable_path[BONGO_CAT_WINDOWS_INSTANCE_PATH_CAP];
    char version[BONGO_CAT_UPDATE_VERSION_CAP];
} BongoCatWindowsInstanceInfo;

const wchar_t *bongo_cat_windows_instance_title(void);
const wchar_t *bongo_cat_windows_update_shutdown_name(void);
const wchar_t *bongo_cat_windows_instance_stopped_name(void);
const wchar_t *bongo_cat_windows_instance_info_name(void);
/* 更新交棒: 请求已存在的实例退出并等它让出位置(实现见 windows_update_handoff.c)。
   返回 true 表示现在可以接管那些命名对象。 */
bool bongo_cat_windows_update_handoff(void);
#endif

#endif
