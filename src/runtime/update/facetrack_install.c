/*
 * 面捕组件的按需下载。
 *
 * 线程模型: 设置页点"下载" -> start() 起工作线程 -> 主线程每帧读 snapshot 画进度。
 * 只有工作线程改状态, 主线程只读 —— 和更新器同一套路 (BongoCatUpdateService 的注释里
 * 有更长的解释)。
 *
 * 落盘纪律:
 *   1. 先写 <path>.part, 校验通过才改名成正式文件; 半截文件永远不会被组件装载读到。
 *   2. 每个文件按清单里的 SHA256 校验 (bongo_cat_update_verify_download 校验失败会删除)。
 *   3. 清单本身也校验 (没有签名, 但至少保证"下载到的不是半截 JSON")。
 */
#include "facetrack_install.h"

#include "facetrack_http.h"

#include "bongo_cat/log.h"
#include "bongo_cat/path.h"
#include "bongo_cat/update.h"
#include "bongo_cat/update_source.h"

#include <SDL3/SDL.h>

#include <yyjson.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACETRACK_MAX_FILES 8

typedef struct ManifestFile {
    char name[64];
    char path[160];
    char sha256[BONGO_CAT_UPDATE_DIGEST_CAP];
    uint64_t size;
} ManifestFile;

/* 还在跑的安装器线程数: 销毁是 detach 的, 需要它来判断"上一轮真的结束了"。 */
static atomic_int g_install_threads = 0;

int bongo_cat_facetrack_install_active_threads(void) {
    return atomic_load(&g_install_threads);
}

struct BongoCatFacetrackInstall {
    char target_root[BONGO_CAT_PATH_CAP];
    SDL_Thread *thread;
    SDL_Mutex *lock;
    atomic_bool running;
    atomic_bool stop;
    /* 销毁时置位: 线程退出后自己释放结构 (网络请求可能卡很久, 不能 join) */
    atomic_bool orphan;
    BongoCatFacetrackInstallSnapshot snapshot;
    /* 工作线程私有 (不跨线程读): 清单解析结果 (解析器在 facetrack_manifest.c) */
    BongoCatFacetrackManifest manifest;
    /* 速度估算的滑动窗口 (只有工作线程写, 读在锁内) */
    Uint64 speed_mark_ticks;
    uint64_t speed_mark_bytes;
    /* 下载源 (与主程序更新器共用同一份默认列表: direct + 公共加速前缀 + 自建镜像)。
       直连 GitHub 在国内经常只有十几 KB/s, 11 MB 的 ONNX Runtime 要跑六七分钟, 所以
       必须按列表顺序试; 成功的源会被记下来给后面的文件优先用。 */
    BongoCatUpdateSourceList sources;
    size_t preferred_source;
    bool source_raced;
    /* true = 只查版本, 不下载文件 */
    bool check_only;
};

static void lock_set_status(BongoCatFacetrackInstall *install,
    BongoCatFacetrackInstallState state, const char *error) {
    SDL_LockMutex(install->lock);
    install->snapshot.state = state;
    if (error) snprintf(install->snapshot.error, sizeof(install->snapshot.error),
        "%s", error);
    else install->snapshot.error[0] = 0;
    SDL_UnlockMutex(install->lock);
}


/* 按源列表顺序尝试抓一个东西。fetch(url, user) 返回 true 即停止。
   origin 是官方地址 (direct 时原样使用), 其余条目按 update_source_url 拼。 */
typedef bool (*FacetrackFetch)(const char *url, void *user);

static bool fetch_from_sources(BongoCatFacetrackInstall *install,
    const char *origin, FacetrackFetch fetch, void *user) {
    const size_t count = install->sources.count;
    if (count == 0) return fetch(origin, user);
    for (size_t step = 0; step < count; ++step) {
        const size_t index = (install->preferred_source + step) % count;
        char url[BONGO_CAT_UPDATE_URL_CAP];
        if (!bongo_cat_update_source_url(install->sources.entries[index], origin,
                url, sizeof(url))) continue;
        if (atomic_load(&install->stop)) return false;
        const Uint64 started = SDL_GetTicks();
        if (fetch(url, user)) {
            install->preferred_source = index;
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 源可用: %s",
                install->sources.entries[index]);
            return true;
        }
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 源不可用/太慢: %s (%ums)",
            install->sources.entries[index], (unsigned)(SDL_GetTicks() - started));
    }
    return false;
}

