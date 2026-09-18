#include "bongo_cat/json.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdio.h>

#define BONGO_CAT_JSON_FILE_CAP (16u * 1024u * 1024u)

yyjson_doc *bongo_cat_json_read_file(const char *path,
    yyjson_read_flag flags, yyjson_read_err *error) {
    uint64_t size;
    if (!bongo_cat_path_file_size(path, &size) ||
        size == 0 || size > BONGO_CAT_JSON_FILE_CAP) return NULL;
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) return NULL;
    yyjson_doc *document = yyjson_read_fp(file, flags, NULL, error);
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
    if (!ok) bongo_cat_path_remove(path);
    return ok;
}
