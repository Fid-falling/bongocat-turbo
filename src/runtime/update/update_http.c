/*
 * 更新检查的 Windows 网络实现 (WinHTTP): 拉取 GitHub latest release 的 JSON 原文。
 * 只在更新工作线程里调用, 因此可以阻塞; 主线程只能靠 bongo_cat_update_http_cancel()
 * 从外面打断它, 其它线程碰它都是数据竞争。
 * 契约: 成功时 *response 是 malloc 出来、NUL 结尾的缓冲, 归调用方 free; 失败都返回
 * 非 OK 并往 error 写一句可直接显示的英文文案, 不静默失败。
 * 三个 HINTERNET 都在 http_mutex 下登记, 取消与清理只关闭属于自己的句柄。
 */
#include "update_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

#define UPDATE_RESPONSE_LIMIT (1024u * 1024u)
#define UPDATE_INITIAL_CAPACITY 8192u

/* 把新句柄登记到 service 的槽位; 返回 false 表示登记时已被取消, 此时本函数已经
 * 关掉了句柄, 调用方不能再关第二次。登记与取消必须在同一把锁下做, 否则会出现
 * "登记完才发现已取消"的窗口, 句柄就泄漏了。 */
static bool track_handle(BongoCatUpdateService *service, void **slot,
    HINTERNET handle) {
    SDL_LockMutex(service->http_mutex);
    bool accepted = !service->http_cancelled;
    if (accepted) *slot = handle;
    SDL_UnlockMutex(service->http_mutex);
    if (!accepted) WinHttpCloseHandle(handle);
    return accepted;
}

/* 只关闭仍属于自己的句柄: 槽位已被取消路径摘空 (或换成了别人) 就说明所有权不在
 * 自己手上, 绝不能再 Close —— 这是防止双重关闭的关键。 */
static void release_handle(BongoCatUpdateService *service, void **slot,
    HINTERNET handle) {
    SDL_LockMutex(service->http_mutex);
    bool owned = *slot == handle;
    if (owned) *slot = NULL;
    SDL_UnlockMutex(service->http_mutex);
    if (owned) WinHttpCloseHandle(handle);
}

/* 取消是一旦置位就不再复位的粘滞标志, 只有下一次 bongo_cat_update_check() 才清零。 */
static bool cancelled(BongoCatUpdateService *service) {
    SDL_LockMutex(service->http_mutex);
    bool value = service->http_cancelled;
    SDL_UnlockMutex(service->http_mutex);
    return value;
}

/* 只查状态码; 返回 false 表示查询本身失败 (连接已断), 要和"查到了但不是 200"
 * 分开处理: 前者算网络错误, 后者算响应错误。 */
static bool response_status(HINTERNET request, DWORD *status) {
    DWORD size = sizeof(*status);
    return WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, status, &size,
        WINHTTP_NO_HEADER_INDEX) != FALSE;
}

/* 把 WinHTTP 错误码翻成用户看得懂的英文短句; 未列举的码统一兜底, 具体码仍会拼进 error。 */
static const char *winhttp_error_name(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return "network timeout";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "DNS lookup failed";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "network connection refused";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "network connection error";
    case ERROR_WINHTTP_SECURE_FAILURE: return "TLS/SSL certificate error";
    default: return "network request failed";
    }
}

/* 边收边扩地读完整响应体, 返回 malloc 缓冲 (归调用方 free) 或 NULL, 保证 NUL 结尾。
 * 上限 1 MiB: 超过就放弃, 免得被异常巨大的响应拖死或吃满内存 (宁可报错也不无限增长);
 * 分配失败同样返回 NULL, 由调用方统一报 BONGO_CAT_UPDATE_FETCH_MEMORY。
 * available == 0 表示服务端收完了; 查询失败或读出 0 字节都当作中断退出。 */
static char *read_response(BongoCatUpdateService *service,
    HINTERNET request) {
    size_t capacity = UPDATE_INITIAL_CAPACITY, length = 0;
    char *buffer = malloc(capacity);
    if (!buffer) return NULL;
    while (!cancelled(service)) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) break;
        if (!available) {
            buffer[length] = '\0';
            return buffer;
        }
        if (available > UPDATE_RESPONSE_LIMIT - length) break;
        size_t required = length + available + 1;
        if (required > capacity) {
            size_t next = capacity;
            while (next < required && next <= UPDATE_RESPONSE_LIMIT / 2)
                next *= 2;
            if (next < required) next = required;
            char *resized = next <= UPDATE_RESPONSE_LIMIT + 1
                ? realloc(buffer, next) : NULL;
            if (!resized) break;
            buffer = resized;
            capacity = next;
        }
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer + length, available, &read) ||
            !read) break;
        length += read;
    }
    free(buffer);
    return NULL;
}