/* 下面两个回调是 fetch_from_sources 的 fetch 参数 (C 没有 lambda, 只能这样传上下文)。 */
typedef struct FileFetchContext {
    const char *path;
    BongoCatFacetrackInstall *install;
    uint64_t *downloaded;
    uint64_t time_limit_ms;
    uint64_t min_bytes_per_second;
    char *error;
    size_t capacity;
} FileFetchContext;

typedef struct ManifestFetchContext {
    char **json;
    size_t *length;
    char *error;
    size_t capacity;
} ManifestFetchContext;


/* 让 <target_root>/<relative> 的父目录存在。 */
static bool ensure_parent(const char *target_root, const char *relative) {
    char full[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(full, sizeof(full), target_root, relative)) return false;
    char *slash = strrchr(full, '\\');
    char *forward = strrchr(full, '/');
    if (forward && (!slash || forward > slash)) slash = forward;
    if (!slash) return true;   /* 就在根下, 不用建目录 */
    *slash = 0;
    return bongo_cat_path_create_directory(full);
}

static bool progress_trampoline(uint64_t received, uint64_t total, void *user) {
    BongoCatFacetrackInstall *install = user;
    if (atomic_load(&install->stop)) return false;
    const Uint64 now = SDL_GetTicks();
    SDL_LockMutex(install->lock);
    install->snapshot.file_bytes = received;
    install->snapshot.file_total = total;
    /* 速度用"每 ~半秒的增量"估算, 不做指数平滑: 用户要的是"现在大概多快",
       半秒粒度已经够稳, 而平滑会让刚换源时的跳变看不出来。 */
    if (install->speed_mark_ticks == 0) {
        install->speed_mark_ticks = now;
        install->speed_mark_bytes = received;
    } else if (now - install->speed_mark_ticks >= 500) {
        const Uint64 elapsed = now - install->speed_mark_ticks;
        const uint64_t delta = received > install->speed_mark_bytes
            ? received - install->speed_mark_bytes : 0;
        install->snapshot.bytes_per_second = delta * 1000u / elapsed;
        install->speed_mark_ticks = now;
        install->speed_mark_bytes = received;
    }
    SDL_UnlockMutex(install->lock);
    return true;
}

static bool file_fetch_cb(const char *url, void *user) {
    FileFetchContext *ctx = user;
    return bongo_cat_facetrack_http_get_file(url, ctx->path, progress_trampoline,
        ctx->install, ctx->downloaded, ctx->time_limit_ms,
        ctx->min_bytes_per_second, ctx->error, ctx->capacity);
}

static bool manifest_fetch_cb(const char *url, void *user) {
    ManifestFetchContext *ctx = user;
    return bongo_cat_facetrack_http_get_memory(url, ctx->json, ctx->length,
        ctx->error, ctx->capacity);
}

