#ifndef BONGO_CAT_FACETRACK_INSTALL_H
#define BONGO_CAT_FACETRACK_INSTALL_H

#include "bongo_cat/common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 面捕组件的按需下载 (工作线程 + 进度快照)。
 *
 * 数据来源: 组件仓库 release 里的 manifest.json, 里面每个文件带自己的 SHA256。
 * 逐个文件下载 -> 校验 -> 落到 <components>/facetrack/<path>; 不解压, 所以没有解压器,
 * 也不会出现"解压到一半的半成品目录"。
 *
 * 与更新器的区别: 组件是**可选**的, 下载失败不影响桌宠本身, 用户随时可以再试。
 * 因此这里没有镜像前缀/断点续传, 只有状态 + 一句人话原因。
 */
typedef enum BongoCatFacetrackInstallState {
    BONGO_CAT_FACETRACK_INSTALL_IDLE = 0,
    BONGO_CAT_FACETRACK_INSTALL_WORKING,
    BONGO_CAT_FACETRACK_INSTALL_DONE,
    BONGO_CAT_FACETRACK_INSTALL_FAILED
} BongoCatFacetrackInstallState;

typedef struct BongoCatFacetrackInstallSnapshot {
    BongoCatFacetrackInstallState state;
    /* 当前第几个文件 (从 1 开始) 与总数, 用于"2/4"这种显示 */
    uint32_t file_index;
    uint32_t file_count;
    char file_name[64];
    /* 当前文件已收字节与总字节 (总字节为 0 表示服务器没给长度) */
    uint64_t file_bytes;
    uint64_t file_total;
    /* 整体: 已完成的文件字节 + 当前文件已收 */
    uint64_t bytes;
    uint64_t total;
    char error[192];
} BongoCatFacetrackInstallSnapshot;

typedef struct BongoCatFacetrackInstall BongoCatFacetrackInstall;

/* 创建 (不启动下载)。target_root = <storage>/components/facetrack。 */
BongoCatFacetrackInstall *bongo_cat_facetrack_install_create(const char *target_root);
void bongo_cat_facetrack_install_destroy(BongoCatFacetrackInstall *install);

/* 开始下载 (幂等: 正在跑就返回 true)。 */
bool bongo_cat_facetrack_install_start(BongoCatFacetrackInstall *install);
/* 请求停止并等线程退出。 */
void bongo_cat_facetrack_install_stop(BongoCatFacetrackInstall *install);

void bongo_cat_facetrack_install_snapshot(BongoCatFacetrackInstall *install,
    BongoCatFacetrackInstallSnapshot *out);

/* 组件是否已经完整落盘 (DLL + 模型都在)。用于决定状态行显示"下载"还是"可用"。 */
bool bongo_cat_facetrack_install_present(const char *target_root);

/* 清单地址: 组件仓库 release 的 latest/download/manifest.json (资产名固定, 升版本不用改客户端)。 */
#define BONGO_CAT_FACETRACK_MANIFEST_URL \
    "https://github.com/Fid-falling/bongocat-turbo-facetrack/releases/latest/download/manifest.json"

#endif
