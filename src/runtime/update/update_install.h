/*
 * 自动替换 (自更新) 的接口。实现分两半:
 *   - src/runtime/update/update_install.c: 主线程侧 —— 决定落点、请求重启、失败收场;
 *   - src/platform/windows/windows_update_apply.c: 助手进程侧 —— 等旧进程退出、替换文件、
 *     重新启动 (非 Windows 是 platform/common/update_shutdown.c 里的桩)。
 * 只有 runtime/update 与 runtime/lifecycle 需要它, 所以放在 runtime/update 目录下而不是
 * include/ (它不是对外 API)。
 */
#ifndef BONGO_CAT_UPDATE_INSTALL_H
#define BONGO_CAT_UPDATE_INSTALL_H

#include "update_service.h"

/* 下载产物路径 (exe 同目录的 <exe 名>.new.exe; 不可写时退到 <cache>/updates 并置
   service->manual_only)。返回 false 表示两处都写不了。 */
bool bongo_cat_update_install_target(BongoCatUpdateService *service, char *output,
    size_t capacity);
/* 启动时的一次性收尾: 删掉上一版备份 .old, 读取助手的失败回执并按需提示用户。 */
void bongo_cat_update_install_startup(BongoCatUpdateService *service);
/* 主线程收到"校验通过"后的唯一入口: 检查前提条件并安排"退出->替换->重启"。 */
void bongo_cat_update_install_request(BongoCatUpdateService *service);
/* 主循环末尾 (application_restart.c) 调用: 拉起助手进程。参数是原样带给新进程的
   (--storage-root= 之类)。失败返回 false, 调用方必须撤销重启意图并继续运行。 */
bool bongo_cat_update_install_spawn(BongoCatApp *app, const char *restart_arguments,
    BongoCatError *error);
/* 助手进程没起来: 撤销替换意图, 把状态改成"已下载到 <路径>, 请手动替换"并提示用户。 */
void bongo_cat_update_install_abort(BongoCatUpdateService *service,
    const BongoCatError *error);

#endif
