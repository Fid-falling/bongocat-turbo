#ifndef BONGO_CAT_FACETRACK_HTTP_H
#define BONGO_CAT_FACETRACK_HTTP_H

#include "bongo_cat/common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 面捕组件下载用的极简 HTTP。
 *
 * 为什么不复用更新器的 HTTP: 那条路径紧耦合在 BongoCatUpdateService 上 (会话/连接/请求
 * 句柄都是它的字段, 还要处理取消、镜像前缀、进度事件), 而这里只需要"把一个小文件抓到
 * 内存/磁盘"。两件事的失败语义也不同 —— 更新失败要提示用户重试, 组件下载失败只需要
 * 在设置页显示一行原因。
 *
 * 两个入口:
 *   get_memory: 清单 JSON (几十 KB), 一次读完。
 *   get_file:   组件文件 (最大 11 MB), 边收边写, 每块回调一次让调用方更新进度。
 *
 * 返回 false 时 error 里是给人看的原因; 所有临时文件由调用方负责清理。
 */
typedef bool (*BongoCatFacetrackProgress)(uint64_t received, uint64_t total, void *user);

bool bongo_cat_facetrack_http_get_memory(const char *url, char **output,
    size_t *length, char *error, size_t error_capacity);

/* time_limit_ms > 0 时, 总耗时超过它就中止 (返回 false, error="太慢")。
   用来给每个下载源快速测速: 慢源不必等它把 11 MB 拖完。 */
/* min_bytes_per_second > 0 时, 下载超过 1.5 秒后如果平均速度低于它, 立刻放弃这个源。
   用于"选源": 直连 GitHub 常常能连但只有十几 KB/s, 光看成功/失败永远选不出快源。 */
bool bongo_cat_facetrack_http_get_file(const char *url, const char *path,
    BongoCatFacetrackProgress progress, void *user, uint64_t *downloaded,
    uint64_t time_limit_ms, uint64_t min_bytes_per_second, char *error,
    size_t error_capacity);

#endif
