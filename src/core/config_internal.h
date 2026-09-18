#ifndef BONGO_CAT_CONFIG_INTERNAL_H
#define BONGO_CAT_CONFIG_INTERNAL_H

#include "bongo_cat/config.h"
#include <yyjson.h>

#define BONGO_CAT_SETTINGS_SCHEMA 1
#define BONGO_CAT_SESSION_SCHEMA 1
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
