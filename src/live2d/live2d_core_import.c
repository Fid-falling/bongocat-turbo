/*
 * Live2D Cubism Core 的导入编排: 用户选中的官方 SDK 压缩包(或裸动态库) -> 存储根。
 *
 * 分工: 压缩包本身的安全规则在 live2d_core_archive.c(不可信输入), 本文件只管可信的本机
 * 状态机:
 *   1. 先在 <存储根>/cores/.staging/ 写完整文件(解压出来的或复制过来的都一样);
 *   2. 校验: 机器架构(PE/ELF/Mach-O) + 三符号探测(csmGetVersion/csmReviveMocInPlace/
 *      csmInitializeModelInPlace) + SHA-256(只记录给用户核对, 不当白名单 —— 官方会换版本);
 *   3. 落盘: 旧 Core 先改名成 <库名>.<旧sha8>.bak(可一键回滚), 再原子替换;
 *   4. 记录 core.json(schema/平台/架构/哈希/csm 版本/来源/sdk 根目录名/上一个版本);
 *   5. 重新加载 Core, 让"导入完立刻可用"成立(调用方需保证此时没有正在使用的模型实例)。
 * 任何一步失败: 只删 .staging, 旧 Core 与旧 core.json 完全不变 —— 导入失败不能让用户
 * 现有的渲染能力一起消失。
 *
 * 为什么不在应用内下载: 见 LIVE2D-PLUGIN-PLAN.md §14 —— 代下载法律上更激进, v1 不做。
 * 本文件里没有任何网络代码, 这是刻意的。
 */
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/file.h"
#include "bongo_cat/json.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"
#include "bongo_cat/sha256.h"

#include "live2d_core_internal.h"

#include <SDL3/SDL.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CORE_MANIFEST "core.json"
/* 删除安装目录里多余的 .bak: 只保留"最近一次被替换的 Core"(回滚要用), 避免用户每换一次
   版本就留下几十 MB 的孤儿文件。 */
typedef struct BackupPrune {
    const char *keep;
} BackupPrune;

static BongoCatPathVisit prune_visitor(void *userdata, const char *directory,
    const char *name) {
    BackupPrune *prune = userdata;
    size_t length = strlen(name);
    if (length < 4 || strcmp(name + length - 4, ".bak") != 0)
        return BONGO_CAT_PATH_CONTINUE;
    char path[BONGO_CAT_PATH_CAP];
    if (bongo_cat_path_join(path, sizeof(path), directory, name) &&
        !(prune->keep && !strcmp(path, prune->keep)))
        bongo_cat_file_remove(path);
    return BONGO_CAT_PATH_CONTINUE;
}

static void prune_backups(const char *install, const char *keep) {
    BackupPrune prune = {keep};
    bongo_cat_path_enumerate(install, prune_visitor, &prune);
}

static bool remove_file(const char *path) {
    return !bongo_cat_path_is_file(path) || bongo_cat_file_remove(path);
}

/* .bak 名称带旧哈希前 8 位: 用户能一眼看出"这份是哪个版本", 也避免覆盖不同版本。 */
static void backup_name(char *output, size_t capacity, const char *library,
    const char *sha256) {
    char digest[9] = {0};
    if (sha256 && strlen(sha256) >= 8) memcpy(digest, sha256, 8);
    snprintf(output, capacity, "%s.%s.bak", library, digest[0] ? digest : "old");
}

static void manifest_path(char *output, size_t capacity) {
    char root[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_live2d_core_root_directory(root, sizeof(root))) {
        output[0] = '\0';
        return;
    }
    if (!bongo_cat_path_join(output, capacity, root, CORE_MANIFEST))
        output[0] = '\0';
}

/* 写 core.json: 记录"这份 Core 从哪来、是什么、什么时候导入的、上一个版本在哪"。
   先写临时文件再原子替换 —— 半截的 JSON 会让下一次回滚读不到 previous。 */
