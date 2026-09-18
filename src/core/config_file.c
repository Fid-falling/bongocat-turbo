#include "config_internal.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "windows_utf8.h"
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static yyjson_val *unique_member(yyjson_val *object, const char *name,
    bool *duplicate) {
    yyjson_val *found = NULL;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iterator))) {
        if (!yyjson_equals_str(key, name)) continue;
        if (found) {
            *duplicate = true;
            return NULL;
        }
        found = yyjson_obj_iter_get_val(key);
    }
    return found;
}

BongoCatResult bongo_cat_config_read_document(const char *path,
    const char *format, int schema, yyjson_doc **document,
    BongoCatError *error) {
    if (!path || !format || !document) return BONGO_CAT_ERROR_ARGUMENT;
    *document = NULL;
    uint64_t file_size = 0;
    if (!bongo_cat_path_file_size(path, &file_size)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot inspect configuration file: %s", path);
        return BONGO_CAT_ERROR_IO;
    }
    if (!file_size || file_size > BONGO_CAT_CONFIG_FILE_CAP) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Configuration file size must be between 1 and %u bytes",
            (unsigned)BONGO_CAT_CONFIG_FILE_CAP);
        return BONGO_CAT_ERROR_FORMAT;
    }
    yyjson_read_err json_error = {0};
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot open configuration file: %s", path);
        return BONGO_CAT_ERROR_IO;
    }
    yyjson_doc *parsed = yyjson_read_fp(file, 0, NULL, &json_error);
    if (fclose(file) != 0) {
        yyjson_doc_free(parsed);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot finish reading configuration file: %s", path);
        return BONGO_CAT_ERROR_IO;
    }
    if (!parsed) {
        BongoCatResult result = json_error.code ==
            YYJSON_READ_ERROR_MEMORY_ALLOCATION ? BONGO_CAT_ERROR_MEMORY :
            json_error.code == YYJSON_READ_ERROR_FILE_OPEN ||
            json_error.code == YYJSON_READ_ERROR_FILE_READ ?
                BONGO_CAT_ERROR_IO : BONGO_CAT_ERROR_FORMAT;
        bongo_cat_error_set(error, result,
            result == BONGO_CAT_ERROR_FORMAT ?
                "Invalid configuration JSON: %s" :
                "Cannot read configuration file: %s",
            json_error.msg ? json_error.msg : path);
        return result;
    }
    yyjson_val *root = yyjson_doc_get_root(parsed);
    bool duplicate = false;
    yyjson_val *format_value = yyjson_is_obj(root) ?
        unique_member(root, "format", &duplicate) : NULL;
    yyjson_val *version = yyjson_is_obj(root) ?
        unique_member(root, "schemaVersion", &duplicate) : NULL;
    bool schema_matches = yyjson_is_sint(version) ?
        yyjson_get_sint(version) == schema : yyjson_is_uint(version) &&
        yyjson_get_uint(version) == (uint64_t)schema;
    if (!yyjson_is_obj(root) || !yyjson_is_str(format_value) ||
        yyjson_get_len(format_value) != strlen(format) ||
        strcmp(yyjson_get_str(format_value), format) != 0 ||
        !schema_matches || duplicate) {
        yyjson_doc_free(parsed);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Unsupported configuration format; expected %s schema %d",
            format, schema);
        return BONGO_CAT_ERROR_FORMAT;
    }
    *document = parsed;
    return BONGO_CAT_OK;
}

static bool sync_file(const char *path) {
#ifdef _WIN32
    wchar_t *wide = bongo_cat_windows_wide(path);
    HANDLE file = wide ? CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    free(wide);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool result = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return result;
#else
    int file = open(path, O_RDONLY);
    if (file < 0) return false;
    bool result = fsync(file) == 0;
    close(file);
    return result;
#endif
}

#ifndef _WIN32
static bool sync_parent(const char *path) {
    char directory[BONGO_CAT_PATH_CAP];
    int length = snprintf(directory, sizeof(directory), "%s", path);
    if (length < 0 || (size_t)length >= sizeof(directory)) return false;
    char *slash = strrchr(directory, '/');
    if (!slash) snprintf(directory, sizeof(directory), ".");
    else if (slash == directory) slash[1] = '\0';
    else *slash = '\0';
    int handle = open(directory, O_RDONLY);
    if (handle < 0) return false;
    bool result = fsync(handle) == 0;
    close(handle);
    return result;
}
#endif

BongoCatResult bongo_cat_config_write_document(const char *path,
    yyjson_mut_doc *document, const char *description, BongoCatError *error) {
    char temporary[BONGO_CAT_PATH_CAP + 8];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Configuration path is too long");
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    yyjson_write_err json_error = {0};
    FILE *file = bongo_cat_file_open(temporary, "wb");
    bool written = file && yyjson_mut_write_fp(file, document,
        YYJSON_WRITE_PRETTY, NULL, &json_error);
    if (file && fclose(file) != 0) written = false;
    if (!written) {
        bongo_cat_file_remove(temporary);
        BongoCatResult result = json_error.code ==
            YYJSON_WRITE_ERROR_MEMORY_ALLOCATION ? BONGO_CAT_ERROR_MEMORY :
            json_error.code == YYJSON_WRITE_ERROR_INVALID_PARAMETER ||
            json_error.code == YYJSON_WRITE_ERROR_INVALID_VALUE_TYPE ||
            json_error.code == YYJSON_WRITE_ERROR_NAN_OR_INF ||
            json_error.code == YYJSON_WRITE_ERROR_INVALID_STRING ?
                BONGO_CAT_ERROR_FORMAT : BONGO_CAT_ERROR_IO;
        bongo_cat_error_set(error, result, "Cannot write %s: %s",
            description, json_error.msg ? json_error.msg : "cannot open file");
        return result;
    }
    if (!sync_file(temporary)) {
        bongo_cat_file_remove(temporary);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot flush %s", description);
        return BONGO_CAT_ERROR_IO;
    }
    if (!bongo_cat_file_replace(temporary, path, true)) {
        bongo_cat_file_remove(temporary);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot replace %s", description);
        return BONGO_CAT_ERROR_IO;
    }
#ifndef _WIN32
    if (!sync_parent(path)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot flush configuration directory");
        return BONGO_CAT_ERROR_IO;
    }
#endif
    return BONGO_CAT_OK;
}
