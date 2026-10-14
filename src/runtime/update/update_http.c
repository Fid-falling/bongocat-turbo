/*
 * 更新检查与更新下载的 Windows 网络实现 (WinHTTP): 拉取 GitHub latest release 的 JSON
 * 原文, 以及把发布资产 (单文件 exe) 下载到本地。
 * 只在更新工作线程里调用, 因此可以阻塞; 主线程只能靠 bongo_cat_update_http_cancel()
 * 从外面打断它, 其它线程碰它都是数据竞争。
 * 契约: 成功时 *response 是 malloc 出来、NUL 结尾的缓冲, 归调用方 free; 失败都返回
 * 非 OK 并往 error 写一句可直接显示的英文文案, 不静默失败。
 * 三个 HINTERNET 都在 http_mutex 下登记, 取消与清理只关闭属于自己的句柄。
 *
 * 下载与检查共用这三个句柄槽位: 检查阶段结束时它们都已经释放, 下载阶段重新登记, 两段
 * 永远不会同时在飞 (同一个工作线程顺序执行), 所以不需要第四、第五个槽位。
 */
#include "update_internal.h"
#include "bongo_cat/file.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"
#include "windows_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

#define UPDATE_RESPONSE_LIMIT (1024u * 1024u)
#define UPDATE_INITIAL_CAPACITY 8192u
/* 单个源的连接/收发超时: 15 秒连接是需求给的口径 —— 公共加速镜像挂掉时表现为连不上
   或不回包, 宁可早点失败换下一个源, 也不让用户对着"下载中 0%"发呆。 */
#define UPDATE_CONNECT_TIMEOUT_MS 15000
#define UPDATE_TRANSFER_TIMEOUT_MS 30000
/* 下载体积上限: 真实产物 30MB 上下, 512MB 足够宽容。这道闸门防的是"镜像被换成无限
   流"把用户磁盘写满 —— 校验要等下载完才做得了, 那时磁盘已经满了。 */
#define UPDATE_DOWNLOAD_LIMIT (512ull * 1024ull * 1024ull)

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

/* 一次解析出来的请求目标 (host / port / path+query / 是否 TLS)。
   update_http.c 里所有 URL 都先过这里, 于是"必须带主机名""必须是我们认识的协议"这类
   检查只写一遍。路径要把 query 拼回去: GitHub 的下载重定向和镜像前缀都可能把真正的
   地址放在查询串里, 漏掉它请求就会打到错误的位置。 */
typedef struct UpdateRequestTarget {
    wchar_t host[256];
    wchar_t path[2048];
    INTERNET_PORT port;
    bool secure;
} UpdateRequestTarget;

/* URL 的 UTF-8 -> UTF-16 转换。**不能**用 bongo_cat_windows_wide: 那个函数带路径语义,
   会把 '/' 全部换成 '\\'、给长字符串加 \\?\ 前缀 —— URL 被这么一改, WinHttpCrackUrl 直接
   解析失败 (表现为"地址不合法"), 而它本身正是给文件路径用的。返回值由调用方 free。 */
static wchar_t *wide_url(const char *url) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, NULL, 0);
    wchar_t *wide = length > 0 ? malloc((size_t)length * sizeof(*wide)) : NULL;
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, wide,
            length)) {
        free(wide);
        wide = NULL;
    }
    return wide;
}

