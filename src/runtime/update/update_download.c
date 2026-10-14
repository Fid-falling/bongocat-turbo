/*
 * 一键更新的"下载并校验"阶段 (工作线程里跑, 因此可以阻塞)。
 *
 * 顺序与信任边界:
 *   1. 期望哈希: 只认官方来源 (GitHub API 的 asset.digest / release 说明 / 官方 .sha256
 *      资产)。一个都没有就**不下载**, 直接判为"无法校验, 已取消"——宁可不让用户更新, 也
 *      不装未经验证的二进制。第三级要额外抓一次校验文件, 但那个抓取**只走官方源**;
 *   2. 落点: exe 同目录下的 <exe 名>.new.exe (替换必须同卷), 目录不可写就退到缓存目录
 *      并标记"只能手动替换";
 *   3. 逐源下载: 镜像只出现在这一步。顺序 = 用户配置或内置默认, 上次成功的源排在最前;
 *      一个源失败就换下一个, 全部失败才报错。镜像返回的任何内容都要过第 4 步;
 *   4. 校验: 本地重算 SHA-256 与期望值逐字节比较, 不一致就删掉文件并报错 (core/update_digest.c)。
 *
 * 因此镜像被投毒/被劫持的最坏结果只是"这次更新失败", 不会装上被篡改的程序。
 */
#include "update_internal.h"
#include "update_install.h"
#include "bongo_cat/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 单个源的尝试次数上限。同一源只重试"传了一半断掉"的情况 (见 download_from_sources):
   连不上/返回 404 说明这个源不可用, 原地重试只是浪费用户时间。 */
#define UPDATE_SOURCE_ATTEMPTS 2

/* 期望哈希: 三级回退。digest 与说明在解析 JSON 时就已经填好, 这里只处理"需要额外抓一份
   校验资产"的情况。 */
static bool resolve_expected(BongoCatUpdateService *service, char *error,
    size_t capacity) {
    BongoCatUpdateAsset *asset = &service->asset;
    snprintf(asset->sha256, sizeof(asset->sha256), "%s",
        service->release.portable.sha256);
    asset->checksum = service->release.portable.checksum;
    if (asset->checksum == BONGO_CAT_UPDATE_CHECKSUM_ASSET) {
        char *content = NULL;
        char message[256] = {0};
        BongoCatUpdateFetchResult fetched = bongo_cat_update_http_fetch_url(
            service, service->release.portable.checksum_url, &content, message,
            sizeof(message));
        bool parsed = fetched == BONGO_CAT_UPDATE_FETCH_OK && content &&
            bongo_cat_update_checksum_file_sha256(content, asset->name,
                asset->sha256);
        free(content);
        if (!parsed) {
            SDL_LogWarn(BONGO_CAT_LOG_UPDATE,
                "Cannot read the release checksum asset (%s): %s",
                service->release.portable.checksum_url, message);
            asset->checksum = BONGO_CAT_UPDATE_CHECKSUM_NONE;
            asset->sha256[0] = '\0';
        }
    }
    if (asset->checksum == BONGO_CAT_UPDATE_CHECKSUM_NONE || !asset->sha256[0]) {
        snprintf(error, capacity, "%s", BONGO_CAT_UPDATE_ERROR_NO_CHECKSUM);
        SDL_LogError(BONGO_CAT_LOG_UPDATE,
            "The release has no usable checksum for %s; the update is cancelled",
            asset->name);
        return false;
    }
    SDL_LogInfo(BONGO_CAT_LOG_UPDATE, "Update checksum source: %s",
        asset->checksum == BONGO_CAT_UPDATE_CHECKSUM_DIGEST ? "asset digest" :
        (asset->checksum == BONGO_CAT_UPDATE_CHECKSUM_NOTES ?
            "release notes" : "release checksum asset"));
    return true;
}

/* 把当前进度 (第几个源/源名) 写进界面可见状态。 */
static void report_source(BongoCatUpdateService *service, size_t index,
    const char *label) {
    SDL_LockMutex(service->mutex);
    service->source_index = (unsigned)(index + 1);
    service->source_count = (unsigned)service->sources.count;
    snprintf(service->source_label, sizeof(service->source_label), "%s", label);
    SDL_UnlockMutex(service->mutex);
}

static uint64_t received_bytes(BongoCatUpdateService *service) {
    SDL_LockMutex(service->mutex);
    uint64_t received = service->downloaded;
    SDL_UnlockMutex(service->mutex);
    return received;
}

