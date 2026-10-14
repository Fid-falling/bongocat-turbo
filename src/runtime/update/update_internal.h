/*
 * 更新模块的私有头: 只有 update_* 这几个 .c 包含, 不对外导出。
 * 这里固定两件事: 一是并发模型 —— mutex 保护界面可见的状态字段, http_mutex 保护取消
 * 标志与在飞的网络句柄; 二是内部接口契约 —— http_fetch 的平台实现写在 update_http.c
 * (Windows) 与 update_http_unix.c (Unix), 平台策略写在 update_channel.c。
 *
 * 一键更新 (检查 -> 下载 -> 校验 -> 替换 -> 重启) 在这套状态机上又加了三段, 并发约定不变,
 * 只多一条: worker_done 才是"工作线程已经交卷"的唯一判据。早期版本用"status 不再是
 * CHECKING"来判断, 加了下载阶段之后它会误判 (下载中状态不是 CHECKING, 线程却还在跑),
 * 主线程一旦据此 join 就会卡住整个事件循环。见 reap_worker。
 */
#ifndef BONGO_CAT_UPDATE_INTERNAL_H
#define BONGO_CAT_UPDATE_INTERNAL_H

#include "update_service.h"
#include "bongo_cat/app.h"
#include "bongo_cat/update_source.h"
#include <SDL3/SDL.h>

/* 抓取结果: 只有 OK 时 *response 有效; CANCELLED 会让工作线程静默退出且不发事件; 其余
 * 取值都必须带 error 文案上报。UNSUPPORTED 目前只有"非 Windows 的自更新下载"会返回。 */
typedef enum BongoCatUpdateFetchResult {
    BONGO_CAT_UPDATE_FETCH_OK,
    BONGO_CAT_UPDATE_FETCH_CANCELLED,
    BONGO_CAT_UPDATE_FETCH_UNSUPPORTED,
    BONGO_CAT_UPDATE_FETCH_NETWORK,
    BONGO_CAT_UPDATE_FETCH_RESPONSE,
    BONGO_CAT_UPDATE_FETCH_MEMORY,
    /* 本地目标文件写不进去 (安装目录只读、磁盘满、被杀软拦截): 与网络失败分开, 因为
       调用方要据此退到"下载到缓存目录, 请用户手动替换"。 */
    BONGO_CAT_UPDATE_FETCH_FILE
} BongoCatUpdateFetchResult;

struct BongoCatUpdateService {
    BongoCatApp *app;
    SDL_Mutex *mutex;
    SDL_Mutex *http_mutex;
    SDL_Thread *worker;
    Uint32 event_type;
    BongoCatUpdateStatus status;
    BongoCatUpdateRelease release;
    char error[256];
    bool manual;
    bool open_after_check;
    bool installed;
    bool shutting_down;
    /* 工作线程是否已经彻底结束 (不再碰 service 的任何字段)。只有它为 true 时主线程
       才可以 join, 见文件头。 */
    bool worker_done;
    bool http_cancelled;
    void *http_session;
    void *http_connection;
    void *http_request;