/* 下载一个文件到 .part -> 校验 -> 改名。 */
static bool install_file(BongoCatFacetrackInstall *install,
    const BongoCatFacetrackFile *file,
    char *error, size_t capacity) {
    char url[BONGO_CAT_UPDATE_URL_CAP];
    if (snprintf(url, sizeof(url), "%s%s", install->manifest.base_url, file->name) >=
            (int)sizeof(url)) {
        snprintf(error, capacity, "下载地址过长");
        return false;
    }
    if (!ensure_parent(install->target_root, file->path)) {
        snprintf(error, capacity, "创建目录失败");
        return false;
    }
    char final_path[BONGO_CAT_PATH_CAP];
    char part_path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(final_path, sizeof(final_path), install->target_root,
            file->path)) {
        snprintf(error, capacity, "目标路径过长");
        return false;
    }
    snprintf(part_path, sizeof(part_path), "%s.part", final_path);

    SDL_LockMutex(install->lock);
    snprintf(install->snapshot.file_name, sizeof(install->snapshot.file_name), "%s",
        file->name);
    install->snapshot.file_bytes = 0;
    install->snapshot.file_total = file->size;
    SDL_UnlockMutex(install->lock);

    uint64_t downloaded = 0;
    /* 每个源都从 .part 头开始写: 失败时上一个源可能留了半截, 不能续。 */
    FileFetchContext ctx = { part_path, install, &downloaded, 0, 0, error,
        capacity };
    bool fetched = false;
    if (!install->source_raced) {
        /* 选源: 每个源都给 200 KB/s 的下限, 低于它立刻换下一个; 同时记下实测最快的那个。
           实测同一台机器上: 直连 16-80 KB/s, ghproxy 16, gh-proxy 57, ghfast.top 869。
           注意镜像速度会随时间波动 —— 所以"没有源达标"时不能老老实实退回 direct
           (那正好是最慢的一个), 而要退到实测最快的那个。 */
        const size_t count = install->sources.count;
        size_t best_index = 0;
        uint64_t best_rate = 0;
        bool completed = false;
        for (size_t step = 0; step < count; ++step) {
            const size_t index = (install->preferred_source + step) % count;
            char candidate[BONGO_CAT_UPDATE_URL_CAP];
            if (!bongo_cat_update_source_url(install->sources.entries[index], url,
                    candidate, sizeof(candidate))) continue;
            if (atomic_load(&install->stop)) break;
            uint64_t got = 0;
            FileFetchContext try_ctx = { part_path, install, &got, 0,
                200u * 1024u, error, capacity };
            const Uint64 started = SDL_GetTicks();
            const bool ok_try = bongo_cat_facetrack_http_get_file(candidate,
                try_ctx.path, progress_trampoline, install, &got, 0,
                200u * 1024u, error, capacity);
            const Uint64 elapsed = SDL_GetTicks() - started;
            const uint64_t rate = elapsed ? got * 1000u / elapsed : 0;
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 测源 %s: %llu KB, %llu ms, %llu KB/s%s",
                install->sources.entries[index], (unsigned long long)(got / 1024),
                (unsigned long long)elapsed, (unsigned long long)(rate / 1024),
                ok_try ? " (完成)" : "");
            if (rate > best_rate) { best_rate = rate; best_index = index; }
            remove(part_path);
            if (ok_try) { completed = true; break; }
        }
        install->preferred_source = best_index;
        install->source_raced = true;
        downloaded = 0;
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 用源 %s 下载 (实测最快)",
            install->sources.entries[best_index]);
        /* 无论竞速里有没有完整下完, 都用选中的源完整重下一遍 (竞速那份可能是半截) */
        fetched = fetch_from_sources(install, url, file_fetch_cb, &ctx);
        (void)completed;
    } else {
        fetched = fetch_from_sources(install, url, file_fetch_cb, &ctx);
    }
    if (!fetched) {
        remove(part_path);
        return false;
    }
    BongoCatError verify_error = {0};
    if (bongo_cat_update_verify_download(part_path, file->sha256,
            &verify_error) != BONGO_CAT_OK) {
        /* verify 已经删掉了可疑文件, 这里只补一句原因 */
        snprintf(error, capacity, "%s 校验失败, 请重试", file->name);
        return false;
    }
    remove(final_path);
    if (rename(part_path, final_path) != 0) {
        snprintf(error, capacity, "写入 %s 失败 (被占用?)", file->name);
        remove(part_path);
        return false;
    }
    SDL_LockMutex(install->lock);
    install->snapshot.bytes += file->size ? file->size : downloaded;
    SDL_UnlockMutex(install->lock);
    return true;
}

