/*
 * 更新检查的 Unix 网络实现 (libcurl): 与 update_http.c 是同一个函数的两个平台版本,
 * 两个文件都参与编译, 靠 _WIN32 互斥。返回码含义与 error 文案风格必须和 Windows 侧
 * 一致 —— 尤其 "GitHub returned HTTP status N" 会被 update_channel.c 解析来本地化。
 * 取消是协作式的: 别的线程只置标志, 由 libcurl 的进度回调返回 1 让它自己中止, 因此
 * bongo_cat_update_destroy() 最坏要等到下一次进度回调 (总时长仍被 15s 总超时兜住)。
 * 响应体上限与 Windows 侧同为 1 MiB; 成功时 *response 是 malloc 缓冲, 归调用方 free。
 */
#include "update_internal.h"

#ifndef _WIN32

#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPDATE_RESPONSE_LIMIT (1024u * 1024u)

/* 累积响应体的缓冲: data 始终 NUL 结尾 (初值 malloc(1)); failed 用来把"超限或分配
 * 失败"与网络错误区分开 —— 两者都会让 write_response 返回 0 中止传输。 */
typedef struct UpdateResponseBuffer {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} UpdateResponseBuffer;

/* 粘滞取消标志, 与 Windows 侧同义: 置位后只有下一次检查开始才会清零。 */
static bool cancelled(BongoCatUpdateService *service) {
    SDL_LockMutex(service->http_mutex);
    bool value = service->http_cancelled;
    SDL_UnlockMutex(service->http_mutex);
    return value;
}

/* CURLOPT_WRITEFUNCTION 回调: 由 libcurl 在工作线程内调用; 返回 count 表示已收下,
 * 返回 0 会让传输以 CURLE_WRITE_ERROR 中止 (取消之外的异常路径都走这里)。
 * size * count 先做溢出判断, 上限 1 MiB, 超过则置 failed 并中止。 */
static size_t write_response(const void *data, size_t size, size_t count,
    void *userdata) {
    UpdateResponseBuffer *buffer = userdata;
    if (!buffer || size == 0 || count > SIZE_MAX / size) return 0;
    size_t amount = size * count;
    if (amount > UPDATE_RESPONSE_LIMIT - buffer->length) {
        buffer->failed = true;
        return 0;
    }
    size_t required = buffer->length + amount + 1;
    if (required > buffer->capacity) {
        size_t next = buffer->capacity;
        while (next < required && next <= UPDATE_RESPONSE_LIMIT / 2)
            next *= 2;
        if (next < required) next = required;
        char *resized = realloc(buffer->data, next);
        if (!resized) {
            buffer->failed = true;
            return 0;
        }
        buffer->data = resized;
        buffer->capacity = next;
    }
    memcpy(buffer->data + buffer->length, data, amount);
    buffer->length += amount;
    buffer->data[buffer->length] = '\0';
    return count;
}

/* 取消唯一能生效的地方: 返回 1 让 libcurl 中止并给出 CURLE_ABORTED_BY_CALLBACK。
 * 不能像 Windows 那样从别的线程去清理句柄 —— libcurl 的 easy handle 不允许跨线程
 * 操作, 所以这里是协作式取消, 代价是取消有延迟 (见文件头)。 */
static int transfer_progress(void *userdata, curl_off_t download_total,
    curl_off_t download_now, curl_off_t upload_total, curl_off_t upload_now) {
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    return cancelled(userdata) ? 1 : 0;
}

/* 与 Windows 侧同一套英文短句, 便于两平台日志互相对照; 未列举的码统一兜底。 */
static const char *curl_error_name(CURLcode code) {
    switch (code) {
    case CURLE_OPERATION_TIMEDOUT: return "network timeout";
    case CURLE_COULDNT_RESOLVE_HOST: return "DNS lookup failed";
    case CURLE_COULDNT_CONNECT: return "network connection refused";
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION: return "TLS/SSL certificate error";
    case CURLE_ABORTED_BY_CALLBACK: return "request cancelled";
    default: return "network request failed";
    }
}

