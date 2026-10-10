#include "bongo_cat/json.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdlib.h>
#include <string.h>

/*
 * 模型清单(.model3.json 与 Mver 的 manifest)的容错解析。
 *
 * 这些文件经常被用户手改过(模型作者或搬运者用记事本改路径), 所以解析要能容忍
 * 三种常见的手写错误: 注释、尾随逗号、BOM。都不行之后, 再针对一种特定的手改
 * 痕迹做修复(见下面那处额外的 ']')。
 * 两个入口都有 4MB 上限: 模型清单本身很小, 超过这个尺寸多半是选错了文件。
 */
static size_t skip_space(const char *data, size_t size, size_t pos) {
    while (pos < size && (data[pos] == ' ' || data[pos] == '\t' ||
        data[pos] == '\r' || data[pos] == '\n')) pos++;
    return pos;
}

/* normalized 输出"是不是靠放宽规则或修复才成功的", 调用方据此决定要不要提示
   用户文件不规范。第一次先用严格模式解析 —— 能过就说明文件本身是干净的。 */
yyjson_doc *bongo_cat_model_json_parse(const char *data, size_t size,
    bool *normalized) {
    if (normalized) *normalized = false;
    if (!data || !size || size > 4u * 1024u * 1024u) return NULL;
    yyjson_doc *document = yyjson_read(data, size, 0);
    if (document) return document;
    yyjson_read_flag flags = YYJSON_READ_ALLOW_COMMENTS |
        YYJSON_READ_ALLOW_TRAILING_COMMAS | YYJSON_READ_ALLOW_BOM;
    yyjson_read_err error = {0};
    document = yyjson_read_opts((char *)data, size, flags, NULL, &error);
    if (!document && error.pos < size && data[error.pos] == ']') {
        /* 修复手法: 解析器停在一个多余的 ']' 上, 后面紧跟 '}}' —— 这是"手工在
           大括号里多包了一层数组"的典型痕迹。把这两个字符替换成空格(长度不变,
           所以错误位置依然有效)再解析一次, 并且要求 FileReferences 确实存在,
           免得把一个完全不相干的文件也"修"成合法 JSON。 */
        size_t second = skip_space(data, size, error.pos + 1);
        size_t third = skip_space(data, size, second + 1);
        /* Some hand-edited Mver manifests have one extra '] }' pair. */
        if (second < size && third < size && data[second] == '}' && data[third] == '}') {
            char *copy = malloc(size);
            if (!copy) return NULL;
            memcpy(copy, data, size);
            copy[error.pos] = copy[second] = ' ';
            document = yyjson_read_opts(copy, size, flags, NULL, NULL);
            free(copy);
            yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
            if (!yyjson_is_obj(root) ||
                !yyjson_is_obj(yyjson_obj_get(root, "FileReferences"))) {
                yyjson_doc_free(document);
                document = NULL;
            }
        }
    }
    if (normalized) *normalized = document != NULL;
    return document;
}

/* 先看文件大小再分配, 不为一个巨大文件白白 malloc; 读取长度必须正好等于文件
   大小 —— 短读说明文件在读取过程中被改过, 这种内容不值得解析。 */
yyjson_doc *bongo_cat_model_json_read(const char *path, bool *normalized) {
    if (normalized) *normalized = false;
    uint64_t size;
    if (!bongo_cat_path_file_size(path, &size) || !size ||
        size > 4u * 1024u * 1024u) return NULL;
    char *data = malloc((size_t)size);
    if (!data) return NULL;
    FILE *file = bongo_cat_file_open(path, "rb");
    bool loaded = file && fread(data, 1, (size_t)size, file) == (size_t)size;
    if (file && fclose(file) != 0) loaded = false;
    yyjson_doc *document = loaded
        ? bongo_cat_model_json_parse(data, (size_t)size, normalized) : NULL;
    free(data);
    return document;
}