static bool write_manifest(const BongoCatLive2DCoreImport *report) {
    char path[BONGO_CAT_PATH_CAP], temporary[BONGO_CAT_PATH_CAP];
    manifest_path(path, sizeof(path));
    if (!path[0] || snprintf(temporary, sizeof(temporary), "%s.new", path) <= 0)
        return false;
    yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
    if (!document) return false;
    yyjson_mut_val *root = yyjson_mut_obj(document);
    yyjson_mut_doc_set_root(document, root);
    yyjson_mut_obj_add_int(document, root, "schema", 1);
    yyjson_mut_obj_add_str(document, root, "platform",
        bongo_cat_live2d_core_platform_name());
    yyjson_mut_obj_add_str(document, root, "arch", bongo_cat_live2d_core_arch_name());
    yyjson_mut_obj_add_str(document, root, "library",
        bongo_cat_live2d_core_library_name());
    yyjson_mut_obj_add_str(document, root, "installed", report->installed);
    yyjson_mut_obj_add_str(document, root, "source", report->source);
    yyjson_mut_obj_add_str(document, root, "sha256", report->sha256);
    yyjson_mut_obj_add_str(document, root, "previous", report->previous);
    yyjson_mut_obj_add_str(document, root, "sdk_hint", report->sdk_hint);
    yyjson_mut_obj_add_uint(document, root, "csm_version", report->csm_version);
    yyjson_mut_obj_add_int(document, root, "imported_at", (int64_t)time(NULL));
    yyjson_write_err write_error = {0};
    bool ok = bongo_cat_json_write_file(temporary, document, 0, &write_error);
    yyjson_mut_doc_free(document);
    return ok && bongo_cat_file_replace(temporary, path, true);
}

/* 校验 + 落盘 + 记录 + 重新加载。staged 是已经写好的候选文件(在 .staging 里)。 */
static BongoCatResult install_core(const char *staged, const char *source,
    bool from_archive, BongoCatLive2DCoreImport *report, BongoCatError *error) {
    if (!bongo_cat_platform_live2d_core_arch_matches(staged)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
            "The selected file is not a %s/%s Cubism Core (architecture mismatch)",
            bongo_cat_live2d_core_platform_name(), bongo_cat_live2d_core_arch_name());
        return BONGO_CAT_ERROR_CUBISM;
    }
    BongoCatError probe_error = {0};
    void *handle = bongo_cat_platform_live2d_core_open(staged, &probe_error);
    if (!handle || !bongo_cat_platform_live2d_core_probe(handle)) {
        if (handle) bongo_cat_platform_live2d_core_close(handle);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
            "The selected file is not a usable Cubism Core: %s",
            probe_error.message[0] ? probe_error.message : "missing core symbols");
        return BONGO_CAT_ERROR_CUBISM;
    }
    unsigned int (*version_fn)(void) = (unsigned int (*)(void))
        bongo_cat_platform_live2d_core_symbol(handle, "csmGetVersion");
    uint32_t version = version_fn ? version_fn() : 0;
    /* 探测句柄必须在替换文件之前关掉: Windows 上被加载的 DLL 无法被覆盖/改名。 */
    bongo_cat_platform_live2d_core_close(handle);

    char install[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_live2d_core_install_directory(install, sizeof(install)) ||
        !bongo_cat_path_create_directory(install)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot create the Cubism Core directory");
        return BONGO_CAT_ERROR_IO;
    }
    const char *library = bongo_cat_live2d_core_library_name();
    char target[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(target, sizeof(target), install, library)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "The Cubism Core path is too long");
        return BONGO_CAT_ERROR_IO;
    }
    snprintf(report->source, sizeof(report->source), "%s", source);
    snprintf(report->installed, sizeof(report->installed), "%s", target);
    snprintf(report->sha256, sizeof(report->sha256), "%s", "");
    report->csm_version = version;
    report->from_archive = from_archive;

    BongoCatError hash_error = {0};
    if (bongo_cat_sha256_file(staged, report->sha256, &hash_error) != BONGO_CAT_OK) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot hash the imported Cubism Core: %s", hash_error.message);
        return BONGO_CAT_ERROR_IO;
    }
    /* 旧 Core 先改名而不是直接删除: 用户换错版本(比如拿了 x86 的包)时要能一键回滚。 */
    report->previous[0] = '\0';
    if (bongo_cat_path_is_file(target)) {
        char backup[BONGO_CAT_PATH_CAP], backup_path[BONGO_CAT_PATH_CAP];
        char old_hash[65] = {0};
        BongoCatError old_error = {0};
        if (bongo_cat_sha256_file(target, old_hash, &old_error) == BONGO_CAT_OK) {
            backup_name(backup, sizeof(backup), library, old_hash);
            if (bongo_cat_path_join(backup_path, sizeof(backup_path), install,
                    backup)) {
                remove_file(backup_path);
                if (bongo_cat_path_rename(target, backup_path))
                    snprintf(report->previous, sizeof(report->previous), "%s",
                        backup_path);
            }
        }
        if (!report->previous[0] && !remove_file(target)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
                "Cannot replace the existing Cubism Core");
            return BONGO_CAT_ERROR_IO;
        }
    }
    prune_backups(install, report->previous[0] ? report->previous : NULL);
    if (!bongo_cat_file_replace(staged, target, true)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot move the Cubism Core into place");
        return BONGO_CAT_ERROR_IO;
    }
    if (!write_manifest(report))
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] Core 已安装但 core.json 写入失败(不影响渲染, 只影响回滚元数据)");
    /* 重新加载: 调用方必须已经销毁模型实例与 GPU 资源, 否则旧句柄会在使用中失效。 */
    bongo_cat_live2d_core_shutdown();
    bongo_cat_live2d_core_initialize();
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 已导入用户提供的 Cubism Core (csm 版本 %u, sha256 %s): %s",
        (unsigned)version, report->sha256, target);
    return BONGO_CAT_OK;
}