/*
 * 按顺序尝试所有源。返回 true = 文件已下齐 (还没校验), 此时调用方接着校验。
 * 每个源最多试 UPDATE_SOURCE_ATTEMPTS 次, 且只在"已经收到一些字节又断了"时重试;
 * 成功的那一个会被记为下次的首选源 (update_source_store.c)。
 */
static bool download_from_sources(BongoCatUpdateService *service, char *error,
    size_t capacity) {
    const char *origin = service->asset.url;
    for (size_t index = 0; index < service->sources.count; ++index) {
        const char *entry = service->sources.entries[index];
        char url[BONGO_CAT_UPDATE_URL_CAP * 2];
        if (!bongo_cat_update_source_url(entry, origin, url, sizeof(url))) {
            SDL_LogWarn(BONGO_CAT_LOG_UPDATE,
                "Ignoring an invalid update source: %s", entry);
            continue;
        }
        char label[BONGO_CAT_UPDATE_SOURCE_LABEL_CAP];
        bongo_cat_update_source_label(entry, label, sizeof(label));
        report_source(service, index, label);
        SDL_LogInfo(BONGO_CAT_LOG_UPDATE,
            "Downloading the update from source %u/%u (%s)",
            (unsigned)(index + 1), (unsigned)service->sources.count, label);
        for (int attempt = 0; attempt < UPDATE_SOURCE_ATTEMPTS; ++attempt) {
            uint64_t before = received_bytes(service);
            BongoCatUpdateFetchResult result = bongo_cat_update_http_download(
                service, url, service->new_path, error, capacity);
            if (result == BONGO_CAT_UPDATE_FETCH_OK) {
                bongo_cat_update_sources_remember(service, index);
                return true;
            }
            if (result == BONGO_CAT_UPDATE_FETCH_CANCELLED) return false;
            SDL_LogWarn(BONGO_CAT_LOG_UPDATE,
                "Update source %s failed (attempt %d): %s", label, attempt + 1,
                error[0] ? error : "unknown error");
            uint64_t after = received_bytes(service);
            if (result != BONGO_CAT_UPDATE_FETCH_NETWORK || after <= before)
                break;
        }
    }
    snprintf(error, capacity, "%s", BONGO_CAT_UPDATE_ERROR_SOURCES_EXHAUSTED);
    return false;
}

/*
 * 下载并校验。返回 true 时 service->new_path 上是一份**已通过 SHA-256 校验**的文件,
 * 可以进入替换阶段; 返回 false 时 error 里是 update.h 里那几个英文原文之一
 * (界面由 core/update_error.c 翻成人话), 临时文件已经被删除。
 */
bool bongo_cat_update_download_asset(BongoCatUpdateService *service, char *error,
    size_t capacity) {
    if (!service || !error || !capacity) return false;
    error[0] = '\0';
    snprintf(service->asset.name, sizeof(service->asset.name), "%s",
        service->release.portable.name);
    snprintf(service->asset.url, sizeof(service->asset.url), "%s",
        service->release.portable.url);
    if (!resolve_expected(service, error, capacity)) return false;
    if (!bongo_cat_update_install_target(service, service->new_path,
            sizeof(service->new_path))) {
        snprintf(error, capacity, "%s", BONGO_CAT_UPDATE_ERROR_NOT_WRITABLE);
        return false;
    }
    /* 每次下载都重读一次源列表: 用户可能在两次更新之间编辑了 update-sources.txt。 */
    bongo_cat_update_sources_load(service);
    bongo_cat_update_report_stage(service, BONGO_CAT_UPDATE_DOWNLOADING);
    if (!download_from_sources(service, error, capacity)) return false;
    bongo_cat_update_report_stage(service, BONGO_CAT_UPDATE_VERIFYING);
    BongoCatError verify = {0};
    if (bongo_cat_update_verify_download(service->new_path, service->asset.sha256,
            &verify) != BONGO_CAT_OK) {
        snprintf(error, capacity, "%s",
            verify.message[0] ? verify.message :
            BONGO_CAT_UPDATE_ERROR_HASH_MISMATCH);
        SDL_LogError(BONGO_CAT_LOG_UPDATE,
            "The downloaded update was rejected: %s (source hash %s)", error,
            service->asset.sha256);
        return false;
    }
    SDL_LogInfo(BONGO_CAT_LOG_UPDATE,
        "The downloaded update passed the SHA-256 check: %s", service->new_path);
    return true;
}