/* 前提: service 与其 http_mutex 有效 (本实现会检查 service/error, 与 Windows 侧略有差异)。
 * 连接超时 5s、总超时 15s; 显式打开 FOLLOWLOCATION 跟随重定向 (Windows 侧没设过重定向
 * 选项, 两侧策略并未对齐), 只有 CURLE_OK 时状态码才有意义, 所以先判传输错误再判状态码。
 * 404 给"没有已发布版本", 其余非 200 一律 RESPONSE 错误并把状态码拼进 error。
 * 所有提前返回都就地 free(buffer.data); 只有 OK 才把这块 malloc 内存交给调用方。 */
BongoCatUpdateFetchResult bongo_cat_update_http_fetch(
    BongoCatUpdateService *service, char **response, char *error,
    size_t error_capacity) {
    *response = NULL;
    if (!service || !error || !error_capacity)
        return BONGO_CAT_UPDATE_FETCH_MEMORY;
    /* 每次检查都调一次 curl_global_init; libcurl 允许重复调用, 这里不做引用计数,
       也始终不调 curl_global_cleanup —— 进程退出时由系统回收。 */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        snprintf(error, error_capacity, "Cannot initialize the network client");
        return BONGO_CAT_UPDATE_FETCH_MEMORY;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(error, error_capacity, "Cannot initialize the network client");
        return BONGO_CAT_UPDATE_FETCH_MEMORY;
    }
    /* 登记在飞的 CURL*: Unix 侧取消不会去动它 (libcurl 句柄不能跨线程清理),
       这个字段只是为了和 Windows 实现保持同样的"谁在持有网络资源"语义。 */
    SDL_LockMutex(service->http_mutex);
    bool accepted = !service->http_cancelled;
    if (accepted) service->http_request = curl;
    SDL_UnlockMutex(service->http_mutex);
    if (!accepted) {
        curl_easy_cleanup(curl);
        return BONGO_CAT_UPDATE_FETCH_CANCELLED;
    }

    UpdateResponseBuffer buffer = {
        .data = malloc(1), .length = 0, .capacity = 1, .failed = false
    };
    if (!buffer.data) {
        curl_easy_cleanup(curl);
        SDL_LockMutex(service->http_mutex);
        service->http_request = NULL;
        SDL_UnlockMutex(service->http_mutex);
        snprintf(error, error_capacity,
            "Cannot allocate the GitHub response buffer");
        return BONGO_CAT_UPDATE_FETCH_MEMORY;
    }
    buffer.data[0] = '\0';
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    curl_easy_setopt(curl, CURLOPT_URL,
        "https://api.github.com/repos/vladelaina/BongoCat/releases/latest");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BongoCat Update Checker/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, service);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    if (result == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    SDL_LockMutex(service->http_mutex);
    service->http_request = NULL;
    bool was_cancelled = service->http_cancelled;
    SDL_UnlockMutex(service->http_mutex);
    curl_easy_cleanup(curl);

    if (was_cancelled || result == CURLE_ABORTED_BY_CALLBACK) {
        free(buffer.data);
        return BONGO_CAT_UPDATE_FETCH_CANCELLED;
    }
    if (buffer.failed) {
        snprintf(error, error_capacity, "Cannot read the GitHub response");
        free(buffer.data);
        return BONGO_CAT_UPDATE_FETCH_MEMORY;
    }
    if (result != CURLE_OK) {
        snprintf(error, error_capacity, "Network error: %s (code %d)",
            curl_error_name(result), (int)result);
        free(buffer.data);
        return BONGO_CAT_UPDATE_FETCH_NETWORK;
    }
    if (status != 200) {
        snprintf(error, error_capacity,
            status == 404 ? "No published BongoCat release is available" :
            "GitHub returned HTTP status %ld", status);
        free(buffer.data);
        return BONGO_CAT_UPDATE_FETCH_RESPONSE;
    }
    *response = buffer.data;
    return BONGO_CAT_UPDATE_FETCH_OK;
}

/* 只置粘滞取消位, 不碰句柄 —— 句柄归工作线程所有, 由进度回调把它中止;
 * 因此调用方必须等 SDL_WaitThread 返回之后才能 free service。 */
void bongo_cat_update_http_cancel(BongoCatUpdateService *service) {
    if (!service || !service->http_mutex) return;
    SDL_LockMutex(service->http_mutex);
    service->http_cancelled = true;
    SDL_UnlockMutex(service->http_mutex);
}

#endif
