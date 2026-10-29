/*
 * 对外接口, 全部在主线程调用, 且都容忍 service == NULL (降级为不支持检查更新)。
 * 返回 false 只表示本次动作没成功发起, 调用方据此提示用户, 而不是"没有更新"。
 */
#ifndef BONGO_CAT_UPDATE_SERVICE_H
#define BONGO_CAT_UPDATE_SERVICE_H

#include "bongo_cat/update.h"
#include "bongo_cat/update_source.h"

typedef struct BongoCatApp BongoCatApp;
typedef union SDL_Event SDL_Event;
typedef struct BongoCatUpdateService BongoCatUpdateService;

/*
 * 状态机取值。界面依赖其中几种: AVAILABLE 与"ERROR 且带版本号"显示更新角标,
 * STORE/UNSUPPORTED 让按钮变成打开商店或发布页, 而不是发起检查。
 *
 * DOWNLOADING/VERIFYING/INSTALLING 是"点击检查更新后自动下载"这条链路的三个阶段, 只有
 * 手动检查 (manual) 才会进入 —— 自动检查永远停在 AVAILABLE, 只放一个角标提醒。
 * 三者都还属于"进行中": 界面据此显示进度并且不再接受重复点击。
 */
typedef enum BongoCatUpdateStatus {
    BONGO_CAT_UPDATE_IDLE,
    BONGO_CAT_UPDATE_CHECKING,
    BONGO_CAT_UPDATE_CURRENT,
    BONGO_CAT_UPDATE_AVAILABLE,
    BONGO_CAT_UPDATE_ERROR,
    BONGO_CAT_UPDATE_STORE,
    BONGO_CAT_UPDATE_UNSUPPORTED,
    /* 正在从某个下载源拉取新的单文件 exe (进度见 downloaded/total) */
    BONGO_CAT_UPDATE_DOWNLOADING,
    /* 文件已下齐, 正在重算 SHA-256 与期望值比对 */
    BONGO_CAT_UPDATE_VERIFYING,
    /* 校验通过, 正在准备替换当前 exe 并重启 (进程随时会退出) */
    BONGO_CAT_UPDATE_INSTALLING
} BongoCatUpdateStatus;

typedef struct BongoCatUpdateSnapshot {
    BongoCatUpdateStatus status;
    BongoCatUpdateRelease release;
    char error[256];
    bool installed;
    /* ---- 一键更新的可见进度 ---- */
    uint64_t downloaded;
    /* 0 表示服务端没给 Content-Length (进度只能显示已下载字节数) */
    uint64_t total;
    /* 从 1 开始: 当前正在尝试第几个源、一共几个源 */
    unsigned source_index;
    unsigned source_count;
    /* 当前源的显示名 (域名), 例如 "ghproxy.net"; "GitHub" 表示官方直连 */
    char source[BONGO_CAT_UPDATE_SOURCE_LABEL_CAP];
    /* 替换没能自动完成 (目录不可写/文件被占用/助手进程起不来): 新文件还在磁盘上,
       界面必须提示"已下载到 <manual_path>, 请手动替换"。 */
    bool manual_replace;
    char manual_path[BONGO_CAT_PATH_CAP];
    /* 这一轮检查是用户点的 (true) 还是启动时自动跑的 (false)。
       失败提示 (关于页底部状态条、"查看发布版本"链接) 只对用户主动发起的那次出现:
       自动检查是幕后行为, 失败时不该在用户没要求的时候把限流文案摆到脸上。 */
    bool manual;
} BongoCatUpdateSnapshot;

BongoCatUpdateService *bongo_cat_update_create(BongoCatApp *app);
void bongo_cat_update_start_automatic(BongoCatUpdateService *service);
bool bongo_cat_update_check(BongoCatUpdateService *service, bool manual);
bool bongo_cat_update_event(BongoCatUpdateService *service,
    const SDL_Event *event);
void bongo_cat_update_snapshot(BongoCatUpdateService *service,
    BongoCatUpdateSnapshot *snapshot);
bool bongo_cat_update_open(BongoCatUpdateService *service);
/*
 * 用户点击"检查更新"/"更新至 vX"时的唯一入口 (关于页那两个按钮语义共用它):
 * 发起一次手动检查, 发现新版本后由工作线程接着自动下载 -> 校验 -> 准备替换重启。
 * 返回 false 表示本次动作没发起 (已有检查/下载在跑, 或本平台不支持检查更新)。
 */
bool bongo_cat_update_start(BongoCatUpdateService *service);
void bongo_cat_update_destroy(BongoCatUpdateService *service);

#endif