static int install_thread(void *userdata) {
    BongoCatFacetrackInstall *install = userdata;
    atomic_fetch_add(&g_install_threads, 1);
    char error[192] = {0};
    char *json = NULL;
    size_t length = 0;
    ManifestFetchContext manifest = { &json, &length, error, sizeof(error) };
    const bool got_manifest = fetch_from_sources(install,
        BONGO_CAT_FACETRACK_MANIFEST_URL, manifest_fetch_cb, &manifest);
    if (!got_manifest) {
        lock_set_status(install, BONGO_CAT_FACETRACK_INSTALL_FAILED, error);
        atomic_store(&install->running, false);
        atomic_fetch_sub(&g_install_threads, 1);
        return 0;
    }
    bool ok = bongo_cat_facetrack_parse_manifest(json, length, &install->manifest,
        error, sizeof(error));
    free(json);
    if (!ok) {
        lock_set_status(install, BONGO_CAT_FACETRACK_INSTALL_FAILED, error);
        atomic_store(&install->running, false);
        atomic_fetch_sub(&g_install_threads, 1);
        return 0;
    }
    /* 只查版本: 比较完就把结果写进快照, 不下载任何文件 */
    if (install->check_only) {
        char installed[32] = {0};
        const bool have = bongo_cat_facetrack_install_version(install->target_root,
            installed, sizeof(installed));
        const bool newer = !have || !installed[0] ||
            bongo_cat_update_compare_versions(install->manifest.version, installed) > 0;
        SDL_LockMutex(install->lock);
        snprintf(install->snapshot.latest_version,
            sizeof(install->snapshot.latest_version), "%s", install->manifest.version);
        snprintf(install->snapshot.installed_version,
            sizeof(install->snapshot.installed_version), "%s",
            have ? installed : "");
        install->snapshot.update_available = newer;
        install->snapshot.state = BONGO_CAT_FACETRACK_INSTALL_DONE;
        SDL_UnlockMutex(install->lock);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[face] 版本检查: 已装 %s, 最新 %s, %s", have ? installed : "(无)",
            install->manifest.version, newer ? "有新版本" : "已最新");
        atomic_store(&install->running, false);
        atomic_fetch_sub(&g_install_threads, 1);
        return 0;
    }

    SDL_LockMutex(install->lock);
    install->snapshot.file_count = (uint32_t)install->manifest.count;
    install->snapshot.total = 0;
    for (size_t i = 0; i < install->manifest.count; ++i)
        install->snapshot.total += install->manifest.files[i].size;
    SDL_UnlockMutex(install->lock);

    for (size_t i = 0; i < install->manifest.count; ++i) {
        if (atomic_load(&install->stop)) break;
        SDL_LockMutex(install->lock);
        install->snapshot.file_index = (uint32_t)i + 1;
        SDL_UnlockMutex(install->lock);
        if (!install_file(install, &install->manifest.files[i], error, sizeof(error))) {
            lock_set_status(install, BONGO_CAT_FACETRACK_INSTALL_FAILED, error);
            atomic_store(&install->running, false);
            atomic_fetch_sub(&g_install_threads, 1);
            return 0;
        }
    }
    const bool stopped = atomic_load(&install->stop);
    lock_set_status(install, stopped ? BONGO_CAT_FACETRACK_INSTALL_FAILED
        : BONGO_CAT_FACETRACK_INSTALL_DONE, stopped ? "已取消" : NULL);
    if (!stopped) {
        bongo_cat_facetrack_install_write_version(install->target_root,
            install->manifest.version);
        SDL_LockMutex(install->lock);
        snprintf(install->snapshot.installed_version,
            sizeof(install->snapshot.installed_version), "%s",
            install->manifest.version);
        SDL_UnlockMutex(install->lock);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[face] 组件下载完成 (v%s): %s",
            install->manifest.version, install->target_root);
    }
    atomic_store(&install->running, false);
    atomic_fetch_sub(&g_install_threads, 1);
    if (atomic_load(&install->orphan)) {
        if (install->lock) SDL_DestroyMutex(install->lock);
        free(install);
    }
    return 0;
}

