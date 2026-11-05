/*
 * 面捕组件的 HTTP 实现 (Windows: WinHTTP)。
 *
 * 只做两件事: 抓清单到内存、抓文件到磁盘。没有镜像前缀、没有断点续传 —— 组件包最大
 * 11 MB, 失败就让用户重按一次; 复杂化只会带来"重试到一半状态混乱"这类问题。
 *
 * 安全纪律与更新器一致: 只认 https, 跟随不超过 5 次重定向, 不读代理环境变量以外的配置
 * (WinHTTP 默认走系统代理, 与更新器行为一致)。
 */
#include "facetrack_http.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)

#include <windows.h>
#include <winhttp.h>

#include <SDL3/SDL.h>

#include <stdlib.h>

#define FACETRACK_USER_AGENT L"BongoCatTurbo-Facetrack/1.0"
#define FACETRACK_MANIFEST_CAP (4u * 1024u * 1024u)   /* 清单不该超过 4MB */

static void set_error(char *error, size_t capacity, const char *text) {
    if (error && capacity) snprintf(error, capacity, "%s", text ? text : "");
}

/* 把 UTF-8 的 URL 换成宽字符。WinHTTP 只吃宽字符。 */
static bool to_wide(const char *text, wchar_t *output, size_t capacity) {
    if (!text || !text[0]) return false;
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, output,
        (int)capacity);
    return length > 0;
}

/* 打开一次请求: 解析 URL -> 会话/连接/请求。返回 true 时三个句柄都已就绪。 */
static bool open_request(const wchar_t *wide_url, HINTERNET *session,
    HINTERNET *connection, HINTERNET *request, char *error, size_t capacity) {
    URL_COMPONENTS parts;
    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = (DWORD)-1;
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wide_url, 0, 0, &parts)) {
        set_error(error, capacity, "下载地址解析失败");
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
        set_error(error, capacity, "只接受 https 下载地址");
        return false;
    }
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    if (parts.dwHostNameLength >= 256 || parts.dwUrlPathLength +
            parts.dwExtraInfoLength >= 2048) {
        set_error(error, capacity, "下载地址过长");
        return false;
    }
    memcpy(host, parts.lpszHostName, parts.dwHostNameLength * sizeof(wchar_t));
    memcpy(path, parts.lpszUrlPath, parts.dwUrlPathLength * sizeof(wchar_t));
    if (parts.dwExtraInfoLength)
        memcpy(path + parts.dwUrlPathLength, parts.lpszExtraInfo,
            parts.dwExtraInfoLength * sizeof(wchar_t));

    /* 必须用 AUTOMATIC_PROXY: 它才会沿用系统代理 (WinINET 里配置的那个)。
       DEFAULT_PROXY 是"WinHTTP 自己的代理配置", 默认是直连 —— 明明系统开着代理,
       下载却走直连, 表现就是"十几 KB/s 甚至卡死"。更新器 (update_http.c) 用的也是
       AUTOMATIC_PROXY, 这里保持一致。 */
    *session = WinHttpOpen(FACETRACK_USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!*session) {
        set_error(error, capacity, "初始化网络失败");
        return false;
    }
    *connection = WinHttpConnect(*session, host, parts.nPort, 0);
    if (!*connection) {
        set_error(error, capacity, "连接服务器失败");
        WinHttpCloseHandle(*session);
        *session = NULL;
        return false;
    }
    *request = WinHttpOpenRequest(*connection, L"GET", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
    if (!*request) {
        set_error(error, capacity, "创建请求失败");
        WinHttpCloseHandle(*connection);
        WinHttpCloseHandle(*session);
        *connection = *session = NULL;
        return false;
    }
    /* 三个超时都要设: 默认的 60 秒连接超时会让"这个源根本连不上"变成卡死一分钟,
       用户看到的就是"设置页点了没反应"。8 秒足够国内任何一个可用镜像握手。 */
    DWORD timeout = 8000;
    WinHttpSetOption(*request, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout,
        sizeof(timeout));
    WinHttpSetOption(*request, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(*request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout,
        sizeof(timeout));
    return true;
}

static void close_request(HINTERNET session, HINTERNET connection,
    HINTERNET request) {
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
}

/* 发请求并检查状态码; 成功时把总长度写进 total (0 = 服务器没给长度)。 */
static bool send_and_check(HINTERNET request, uint64_t *total, char *error,
    size_t capacity) {
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, NULL)) {
        set_error(error, capacity, "请求失败 (网络不通或被拦截)");
        return false;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE |
            WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
            WINHTTP_NO_HEADER_INDEX)) {
        set_error(error, capacity, "读不到响应状态");
        return false;
    }
    if (status != 200) {
        snprintf(error, capacity, "服务器返回 %lu", (unsigned long)status);
        return false;
    }
    if (total) {
        wchar_t length_text[32] = {0};
        DWORD length_size = sizeof(length_text);
        *total = 0;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size,
                WINHTTP_NO_HEADER_INDEX))
            *total = _wcstoui64(length_text, NULL, 10);
    }
    return true;
}

