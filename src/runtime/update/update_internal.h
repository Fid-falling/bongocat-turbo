/*
 * 更新模块的私有头: 只有 update_* 这几个 .c 包含, 不对外导出。
 * 这里固定两件事: 一是并发模型 —— mutex 保护界面可见的状态字段, http_mutex 保护取消
 * 标志与在飞的网络句柄; 二是内部接口契约 —— http_fetch 的平台实现写在 update_http.c
 * (Windows) 与 update_http_unix.c (Unix), 平台策略写在 update_channel.c。
 */
#ifndef BONGO_CAT_UPDATE_INTERNAL_H
#define BONGO_CAT_UPDATE_INTERNAL_H

#include "update_service.h"
#include "bongo_cat/app.h"
#include <SDL3/SDL.h>

/* 抓取结果: 只有 OK 时 *response 有效; CANCELLED 会让工作线程静默退出且不发事件; 其余
 * 取值都必须带 error 文案上报。UNSUPPORTED 目前没有任何实现会返回, 是保留值。 */
typedef enum BongoCatUpdateFetchResult {
    BONGO_CAT_UPDATE_FETCH_OK,
    BONGO_CAT_UPDATE_FETCH_CANCELLED,
    BONGO_CAT_UPDATE_FETCH_UNSUPPORTED,
    BONGO_CAT_UPDATE_FETCH_NETWORK,
    BONGO_CAT_UPDATE_FETCH_RESPONSE,
    BONGO_CAT_UPDATE_FETCH_MEMORY
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
    bool http_cancelled;
    void *http_session;
    void *http_connection;
    void *http_request;
};

BongoCatUpdateFetchResult bongo_cat_update_http_fetch(
    BongoCatUpdateService *service, char **response, char *error,
    size_t error_capacity);
void bongo_cat_update_http_cancel(BongoCatUpdateService *service);
void bongo_cat_update_show_completion(BongoCatUpdateService *service);
bool bongo_cat_update_platform_supported(void);
bool bongo_cat_update_platform_store(void);
bool bongo_cat_update_platform_installed(void);
const char *bongo_cat_update_platform_asset(void);

#endif