static bool split_url(const char *url, UpdateRequestTarget *target) {
    if (!url || !url[0] || !target) return false;
    wchar_t *wide = wide_url(url);
    if (!wide) return false;
    URL_COMPONENTS parts = {0};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = (DWORD)-1;
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1;
    bool cracked = WinHttpCrackUrl(wide, 0, 0, &parts) != FALSE &&
        parts.dwHostNameLength > 0 && parts.dwHostNameLength <
            sizeof(target->host) / sizeof(target->host[0]) &&
        parts.dwUrlPathLength < sizeof(target->path) / sizeof(target->path[0]);
    if (cracked) {
        memcpy(target->host, parts.lpszHostName,
            parts.dwHostNameLength * sizeof(wchar_t));
        target->host[parts.dwHostNameLength] = L'\0';
        DWORD length = parts.dwUrlPathLength;
        memcpy(target->path, parts.lpszUrlPath, length * sizeof(wchar_t));
        /* 只有路径没有查询串时 WinHttpOpenRequest 也需要一个非空路径。 */
        if (!length) target->path[length++] = L'/';
        if (parts.dwExtraInfoLength) {
            DWORD extra = parts.dwExtraInfoLength;
            if (length + extra >= sizeof(target->path) / sizeof(target->path[0]))
                cracked = false;
            else {
                memcpy(target->path + length, parts.lpszExtraInfo,
                    extra * sizeof(wchar_t));
                length += extra;
            }
        }
        if (cracked) target->path[length] = L'\0';
        target->port = parts.nPort;
        target->secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        /* 只认 http/https: 别的协议 WinHttpConnect 会失败, 不如早点拒绝并给出文案。 */
        if (cracked && parts.nScheme != INTERNET_SCHEME_HTTPS &&
            parts.nScheme != INTERNET_SCHEME_HTTP) cracked = false;
    }
    free(wide);
    return cracked;
}

/*
 * 演练/开发用逃生口: BONGO_CAT_UPDATE_FEED_URL 指向本地 HTTP 服务时, 检查改用那个地址。
 * 目的是让"检查 -> 下载 -> 校验 -> 替换 -> 重启"整条链路可以在不碰真实发布的前提下演练
 * (本地服务器伪造一个更高版本号 + 当前 exe 的哈希)。它不改变任何信任边界: 期望哈希仍然
 * 只来自这份 JSON 的 digest/说明/校验资产, 下载完仍要本地重算比对。
 */
#define BONGO_CAT_UPDATE_FEED_URL_ENV "BONGO_CAT_UPDATE_FEED_URL"
#define BONGO_CAT_UPDATE_FEED_URL_DEFAULT \
    "https://api.github.com/repos/Fid-falling/bongocat-turbo/releases/latest"

