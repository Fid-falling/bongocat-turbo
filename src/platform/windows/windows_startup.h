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
 *   - wake 事件: 第二个实例请求唤到前台 —— 自动重置, 只表示"有人敲了一下门";
 *   - update-shutdown 事件: "请退出, 我要更新";
 *   - stopped 事件: 旧实例已退出的通知(新实例会等它);
 *   - 共享内存: 写入可执行文件路径、版本, 以及**唤醒原因**(打开设置窗口 / 只唤到前台)。
 *     事件带不了载荷, 所以"这一次想干什么"只能写在共享内存里, 由写入方在 SetEvent
 *     之前填好; 这是既有唤醒通道的扩展, 不是第二套 IPC。
 * 这些名字都会被 initialize_identity 加上可选后缀(BONGO_CAT_TEST_INSTANCE_ID), 好让
 * 并行跑的测试互不干扰 —— 改名字必须同时改所有 CreateEvent / OpenEvent 的调用点。
 */
#define BONGO_CAT_WINDOWS_INSTANCE_PATH_CAP 2048
/* 共享内存已被初始化过的标记: 读到别的值就说明这块内存不是我们写的 (旧版本结构更小,
   或者根本不是本程序建的对象), 一律当成"读不到"。 */
#define BONGO_CAT_WINDOWS_INSTANCE_MAGIC 0x42434D49u

typedef struct BongoCatWindowsInstanceInfo {
    uint32_t magic;
    wchar_t executable_path[BONGO_CAT_WINDOWS_INSTANCE_PATH_CAP];
    char version[BONGO_CAT_UPDATE_VERSION_CAP];
    /*
     * 唤醒原因 (BongoCatWakeRequest 的取值), 由第二个实例在 SetEvent 之前写入, 主实例
     * 取走唤醒时读一次并清零。
     *
     * 为什么放在结构末尾: 老版本实例创建的段更小, 新进程按新尺寸 MapViewOfFile 会失败
     * (请求长度超出段), 于是退回"只把窗口唤到前台"的旧行为, 而不是读到一个越界字段。
     * 这条前提意味着以后加字段也只能往后追加。
     */
    uint32_t wake_request;
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
