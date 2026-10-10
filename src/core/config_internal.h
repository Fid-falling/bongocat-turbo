#ifndef BONGO_CAT_CONFIG_INTERNAL_H
#define BONGO_CAT_CONFIG_INTERNAL_H

#include "bongo_cat/config.h"
#include <yyjson.h>

/*
 * core 配置模块的内部头: 只给同目录的 config_file.c / config_io.c /
 * config_session_io.c / config_fields.c 使用, 不对外导出。
 *
 * 契约:
 *   - read_* 系列返回 bool 表示"能不能继续读", 具体是哪个字段错了由
 *     BongoCatError 回传; 调用方用 && 串起来短路, 所以第一个失败点就是
 *     报告给用户的位置。
 *   - 键不存在时一律保留 target 里原有的默认值并返回 true(可选读),
 *     只有 read_int 的 required 参数能把它变成必读项。
 */
#define BONGO_CAT_SETTINGS_SCHEMA 1
#define BONGO_CAT_SESSION_SCHEMA 1
/* 配置/session 文件的硬上限: 超过就拒绝解析, 不把异常大的文件读进内存。 */
#define BONGO_CAT_CONFIG_FILE_CAP (1024u * 1024u)

BongoCatResult bongo_cat_config_read_document(const char *path,
    const char *format, int schema, yyjson_doc **document,
    BongoCatError *error);
BongoCatResult bongo_cat_config_write_document(const char *path,
    yyjson_mut_doc *document, const char *description, BongoCatError *error);
bool bongo_cat_config_type_error(const char *description,
    BongoCatError *error, const char *field, const char *expected);
bool bongo_cat_config_read_value(const char *description, yyjson_val *object,
    const char *key, yyjson_val **target, BongoCatError *error);
bool bongo_cat_config_read_object(const char *description,
    yyjson_val *parent, const char *key, yyjson_val **target,
    BongoCatError *error);
bool bongo_cat_config_read_array(const char *description,
    yyjson_val *parent, const char *key, yyjson_val **target,
    BongoCatError *error);
bool bongo_cat_config_read_bool(const char *description,
    yyjson_val *object, const char *key, bool *target, BongoCatError *error);
bool bongo_cat_config_read_int(const char *description,
    yyjson_val *object, const char *key, int *target, bool required,
    BongoCatError *error);
bool bongo_cat_config_read_float(const char *description,
    yyjson_val *object, const char *key, float *target, BongoCatError *error);
bool bongo_cat_config_read_string(const char *description,
    yyjson_val *object, const char *key, const char **target, size_t *length,
    BongoCatError *error);
bool bongo_cat_config_copy_text(const char *description, char *target,
    size_t capacity, const char *value, size_t length, const char *field,
    BongoCatError *error);
bool bongo_cat_config_read_text(const char *description,
    yyjson_val *object, const char *key, char *target, size_t capacity,
    BongoCatError *error);

#endif