/* 拉取 https://api.github.com/repos/Fid-falling/bongocat-vulkan/releases/latest。
 * 前提: service 与其 http_mutex 有效, error 与 error_capacity 可用 (本实现不做空指针兜底)。
 * 超时 5s 解析 / 5s 连接 / 10s 发送 / 10s 接收: 目标是离线时尽早失败, 不让用户干等。
 * 只有 HTTP 200 算成功; 404 单独给"没有已发布版本"的文案, 其余状态码拼进 error ——
 * 那句 "GitHub returned HTTP status N" 会被 update_channel.c 解析后本地化, 别改措辞。
 * 请求头固定 Accept 与 API 版本 2022-11-28, 免得 GitHub 换默认版本后 JSON 字段漂移。 */
BongoCatUpdateFetchResult bongo_cat_update_http_fetch(
    BongoCatUpdateService *service, char **response, char *error,
    size_t error_capacity) {
    *response = NULL;
    HINTERNET session = WinHttpOpen(L"BongoCat Update Checker/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session || !track_handle(service, &service->http_session, session))
        return cancelled(service) ? BONGO_CAT_UPDATE_FETCH_CANCELLED :
            BONGO_CAT_UPDATE_FETCH_NETWORK;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    HINTERNET connection = WinHttpConnect(session, L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    bool connected = connection && track_handle(service,
        &service->http_connection, connection);
    HINTERNET request = connected ? WinHttpOpenRequest(connection, L"GET",
        L"/repos/Fid-falling/bongocat-vulkan/releases/latest", NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE) : NULL;
    bool requested = request && track_handle(service,
        &service->http_request, request);
    static const wchar_t headers[] =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    bool sent = requested && WinHttpSendRequest(request, headers,
        (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL);
    DWORD request_error = sent ? ERROR_SUCCESS : GetLastError();
    DWORD status = 0;
    bool valid_status = sent && response_status(request, &status) &&
        status == 200;
    if (valid_status) *response = read_response(service, request);
    if (request) release_handle(service, &service->http_request, request);
    if (connection) release_handle(service, &service->http_connection,
        connection);
    release_handle(service, &service->http_session, session);
    if (cancelled(service)) return BONGO_CAT_UPDATE_FETCH_CANCELLED;
    if (!sent) {
        snprintf(error, error_capacity, "Network error: %s (code %lu)",
            winhttp_error_name(request_error), (unsigned long)request_error);
        return BONGO_CAT_UPDATE_FETCH_NETWORK;
    }
    if (!valid_status) {
        snprintf(error, error_capacity,
            status == 404 ? "No published BongoCat release is available" :
            "GitHub returned HTTP status %lu", (unsigned long)status);
        return BONGO_CAT_UPDATE_FETCH_RESPONSE;
    }
    if (!*response) {
        snprintf(error, error_capacity, "Cannot read the GitHub response");
        return BONGO_CAT_UPDATE_FETCH_MEMORY;
    }
    return BONGO_CAT_UPDATE_FETCH_OK;
}

/* 任意线程可调 (实际是主线程在关闭/退出时调): 先在锁内摘走三个句柄并置取消位, 再到锁外
 * 关闭 —— 关掉正在阻塞的 WinHttpReceiveResponse 是打断它的唯一手段。
 * 与 release_handle 的"槽位相等才关"配合, 每个句柄只被关一次; 工作线程随后返回 CANCELLED。 */
void bongo_cat_update_http_cancel(BongoCatUpdateService *service) {
    if (!service || !service->http_mutex) return;
    SDL_LockMutex(service->http_mutex);
    service->http_cancelled = true;
    HINTERNET request = service->http_request;
    HINTERNET connection = service->http_connection;
    HINTERNET session = service->http_session;
    service->http_request = service->http_connection =
        service->http_session = NULL;
    SDL_UnlockMutex(service->http_mutex);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
}

#endif
