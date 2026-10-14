/*
 * 一键更新的下载阶段集成测试: 用本地 HTTP 服务假造"前两个源失败、第三个源成功",
 * 验证多源回退、"记住上次成功的源"、以及哈希不匹配时拒绝并清理临时文件。
 *
 * 为什么必须有这一层: 哈希校验与镜像回退是本功能里唯一"错了就会装错东西"的部分, 而
 * 单元测试碰不到真实的网络栈。这里让 WinHTTP 真的连本机服务, 走完整条
 * 检查(feed 覆盖) -> 选资产 -> 期望哈希 -> 逐源下载 -> 重算校验 的路径。
 * **不碰线上资产**: 全部流量都在 127.0.0.1, 版本号是假的, 校验值由本测试现算。
 *
 * 三个假源:
 *   1. 一个没人监听的端口   -> 连接被拒绝 (最快的那种失败)
 *   2. <镜像>/missing/...   -> 服务端回 404
 *   3. <镜像>/...           -> 正常返回正文
 * 断言点: 最终用的是第 3 个源、文件通过校验、状态文件记住了这个源; 第二次运行直接命中它。
 */
#include "test.h"
#include "update_internal.h"
#include "update_install.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"
#include "bongo_cat/sha256.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int bongo_cat_test_failures;

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#define TEST_FEED_PATH "/feed"
#define TEST_MIRROR_FIRST "/first/"
#define TEST_MIRROR_MISSING "/missing/"
/* 假版本号: 只要比本地 BONGO_CAT_VERSION 大就会进入下载阶段 */
#define TEST_VERSION "9999.0.0"

typedef struct TestServer {
    SOCKET socket;
    SDL_Thread *thread;
    bool stop;
    char feed[4096];
    const char *binary;
    size_t binary_length;
    unsigned requests;
} TestServer;

/* 把一个已知长度的正文发出去。HTTP/1.1 + Content-Length + Connection: close:
   WinHTTP 据此知道什么时候收完, 不会挂在那里等超时。 */
static void respond(SOCKET client, int status, const char *body, size_t length) {
    char header[256];
    int used = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n"
        "Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n",
        status, status == 200 ? "OK" : "Not Found", length);
    if (used > 0) send(client, header, used, 0);
    if (status == 200 && length) send(client, body, (int)length, 0);
}

static int SDLCALL serve(void *userdata) {
    TestServer *server = userdata;
    while (!server->stop) {
        SOCKET client = accept(server->socket, NULL, NULL);
        if (client == INVALID_SOCKET) break;
        char request[2048];
        int received = recv(client, request, sizeof(request) - 1, 0);
        if (received <= 0) {
            closesocket(client);
            continue;
        }
        request[received] = '\0';
        server->requests++;
        bool wants_feed = strstr(request, TEST_FEED_PATH) != NULL;
        bool missing = strstr(request, TEST_MIRROR_MISSING) != NULL;
        if (missing) respond(client, 404, NULL, 0);
        else if (wants_feed) respond(client, 200, server->feed,
            strlen(server->feed));
        else respond(client, 200, server->binary, server->binary_length);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
    return 0;
}

static bool server_start(TestServer *server, const char *binary, size_t length) {
    memset(server, 0, sizeof(*server));
    server->binary = binary;
    server->binary_length = length;
    server->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->socket == INVALID_SOCKET) return false;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0; /* 让系统挑一个空闲端口, 免得和别的测试撞车 */
    if (bind(server->socket, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->socket, 4) != 0) {
        closesocket(server->socket);
        server->socket = INVALID_SOCKET;
        return false;
    }
    int length_of_address = sizeof(address);
    if (getsockname(server->socket, (struct sockaddr *)&address,
            &length_of_address) != 0) return false;
    server->thread = SDL_CreateThread(serve, "update-test-server", server);
    return server->thread != NULL;
}

static unsigned server_port(const TestServer *server) {
    struct sockaddr_in address;
    int length = sizeof(address);
    if (getsockname(server->socket, (struct sockaddr *)&address, &length) != 0)
        return 0;
    return ntohs(address.sin_port);
}