/* 裸动态库输入: 复制到 staging 并加上大小上限。 */
static BongoCatResult stage_plain_file(const char *source, const char *staged,
    BongoCatError *error) {
    uint64_t size = 0;
    if (!bongo_cat_path_file_size(source, &size) || !size) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "The selected file is empty or unreadable: %s", source);
        return BONGO_CAT_ERROR_IO;
    }
    if (size > BONGO_CAT_CORE_ENTRY_LIMIT) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "The selected file is larger than the accepted limit (%llu bytes)",
            (unsigned long long)size);
        return BONGO_CAT_ERROR_FORMAT;
    }
    FILE *input = bongo_cat_file_open(source, "rb");
    FILE *output = input ? bongo_cat_file_open(staged, "wb") : NULL;
    if (!input || !output) {
        if (input) fclose(input);
        if (output) fclose(output);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot copy the selected Cubism Core");
        return BONGO_CAT_ERROR_IO;
    }
    unsigned char buffer[64 * 1024];
    uint64_t remaining = size;
    bool ok = true;
    while (ok && remaining) {
        size_t want = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        size_t got = fread(buffer, 1, want, input);
        if (got != want || fwrite(buffer, 1, got, output) != got) ok = false;
        remaining -= got;
    }
    if (fclose(input) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    if (!ok) {
        bongo_cat_file_remove(staged);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot copy the selected Cubism Core (disk full or unreadable)");
        return BONGO_CAT_ERROR_IO;
    }
    return BONGO_CAT_OK;
}

/* .staging 目录里的候选文件名: 固定用库名, 每次导入前先清掉上一次的残留。 */
static bool prepare_staging(char *directory, size_t directory_capacity,
    char *staged, size_t staged_capacity) {
    if (!bongo_cat_live2d_core_staging_directory(directory, directory_capacity) ||
        !bongo_cat_path_create_directory(directory) ||
        !bongo_cat_path_join(staged, staged_capacity, directory,
            bongo_cat_live2d_core_library_name())) return false;
    return remove_file(staged);
}

BongoCatResult bongo_cat_live2d_core_import(const char *source,
    BongoCatLive2DCoreImport *report, BongoCatError *error) {
    if (!source || !source[0] || !report) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Cannot import a Cubism Core without a source file");
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    *report = (BongoCatLive2DCoreImport){0};
    char staging[BONGO_CAT_PATH_CAP], staged[BONGO_CAT_PATH_CAP];
    if (!prepare_staging(staging, sizeof(staging), staged, sizeof(staged))) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot prepare the Cubism Core staging directory");
        return BONGO_CAT_ERROR_IO;
    }
    /* 扩展名判定只用来选路径: 用户可能选中一个没有扩展名的 zip, 那种情况会以
       "不是可读的 ZIP" 明确失败, 而不是被塞进裸文件分支后报架构不匹配。 */
    bool archive = bongo_cat_path_is_dir(source) ? false : true;
    const char *name = bongo_cat_path_name(source);
    size_t name_length = name ? strlen(name) : 0;
    if (name_length >= 4) {
        char suffix[5] = {0};
        for (size_t i = 0; i < 4; ++i)
            suffix[i] = (char)tolower((unsigned char)name[name_length - 4 + i]);
        archive = !strcmp(suffix, ".zip");
    }
    BongoCatResult result = archive
        ? bongo_cat_live2d_core_extract_archive(source, staged, report->sdk_hint,
            sizeof(report->sdk_hint), error)
        : stage_plain_file(source, staged, error);
    if (result != BONGO_CAT_OK) {
        remove_file(staged);
        *report = (BongoCatLive2DCoreImport){0};
        return result;
    }
    /* install_core 只填"落盘信息", 不碰 sdk_hint(它是压缩包扫描阶段的产物), 所以这里
       不需要额外保存/还原它。 */
    result = install_core(staged, source, archive, report, error);
    if (result != BONGO_CAT_OK) {
        remove_file(staged);
        *report = (BongoCatLive2DCoreImport){0};
        return result;
    }
    return BONGO_CAT_OK;
}