static const char *releases_url(void) {
    const char *override = SDL_getenv(BONGO_CAT_UPDATE_FEED_URL_ENV);
    return override && override[0] && (strncmp(override, "https://", 8) == 0 ||
        strncmp(override, "http://", 7) == 0)
        ? override : BONGO_CAT_UPDATE_FEED_URL_DEFAULT;
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

/* 拉取 https://api.github.com/repos/Fid-falling/bongocat-turbo/releases/latest。
 * 前提: service 与其 http_mutex 有效, error 与 error_capacity 可用 (本实现不做空指针兜底)。
 * 超时 5s 解析 / 5s 连接 / 10s 发送 / 10s 接收: 目标是离线时尽早失败, 不让用户干等。
 * 只有 HTTP 200 算成功; 404 单独给"没有已发布版本"的文案, 其余状态码拼进 error ——
 * 那句 "GitHub returned HTTP status N" 会被 update_channel.c 解析后本地化, 别改措辞。
 * 请求头固定 Accept 与 API 版本 2022-11-28, 免得 GitHub 换默认版本后 JSON 字段漂移。 */
BongoCatUpdateFetchResult bongo_cat_update_http_fetch(
    BongoCatUpdateService *service, char **response, char *error,
    size_t error_capacity) {
    return bongo_cat_update_http_fetch_url(service, releases_url(), response,
        error, error_capacity);
}

/* 抓指定 URL 的响应体。目前唯一的调用者是"官方 .sha256 校验资产"—— 它必须走官方源,
   所以这里不做任何镜像前缀处理, URL 由调用方 (从 release 元数据里挑出来的) 给出。 */
BongoCatUpdateFetchResult bongo_cat_update_http_fetch_url(
    BongoCatUpdateService *service, const char *url, char **response,
    char *error, size_t error_capacity) {
    *response = NULL;
    HINTERNET session = WinHttpOpen(L"BongoCat Update Checker/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session || !track_handle(service, &service->http_session, session))
        return cancelled(service) ? BONGO_CAT_UPDATE_FETCH_CANCELLED :
            BONGO_CAT_UPDATE_FETCH_NETWORK;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    UpdateRequestTarget target;
    bool resolved = split_url(url, &target);
    HINTERNET connection = resolved ? WinHttpConnect(session, target.host,
        target.port, 0) : NULL;
    bool connected = connection && track_handle(service,
        &service->http_connection, connection);
    HINTERNET request = connected ? WinHttpOpenRequest(connection, L"GET",
        target.path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        target.secure ? WINHTTP_FLAG_SECURE : 0) : NULL;
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
    /* 地址本身不合法 (演练用的 BONGO_CAT_UPDATE_FEED_URL 写错) 与网络失败是两件事,
       分开报, 免得用户对着"网络错误"去查没有坏的网络。 */
    if (!resolved) {
        snprintf(error, error_capacity, "The update feed URL is not valid");
        return BONGO_CAT_UPDATE_FETCH_RESPONSE;
    }
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

/* 读一个响应头里的数值 (Content-Length)。查不到就返回 0, 由调用方把 total 记成"未知"。 */
static uint64_t response_length(HINTERNET request) {
    DWORD length = 0, size = sizeof(length);
    if (!WinHttpQueryHeaders(request,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &length, &size,
            WINHTTP_NO_HEADER_INDEX)) return 0;
    return length;
}

/*
 * 把 url 下载到 path。
 *
 * 三道必须在这里守住的线:
 * 1. **只写文件, 不做任何决策**: 期望哈希不经过这里, 镜像因此无法影响"什么算正确";
 * 2. 体积上限 (UPDATE_DOWNLOAD_LIMIT): 校验要等下载完才做, 不设上限的话一个无限流的
 *    镜像就能把用户磁盘写满;
 * 3. 任何失败路径都删掉半个文件: 留下不完整的 .new 只会让后续的替换路径用到坏数据。
 *
 * 重定向保持开启 (GitHub 的下载地址会跳到 objects.githubusercontent.com, 镜像也可能跳),
 * 跳到哪儿都不影响正确性 —— 内容仍然要过哈希。取消由主线程关闭句柄触发 (见 cancel)。
 */
BongoCatUpdateFetchResult bongo_cat_update_http_download(
    BongoCatUpdateService *service, const char *url, const char *path,
    char *error, size_t error_capacity) {
    UpdateRequestTarget target;
    if (!split_url(url, &target)) {
        snprintf(error, error_capacity, "The update download URL is not valid");
        return BONGO_CAT_UPDATE_FETCH_RESPONSE;
    }
    bongo_cat_update_report_progress(service, 0, 0);
    HINTERNET session = WinHttpOpen(L"BongoCat Update Downloader/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session || !track_handle(service, &service->http_session, session))
        return cancelled(service) ? BONGO_CAT_UPDATE_FETCH_CANCELLED :
            BONGO_CAT_UPDATE_FETCH_NETWORK;
    WinHttpSetTimeouts(session, UPDATE_CONNECT_TIMEOUT_MS,
        UPDATE_CONNECT_TIMEOUT_MS, UPDATE_TRANSFER_TIMEOUT_MS,
        UPDATE_TRANSFER_TIMEOUT_MS);
    /* 显式声明跟随重定向: 默认策略随系统/版本而变, 而 GitHub 的资产下载必然重定向。
       ALWAYS 允许跨主机跳转 —— 跳到哪儿都不影响正确性, 内容仍然要过 SHA-256。 */
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &policy,
        sizeof(policy));
    HINTERNET connection = WinHttpConnect(session, target.host, target.port, 0);
    bool connected = connection && track_handle(service,
        &service->http_connection, connection);
    HINTERNET request = connected ? WinHttpOpenRequest(connection, L"GET",
        target.path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        target.secure ? WINHTTP_FLAG_SECURE : 0) : NULL;
    bool requested = request && track_handle(service,
        &service->http_request, request);
    bool sent = requested && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS,
        0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL);
    DWORD request_error = sent ? ERROR_SUCCESS : GetLastError();
    DWORD status = 0;
    bool valid_status = sent && response_status(request, &status) && status == 200;
    uint64_t total = valid_status ? response_length(request) : 0;
    BongoCatUpdateFetchResult result = BONGO_CAT_UPDATE_FETCH_OK;
    FILE *file = NULL;
    if (valid_status && total > UPDATE_DOWNLOAD_LIMIT) {
        snprintf(error, error_capacity,
            "The update package is larger than the supported limit");
        result = BONGO_CAT_UPDATE_FETCH_RESPONSE;
    } else if (valid_status) {
        file = bongo_cat_file_open(path, "wb");
        if (!file) {
            snprintf(error, error_capacity,
                "Cannot write the downloaded update file");
            result = BONGO_CAT_UPDATE_FETCH_FILE;
        }
    }
    uint64_t received = 0;
    if (file) bongo_cat_update_report_progress(service, 0, total);
    while (file) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            DWORD code = GetLastError();
            snprintf(error, error_capacity, "Network error: %s (code %lu)",
                winhttp_error_name(code), (unsigned long)code);
            result = BONGO_CAT_UPDATE_FETCH_NETWORK;
            break;
        }
        if (!available) break; /* 服务端收完了 */
        if (available > UPDATE_DOWNLOAD_LIMIT - received) {
            snprintf(error, error_capacity,
                "The update package is larger than the supported limit");
            result = BONGO_CAT_UPDATE_FETCH_RESPONSE;
            break;
        }
        char buffer[16 * 1024];
        DWORD chunk = available < sizeof(buffer) ? available : (DWORD)sizeof(buffer);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, chunk, &read) || !read) {
            DWORD code = GetLastError();
            snprintf(error, error_capacity, "Network error: %s (code %lu)",
                winhttp_error_name(code), (unsigned long)code);
            result = BONGO_CAT_UPDATE_FETCH_NETWORK;
            break;
        }
        if (fwrite(buffer, 1, read, file) != read) {
            snprintf(error, error_capacity,
                "Cannot write the downloaded update file");
            result = BONGO_CAT_UPDATE_FETCH_FILE;
            break;
        }
        received += read;
        bongo_cat_update_report_progress(service, received, total);
    }
    if (file && fclose(file) != 0 && result == BONGO_CAT_UPDATE_FETCH_OK) {
        snprintf(error, error_capacity,
            "Cannot write the downloaded update file");
        result = BONGO_CAT_UPDATE_FETCH_FILE;
    }
    if (request) release_handle(service, &service->http_request, request);
    if (connection) release_handle(service, &service->http_connection, connection);
    release_handle(service, &service->http_session, session);
    /* 取消的判定放在最前: 主线程取消时上面每一步都可能以"网络错误"退出, 但那不是错误,
       是用户关了程序。 */
    if (cancelled(service)) result = BONGO_CAT_UPDATE_FETCH_CANCELLED;
    if (result == BONGO_CAT_UPDATE_FETCH_OK && !received) {
        snprintf(error, error_capacity, "The update download is empty");
        result = BONGO_CAT_UPDATE_FETCH_RESPONSE;
    } else if (result == BONGO_CAT_UPDATE_FETCH_OK && total && received != total) {
        /* 服务端声明了长度却没给够: 当作传输中断, 换下一个源重试。 */
        snprintf(error, error_capacity,
            "Network error: the update download was interrupted");
        result = BONGO_CAT_UPDATE_FETCH_NETWORK;
    }
    if (result != BONGO_CAT_UPDATE_FETCH_OK) bongo_cat_path_remove(path);
    if (result == BONGO_CAT_UPDATE_FETCH_OK ||
        result == BONGO_CAT_UPDATE_FETCH_CANCELLED ||
        result == BONGO_CAT_UPDATE_FETCH_FILE) return result;
    if (!sent) {
        if (!error[0]) snprintf(error, error_capacity,
            "Network error: %s (code %lu)", winhttp_error_name(request_error),
            (unsigned long)request_error);
        return BONGO_CAT_UPDATE_FETCH_NETWORK;
    }
    if (!valid_status) {
        if (!error[0]) snprintf(error, error_capacity,
            status == 404 ? "The update source does not have this file" :
            "The update source returned HTTP status %lu", (unsigned long)status);
        return BONGO_CAT_UPDATE_FETCH_RESPONSE;
    }
    return result;
}

#endif
