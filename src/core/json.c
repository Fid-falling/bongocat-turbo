#include "bongo_cat/json.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdio.h>

/*
 * yyjson 的薄封装。统一三件事:
 *   - 读之前先卡文件大小上限, 避免把异常大的文件整个读进内存;
 *   - 统一用 bongo_cat_file_open 打开(Windows 上走宽字符路径);
 *   - 写失败时删掉半成品文件 —— 留下一个截断的 JSON 比没有文件更糟,
 *     下次启动会把它当成损坏的配置。
 */
#define BONGO_CAT_JSON_FILE_CAP (16u * 1024u * 1024u)

yyjson_doc *bongo_cat_json_read_file(const char *path,
    yyjson_read_flag flags, yyjson_read_err *error) {
    uint64_t size;
    if (!bongo_cat_path_file_size(path, &size) ||
        size == 0 || size > BONGO_CAT_JSON_FILE_CAP) return NULL;
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) return NULL;
    yyjson_doc *document = yyjson_read_fp(file, flags, NULL, error);
    /* fclose 负责 flush; 它失败说明数据没完整落盘, 此时不能把解析结果交出去。 */
    if (fclose(file) != 0 && document) {
        yyjson_doc_free(document); document = NULL;
    }
    return document;
}

bool bongo_cat_json_write_file(const char *path,
    const yyjson_mut_doc *document, yyjson_write_flag flags,
    yyjson_write_err *error) {
    if (!path || !document) return false;
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) return false;
    bool ok = yyjson_mut_write_fp(file, document, flags, NULL, error);
    if (fclose(file) != 0) ok = false;
    /* 只写了一半的文件必须删掉, 否则会被下一次读取当成损坏的配置。 */
    if (!ok) bongo_cat_path_remove(path);
    return ok;
}