bool bongo_cat_live2d_core_read_manifest(BongoCatLive2DCoreImport *report) {
    if (!report) return false;
    char path[BONGO_CAT_PATH_CAP];
    manifest_path(path, sizeof(path));
    if (!path[0]) return false;
    yyjson_doc *document = bongo_cat_json_read_file(path, 0, NULL);
    if (!document) return false;
    yyjson_val *root = yyjson_doc_get_root(document);
    *report = (BongoCatLive2DCoreImport){0};
#define CORE_MANIFEST_STR(field, key) do { \
        yyjson_val *value = yyjson_obj_get(root, key); \
        if (yyjson_is_str(value)) \
            snprintf(report->field, sizeof(report->field), "%s", \
                yyjson_get_str(value)); \
    } while (0)
    CORE_MANIFEST_STR(source, "source");
    CORE_MANIFEST_STR(installed, "installed");
    CORE_MANIFEST_STR(previous, "previous");
    CORE_MANIFEST_STR(sha256, "sha256");
    CORE_MANIFEST_STR(sdk_hint, "sdk_hint");
#undef CORE_MANIFEST_STR
    yyjson_val *version = yyjson_obj_get(root, "csm_version");
    report->csm_version = yyjson_is_uint(version) ?
        (uint32_t)yyjson_get_uint(version) : 0;
    yyjson_doc_free(document);
    return true;
}

BongoCatResult bongo_cat_live2d_core_rollback(BongoCatError *error) {
    BongoCatLive2DCoreImport current = {0};
    if (!bongo_cat_live2d_core_read_manifest(&current) || !current.previous[0] ||
        !bongo_cat_path_is_file(current.previous)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "There is no previous Cubism Core to restore");
        return BONGO_CAT_ERROR_FORMAT;
    }
    char install[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_live2d_core_install_directory(install, sizeof(install)) ||
        !bongo_cat_path_join(target, sizeof(target), install,
            bongo_cat_live2d_core_library_name())) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot resolve the Cubism Core directory");
        return BONGO_CAT_ERROR_IO;
    }
    char displaced[BONGO_CAT_PATH_CAP];
    backup_name(displaced, sizeof(displaced), bongo_cat_live2d_core_library_name(),
        current.sha256);
    char displaced_path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(displaced_path, sizeof(displaced_path), install,
            displaced)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot resolve the rollback path");
        return BONGO_CAT_ERROR_IO;
    }
    remove_file(displaced_path);
    if (bongo_cat_path_is_file(target) &&
        !bongo_cat_path_rename(target, displaced_path)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot move the current Cubism Core aside");
        return BONGO_CAT_ERROR_IO;
    }
    if (!bongo_cat_file_replace(current.previous, target, true)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot restore the previous Cubism Core");
        return BONGO_CAT_ERROR_IO;
    }
    BongoCatLive2DCoreImport restored = {0};
    restored.installed[0] = '\0';
    bongo_cat_sha256_file(target, restored.sha256, NULL);
    snprintf(restored.source, sizeof(restored.source), "%s", current.previous);
    snprintf(restored.installed, sizeof(restored.installed), "%s", target);
    snprintf(restored.previous, sizeof(restored.previous), "%s", displaced_path);
    bongo_cat_live2d_core_shutdown();
    bongo_cat_live2d_core_initialize();
    write_manifest(&restored);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[live2d] 已回滚到上一份 Cubism Core: %s",
        target);
    return BONGO_CAT_OK;
}