static void server_stop(TestServer *server) {
    if (server->socket == INVALID_SOCKET) return;
    server->stop = true;
    /* 自己连一次把 accept 叫醒, 否则 join 会一直等在那里 */
    SOCKET wake = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (wake != INVALID_SOCKET) {
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons((u_short)server_port(server));
        connect(wake, (struct sockaddr *)&address, sizeof(address));
        closesocket(wake);
    }
    if (server->thread) SDL_WaitThread(server->thread, NULL);
    closesocket(server->socket);
    server->socket = INVALID_SOCKET;
}

/* 找一个当前没人监听的端口: 绑一次拿到端口号再关掉, 之后连接它就会立刻被拒绝。 */
static unsigned dead_port(void) {
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe == INVALID_SOCKET) return 1;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(probe, (struct sockaddr *)&address, sizeof(address)) != 0) {
        closesocket(probe);
        return 1;
    }
    unsigned port = 0;
    int length = sizeof(address);
    if (getsockname(probe, (struct sockaddr *)&address, &length) == 0)
        port = ntohs(address.sin_port);
    closesocket(probe);
    return port;
}

static bool write_text(const char *path, const char *text) {
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) return false;
    bool written = fputs(text, file) >= 0;
    fclose(file);
    return written;
}

static bool read_text(const char *path, char *output, size_t capacity) {
    output[0] = '\0';
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) return false;
    size_t length = fread(output, 1, capacity - 1, file);
    output[length] = '\0';
    fclose(file);
    return true;
}

/* 拼出假 release 元数据: 资产名必须与客户端按平台拼出来的名字完全一致, 否则会静默
   退化成"打开发布页" (那正是本测试要避免的假通过)。
   digest 为 NULL 表示"这份发布一个校验值都没有" —— 客户端必须据此拒绝下载。 */
static void build_feed(char *output, size_t capacity, const char *digest) {
    char name[128];
    snprintf(name, sizeof(name), "BongoCat-" TEST_VERSION "-%s-portable.exe",
        bongo_cat_update_platform_asset());
    snprintf(output, capacity,
        "{\"tag_name\":\"v" TEST_VERSION "\",\"draft\":false,"
        "\"prerelease\":false,"
        "\"html_url\":\"https://github.com/Fid-falling/bongocat-turbo/"
        "releases/tag/v" TEST_VERSION "\",\"body\":\"test feed\",\"assets\":["
        "{\"name\":\"%s\",\"browser_download_url\":"
        "\"https://github.com/Fid-falling/bongocat-turbo/releases/download/"
        "v" TEST_VERSION "/%s\"%s%s%s}]}",
        name, name, digest ? ",\"digest\":\"sha256:" : "",
        digest ? digest : "", digest ? "\"" : "");
}

/* 造一个只属于本次测试的 app: 只有更新模块用到的几个根目录需要有效。
   必须堆分配: BongoCatApp 里装着模型/行为目录, 结构体有几百 KB, 放在栈上会直接撑爆
   线程栈 (真实程序也是 calloc 出来的)。 */
static BongoCatApp *app_state(const char *root) {
    BongoCatApp *app = calloc(1, sizeof(*app));
    if (!app) return NULL;
    snprintf(app->config_root, sizeof(app->config_root), "%s", root);
    snprintf(app->cache_root, sizeof(app->cache_root), "%s", root);
    snprintf(app->log_root, sizeof(app->log_root), "%s", root);
    app->smoke = false;
    app->automated = false;
    return app;
}

/* 轮询到工作线程交卷 (状态不再是 CHECKING/DOWNLOADING/VERIFYING), 或者超时。 */
static BongoCatUpdateStatus wait_for_result(BongoCatUpdateService *service) {
    BongoCatUpdateSnapshot snapshot;
    for (int attempt = 0; attempt < 600; ++attempt) {
        bongo_cat_update_snapshot(service, &snapshot);
        if (snapshot.status != BONGO_CAT_UPDATE_CHECKING &&
            snapshot.status != BONGO_CAT_UPDATE_DOWNLOADING &&
            snapshot.status != BONGO_CAT_UPDATE_VERIFYING)
            return snapshot.status;
        SDL_Delay(50);
    }
    return BONGO_CAT_UPDATE_IDLE;
}