BongoCatFacetrackInstall *bongo_cat_facetrack_install_create(
    const char *target_root) {
    if (!target_root || !target_root[0]) return NULL;
    BongoCatFacetrackInstall *install = calloc(1, sizeof(*install));
    if (!install) return NULL;
    install->lock = SDL_CreateMutex();
    if (!install->lock) {
        free(install);
        return NULL;
    }
    snprintf(install->target_root, sizeof(install->target_root), "%s", target_root);
    install->snapshot.state = BONGO_CAT_FACETRACK_INSTALL_IDLE;
    bongo_cat_update_source_list_default(&install->sources);
    install->preferred_source = 0;
    return install;
}

void bongo_cat_facetrack_install_destroy(BongoCatFacetrackInstall *install) {
    if (!install) return;
    /* **不要 join**: 检查/下载线程可能正卡在 WinHTTP 里十几秒, join 会把设置窗口冻住,
       用户看到的就是"未响应"。改成 detach, 线程退出时自己释放结构。 */
    atomic_store(&install->stop, true);
    if (install->thread) {
        SDL_DetachThread(install->thread);
        install->thread = NULL;
        atomic_store(&install->orphan, true);
        return;
    }
    if (install->lock) SDL_DestroyMutex(install->lock);
    free(install);
}

static bool start_thread(BongoCatFacetrackInstall *install, bool check_only) {
    if (!install) return false;
    /* 上一轮线程还没退出: 现在起新的会同时写同一份快照, 等下一帧再试 */
    if (bongo_cat_facetrack_install_active_threads() > 0) return false;
    install->check_only = check_only;
    if (!bongo_cat_path_create_directory(install->target_root)) return false;
    SDL_LockMutex(install->lock);
    memset(&install->snapshot, 0, sizeof(install->snapshot));
    install->snapshot.state = check_only ? BONGO_CAT_FACETRACK_INSTALL_CHECKING
        : BONGO_CAT_FACETRACK_INSTALL_WORKING;
    install->snapshot.check_mode = check_only;
    SDL_UnlockMutex(install->lock);
    atomic_store(&install->stop, false);
    atomic_store(&install->running, true);
    install->thread = SDL_CreateThread(install_thread, "facetrack-install", install);
    if (!install->thread) {
        atomic_store(&install->running, false);
        lock_set_status(install, BONGO_CAT_FACETRACK_INSTALL_FAILED,
            "创建下载线程失败");
        return false;
    }
    return true;
}

bool bongo_cat_facetrack_install_check_start(BongoCatFacetrackInstall *install) {
    if (!install) return false;
    if (atomic_load(&install->running)) return true;
    return start_thread(install, true);
}

bool bongo_cat_facetrack_install_start(BongoCatFacetrackInstall *install) {
    if (!install) return false;
    if (atomic_load(&install->running)) return true;
    return start_thread(install, false);
}

void bongo_cat_facetrack_install_stop(BongoCatFacetrackInstall *install) {
    if (!install || !install->thread) return;
    atomic_store(&install->stop, true);
    SDL_WaitThread(install->thread, NULL);
    install->thread = NULL;
    atomic_store(&install->running, false);
}

void bongo_cat_facetrack_install_snapshot(BongoCatFacetrackInstall *install,
    BongoCatFacetrackInstallSnapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!install || !install->lock) return;
    SDL_LockMutex(install->lock);
    *out = install->snapshot;
    SDL_UnlockMutex(install->lock);
}

bool bongo_cat_facetrack_install_present(const char *target_root) {
    if (!target_root || !target_root[0]) return false;
    static const char *required[] = {
        "bongo_facetrack.dll", "onnxruntime.dll", "models/mnv3_detection_opt.onnx",
        "models/lm_model0_opt.onnx"
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        char path[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_path_join(path, sizeof(path), target_root, required[i]) ||
            !bongo_cat_path_is_file(path)) return false;
    }
    return true;
}