    /* ---- 一键更新 (下载 / 校验 / 替换) ---- */
    /* 本次要装的资产 (工作线程在检查完成后选定: Windows 用单文件 portable)。 */
    BongoCatUpdateAsset asset;
    /* 本次尝试的源顺序: 配置覆盖 > 内置默认, 上次成功的源被提前。只由工作线程改,
       界面通过 snapshot 里的 source_label/source_index 看到当前进度。 */
    BongoCatUpdateSourceList sources;
    /* 下载产物路径 (当前 exe 同目录下的 .new; 目录不可写时退到缓存目录)。 */
    char new_path[BONGO_CAT_PATH_CAP];
    /* 目标目录不可写: 只能下载到别处并提示用户手动替换, 不尝试自动替换。 */
    bool manual_only;
    uint64_t downloaded;
    uint64_t total;
    unsigned source_index;
    unsigned source_count;
    char source_label[BONGO_CAT_UPDATE_SOURCE_LABEL_CAP];
    /* 替换没做成: 新文件还在 new_path, 界面提示"已下载到 <路径>, 请手动替换"。 */
    bool manual_replace;
    char manual_path[BONGO_CAT_PATH_CAP];
    /* 工作线程校验通过后置位, 主线程据此执行替换并安排重启。 */
    bool install_requested;
    /*
     * "本进程还活着"的信号 (Windows 命名互斥体)。
     * 为什么不是 PID: 更新模块里不允许出现 OpenProcess 之类的跨进程句柄 API
     * (见 cmake/CheckPlatformRuntimeSafety.cmake 的 FORBIDDEN_APIS), 而"等旧进程退出"
     * 这件事可以用一个**由本进程持有、进程死亡时自动被系统放弃**的命名互斥体表达:
     * 助手进程 OpenMutexW + WaitForSingleObject 会在本进程死掉的那一刻拿到
     * WAIT_ABANDONED。句柄刻意不关闭 —— 它要活到进程结束, 由系统回收。
     */
    void *alive_mutex;
    /* 上面那个互斥体的名字 (含 PID, 便于日志里对号入座)。 */
    char alive_name[128];
};

/* update_http.c / update_http_unix.c: 抓取 release JSON 原文 */
BongoCatUpdateFetchResult bongo_cat_update_http_fetch(
    BongoCatUpdateService *service, char **response, char *error,
    size_t error_capacity);
/* 同上, 但抓指定 URL —— 目前只有"官方 .sha256 校验资产"用 (绝不用它抓镜像上的东西)。 */
BongoCatUpdateFetchResult bongo_cat_update_http_fetch_url(
    BongoCatUpdateService *service, const char *url, char **response,
    char *error, size_t error_capacity);
/* update_http.c (Windows) / update_http_unix.c (Unix 桩): 把 url 下载到 path。
 * 进度会实时写进 service->downloaded/total; 失败时自己删掉半个文件。 */
BongoCatUpdateFetchResult bongo_cat_update_http_download(
    BongoCatUpdateService *service, const char *url, const char *path,
    char *error, size_t error_capacity);
void bongo_cat_update_http_cancel(BongoCatUpdateService *service);
void bongo_cat_update_show_completion(BongoCatUpdateService *service);
bool bongo_cat_update_platform_supported(void);
bool bongo_cat_update_platform_store(void);
bool bongo_cat_update_platform_installed(void);
const char *bongo_cat_update_platform_asset(void);
/* 本平台是否支持"下载后原地替换自己" (目前只有 Windows)。false 时手动检查仍然只跳到
   发布页, 行为与没有这个功能之前完全一致。 */
bool bongo_cat_update_platform_self_update(void);

/* update_service.c: 下载进度 (工作线程写, 界面读) */
void bongo_cat_update_report_progress(BongoCatUpdateService *service,
    uint64_t downloaded, uint64_t total);
/* 阶段切换: 只改状态并发事件, 不清空上次的版本号 (与 complete 的差别)。 */
void bongo_cat_update_report_stage(BongoCatUpdateService *service,
    BongoCatUpdateStatus status);
/* 工作线程取消检查 (读取粘滞取消位) */
bool bongo_cat_update_cancelled(BongoCatUpdateService *service);

/* update_download.c: "下载并校验"阶段 (工作线程) */
bool bongo_cat_update_download_asset(BongoCatUpdateService *service, char *error,
    size_t capacity);

/* update_source_store.c: 源列表的配置覆盖与"记住上次成功的源" */
void bongo_cat_update_sources_load(BongoCatUpdateService *service);
void bongo_cat_update_sources_remember(BongoCatUpdateService *service,
    size_t index);

/* Windows 专用能力判定与替换调度见 update_install.h (runtime/lifecycle 也要用) */

#endif