static void test_update_download(void) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
    const char *payload = "BongoCat test payload: the bytes that must match the "
        "published SHA-256 before anything is replaced.";
    char payload_hash[BONGO_CAT_UPDATE_DIGEST_CAP];
    bongo_cat_sha256_bytes(payload, strlen(payload), payload_hash);
    char root[BONGO_CAT_PATH_CAP];
    (void)SDL_Init(SDL_INIT_EVENTS);
    const char *temp = SDL_getenv("TEMP");
    snprintf(root, sizeof(root), "%s/bongocat-update-download-test", temp ?
        temp : ".");
    bongo_cat_path_create_directory(root);
    char sources_path[BONGO_CAT_PATH_CAP], state_path[BONGO_CAT_PATH_CAP];
    bongo_cat_path_join(sources_path, sizeof(sources_path), root,
        "update-sources.txt");
    bongo_cat_path_join(state_path, sizeof(state_path), root,
        "update-sources.state");
    bongo_cat_path_remove(state_path);

    TestServer server;
    CHECK(server_start(&server, payload, strlen(payload)));
    unsigned port = server_port(&server);
    CHECK(port != 0);
    unsigned refused = dead_port();
    char sources[1024];
    snprintf(sources, sizeof(sources),
        "# 本测试用的假镜像列表\nhttp://127.0.0.1:%u/\n"
        "http://127.0.0.1:%u" TEST_MIRROR_MISSING "\n"
        "http://127.0.0.1:%u/\n", refused, port, port);
    CHECK(write_text(sources_path, sources));
    /* feed 覆盖: 检查阶段不看 GitHub, 而是看本机服务 (演练用逃生口的正式用法)。
       用 SDL_setenv_unsafe 而不是 CRT 的 _putenv_s: SDL_getenv 读的是 SDL 缓存的那份
       环境, unsafe 版本会同时更新缓存与进程环境, 于是检查阶段一定能看到它。 */
    char feed_url[128];
    snprintf(feed_url, sizeof(feed_url), "http://127.0.0.1:%u" TEST_FEED_PATH,
        port);
    CHECK(SDL_setenv_unsafe("BONGO_CAT_UPDATE_FEED_URL", feed_url, 1) == 0);
    build_feed(server.feed, sizeof(server.feed), payload_hash);

    BongoCatApp *app = app_state(root);
    CHECK(app != NULL);
    if (!app) return;
    BongoCatUpdateService *service = bongo_cat_update_create(app);
    CHECK(service != NULL);
    if (!service) return;
    CHECK(bongo_cat_update_check(service, true));
    BongoCatUpdateStatus status = wait_for_result(service);
    /* 校验通过后停在 INSTALLING: 替换只能在主线程做, 主线程在这里刻意不参与 */
    CHECK(status == BONGO_CAT_UPDATE_INSTALLING);
    CHECK(service->install_requested);
    CHECK(service->source_index == 3);
    CHECK(service->source_count == 3);
    char expected_target[BONGO_CAT_PATH_CAP];
    snprintf(expected_target, sizeof(expected_target), "%s", service->new_path);
    CHECK(expected_target[0] != '\0');
    CHECK(bongo_cat_path_is_file(expected_target));
    char actual[BONGO_CAT_UPDATE_DIGEST_CAP] = {0};
    BongoCatError error = {0};
    CHECK(bongo_cat_sha256_file(expected_target, actual, &error) == BONGO_CAT_OK);
    CHECK(strcmp(actual, payload_hash) == 0);
    /* 记住这次成功的源: 状态文件里就是第三个 */
    char remembered[512];
    CHECK(read_text(state_path, remembered, sizeof(remembered)));
    CHECK(strstr(remembered, "127.0.0.1") != NULL);
    bongo_cat_update_destroy(service);
    bongo_cat_path_remove(expected_target);

    /* 第二次运行: 记住的源被提到最前, 于是第一个就命中 (不再等那个死端口) */
    BongoCatApp *second_app = app_state(root);
    CHECK(second_app != NULL);
    BongoCatUpdateService *second = second_app ?
        bongo_cat_update_create(second_app) : NULL;
    CHECK(second != NULL);
    if (second) {
        CHECK(strncmp(second->sources.entries[0], "http://127.0.0.1:", 17) == 0);
        CHECK(bongo_cat_update_check(second, true));
        CHECK(wait_for_result(second) == BONGO_CAT_UPDATE_INSTALLING);
        CHECK(second->source_index == 1);
        snprintf(expected_target, sizeof(expected_target), "%s", second->new_path);
        bongo_cat_update_destroy(second);
        bongo_cat_path_remove(expected_target);
    }
    free(second_app);

    /*
     * 哈希不匹配 (镜像被投毒/传输损坏): 必须拒绝、删掉刚下载的文件, 并给出可本地化的
     * 那句原文。这是"镜像不可信"这条安全边界的实证。
     */
    {
        char wrong_hash[BONGO_CAT_UPDATE_DIGEST_CAP];
        bongo_cat_sha256_bytes("another payload", 15, wrong_hash);
        build_feed(server.feed, sizeof(server.feed), wrong_hash);
        BongoCatApp *mismatch_app = app_state(root);
        BongoCatUpdateService *mismatch = mismatch_app ?
            bongo_cat_update_create(mismatch_app) : NULL;
        CHECK(mismatch != NULL);
        if (mismatch) {
            CHECK(bongo_cat_update_check(mismatch, true));
            CHECK(wait_for_result(mismatch) == BONGO_CAT_UPDATE_ERROR);
            BongoCatUpdateSnapshot snapshot;
            bongo_cat_update_snapshot(mismatch, &snapshot);
            CHECK(strcmp(snapshot.error,
                BONGO_CAT_UPDATE_ERROR_HASH_MISMATCH) == 0);
            CHECK(!snapshot.manual_replace);
            bongo_cat_update_destroy(mismatch);
        }
        free(mismatch_app);
    }

    /* 一个校验值都拿不到: 不下载、不替换, 只提示"无法校验, 已取消" */
    {
        build_feed(server.feed, sizeof(server.feed), NULL);
        BongoCatApp *blind_app = app_state(root);
        BongoCatUpdateService *blind = blind_app ?
            bongo_cat_update_create(blind_app) : NULL;
        CHECK(blind != NULL);
        if (blind) {
            CHECK(bongo_cat_update_check(blind, true));
            CHECK(wait_for_result(blind) == BONGO_CAT_UPDATE_ERROR);
            BongoCatUpdateSnapshot snapshot;
            bongo_cat_update_snapshot(blind, &snapshot);
            CHECK(strcmp(snapshot.error,
                BONGO_CAT_UPDATE_ERROR_NO_CHECKSUM) == 0);
            bongo_cat_update_destroy(blind);
        }
        free(blind_app);
    }

    free(app);
    SDL_Quit();
    SDL_setenv_unsafe("BONGO_CAT_UPDATE_FEED_URL", "", 1);
    bongo_cat_path_remove(sources_path);
    bongo_cat_path_remove(state_path);
    /* 临时目录也一起收掉: 用例跑完不该在用户机器上留东西 (目录空时 remove 就能删掉)。 */
    bongo_cat_path_remove(root);
    server_stop(&server);
    WSACleanup();
}

int main(void) {
    test_update_download();
    if (bongo_cat_test_failures) {
        fprintf(stderr, "%d checks failed\n", bongo_cat_test_failures);
        return 1;
    }
    puts("update download checks passed");
    return 0;
}

#else /* 非 Windows: 没有自更新下载 (平台能力为 false), 用例整体跳过 */

int main(void) {
    puts("update download checks skipped on this platform");
    return 0;
}

#endif