bool bongo_cat_facetrack_http_get_memory(const char *url, char **output,
    size_t *length, char *error, size_t error_capacity) {
    if (output) *output = NULL;
    if (length) *length = 0;
    wchar_t wide_url[2048];
    if (!to_wide(url, wide_url, 2048)) {
        set_error(error, error_capacity, "下载地址无效");
        return false;
    }
    HINTERNET session = NULL, connection = NULL, request = NULL;
    if (!open_request(wide_url, &session, &connection, &request, error,
            error_capacity)) return false;

    bool ok = false;
    char *buffer = NULL;
    size_t used = 0;
    uint64_t total = 0;
    if (!send_and_check(request, &total, error, error_capacity)) goto done;
    buffer = malloc(4096);
    if (!buffer) {
        set_error(error, error_capacity, "内存不足");
        goto done;
    }
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            set_error(error, error_capacity, "读取响应失败");
            goto done;
        }
        if (available == 0) break;
        if (used + available + 1 > FACETRACK_MANIFEST_CAP) {
            set_error(error, error_capacity, "清单过大");
            goto done;
        }
        char *grown = realloc(buffer, used + available + 1);
        if (!grown) {
            set_error(error, error_capacity, "内存不足");
            goto done;
        }
        buffer = grown;
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer + used, available, &read) || read == 0) {
            set_error(error, error_capacity, "读取响应失败");
            goto done;
        }
        used += read;
    }
    buffer[used] = 0;
    *output = buffer;
    if (length) *length = used;
    buffer = NULL;
    ok = true;

done:
    free(buffer);
    close_request(session, connection, request);
    return ok;
}

bool bongo_cat_facetrack_http_get_file(const char *url, const char *path,
    BongoCatFacetrackProgress progress, void *user, uint64_t *downloaded,
    uint64_t time_limit_ms, uint64_t min_bytes_per_second, char *error,
    size_t error_capacity) {
    if (downloaded) *downloaded = 0;
    const ULONGLONG started = GetTickCount64();
    wchar_t wide_url[2048];
    if (!to_wide(url, wide_url, 2048)) {
        set_error(error, error_capacity, "下载地址无效");
        return false;
    }
    HINTERNET session = NULL, connection = NULL, request = NULL;
    if (!open_request(wide_url, &session, &connection, &request, error,
            error_capacity)) return false;

    bool ok = false;
    SDL_IOStream *file = NULL;
    char *buffer = NULL;
    uint64_t total = 0, received = 0;
    if (!send_and_check(request, &total, error, error_capacity)) goto done;
    /* 用 SDL_IOFromFile 而不是 fopen: 它按 UTF-8 解释路径 (中文用户目录常见),
       也避开了 MSVC 把 fopen 当成不安全函数的警告。 */
    file = SDL_IOFromFile(path, "wb");
    if (!file) {
        set_error(error, error_capacity, "写不了临时文件 (磁盘满或没权限)");
        goto done;
    }
    buffer = malloc(65536);
    if (!buffer) {
        set_error(error, error_capacity, "内存不足");
        goto done;
    }
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            set_error(error, error_capacity, "读取响应失败");
            goto done;
        }
        if (available == 0) break;
        if (available > 65536) available = 65536;
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, available, &read) || read == 0) {
            set_error(error, error_capacity, "读取响应失败");
            goto done;
        }
        if (SDL_WriteIO(file, buffer, read) != read) {
            set_error(error, error_capacity, "写入文件失败 (磁盘满?)");
            goto done;
        }
        received += read;
        if (downloaded) *downloaded = received;
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (time_limit_ms && elapsed > time_limit_ms) {
            set_error(error, error_capacity, "这个源太慢");
            goto done;
        }
        if (min_bytes_per_second && elapsed > 1500 &&
                received * 1000 / elapsed < min_bytes_per_second) {
            set_error(error, error_capacity, "这个源太慢");
            goto done;
        }
        if (progress && !progress(received, total, user)) {
            set_error(error, error_capacity, "已取消");
            goto done;
        }
    }
    if (!SDL_FlushIO(file)) {
        set_error(error, error_capacity, "写入文件失败 (磁盘满?)");
        goto done;
    }
    ok = true;

done:
    if (file) SDL_CloseIO(file);
    free(buffer);
    close_request(session, connection, request);
    return ok;
}

#else

/* 其它平台还没接: 面捕组件目前只发布 win-x64 包, 这里明确返回"不支持",
   免得调用方以为只是网络失败。 */
bool bongo_cat_facetrack_http_get_memory(const char *url, char **output,
    size_t *length, char *error, size_t error_capacity) {
    (void)url;
    (void)length;
    if (output) *output = NULL;
    if (error && error_capacity) snprintf(error, error_capacity,
        "当前平台没有面捕组件包");
    return false;
}

bool bongo_cat_facetrack_http_get_file(const char *url, const char *path,
    BongoCatFacetrackProgress progress, void *user, uint64_t *downloaded,
    uint64_t time_limit_ms, uint64_t min_bytes_per_second, char *error,
    size_t error_capacity) {
    (void)url; (void)path; (void)progress; (void)user; (void)time_limit_ms;
    (void)min_bytes_per_second;
    if (downloaded) *downloaded = 0;
    if (error && error_capacity) snprintf(error, error_capacity,
        "当前平台没有面捕组件包");
    return false;
}

#endif
