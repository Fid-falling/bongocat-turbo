/*
 * 面捕组件发布清单 (manifest.json) 的解析。
 *
 * 单独一个文件是为了守住 facetrack_install.c 的行数策略 (CheckLines.cmake 500 行上限):
 * 下载/选源/校验已经把那一个文件顶满, 把解析拆出来最省事, 也让"清单格式"这一件事有单一归属。
 *
 * 清单形状 (组件仓库 release 里的 manifest.json):
 *   { "version": "1.3.0", "abi": 1, "base_url": "https://.../v1.3.0/",
 *     "files": [ { "name": "...", "path": "models/x.onnx", "sha256": "...", "size": 123 }, ... ] }
 */
#include "facetrack_install.h"

#include "bongo_cat/update.h"

#include <yyjson.h>

#include <stdio.h>
#include <string.h>

bool bongo_cat_facetrack_parse_manifest(const char *json, size_t length,
    BongoCatFacetrackManifest *manifest, char *error, size_t capacity) {
    if (!manifest) return false;
    memset(manifest, 0, sizeof(*manifest));
    yyjson_doc *doc = yyjson_read(json, length, 0);
    if (!doc) {
        snprintf(error, capacity, "组件清单不是合法 JSON");
        return false;
    }
    bool ok = false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *version = yyjson_obj_get(root, "version");
    yyjson_val *base = yyjson_obj_get(root, "base_url");
    yyjson_val *files = yyjson_obj_get(root, "files");
    if (!yyjson_is_str(base) || !yyjson_is_arr(files)) {
        snprintf(error, capacity, "组件清单缺少 base_url / files");
        goto done;
    }
    snprintf(manifest->base_url, sizeof(manifest->base_url), "%s",
        yyjson_get_str(base));
    if (yyjson_is_str(version))
        snprintf(manifest->version, sizeof(manifest->version), "%s",
            yyjson_get_str(version));
    yyjson_val *item;
    yyjson_arr_iter iter = yyjson_arr_iter_with(files);
    while ((item = yyjson_arr_iter_next(&iter))) {
        if (manifest->count >= BONGO_CAT_FACETRACK_MAX_FILES) break;
        yyjson_val *name = yyjson_obj_get(item, "name");
        yyjson_val *path = yyjson_obj_get(item, "path");
        yyjson_val *sha = yyjson_obj_get(item, "sha256");
        yyjson_val *size = yyjson_obj_get(item, "size");
        if (!yyjson_is_str(name) || !yyjson_is_str(path) || !yyjson_is_str(sha))
            continue;
        BongoCatFacetrackFile *file = &manifest->files[manifest->count];
        snprintf(file->name, sizeof(file->name), "%s", yyjson_get_str(name));
        snprintf(file->path, sizeof(file->path), "%s", yyjson_get_str(path));
        if (!bongo_cat_update_sha256_text(yyjson_get_str(sha), file->sha256)) {
            snprintf(error, capacity, "组件清单里 %s 的 SHA256 不合法", file->name);
            goto done;
        }
        file->size = yyjson_is_uint(size) ? (uint64_t)yyjson_get_uint(size) : 0;
        manifest->count++;
    }
    if (manifest->count == 0) {
        snprintf(error, capacity, "组件清单里没有可下载的文件");
        goto done;
    }
    ok = true;

done:
    yyjson_doc_free(doc);
    return ok;
}
