#include "config_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

bool bongo_cat_config_type_error(const char *description,
    BongoCatError *error, const char *field, const char *expected) {
    bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
        "%s field '%s' must be %s", description, field, expected);
    return false;
}

bool bongo_cat_config_read_value(const char *description, yyjson_val *object,
    const char *key, yyjson_val **target, BongoCatError *error) {
    *target = NULL;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
    yyjson_val *name;
    while ((name = yyjson_obj_iter_next(&iterator))) {
        if (!yyjson_equals_str(name, key)) continue;
        if (*target) return bongo_cat_config_type_error(
            description, error, key, "present only once");
        *target = yyjson_obj_iter_get_val(name);
    }
    return true;
}

bool bongo_cat_config_read_object(const char *description,
    yyjson_val *parent, const char *key, yyjson_val **target,
    BongoCatError *error) {
    yyjson_val *value;
    if (!bongo_cat_config_read_value(
            description, parent, key, &value, error)) return false;
    *target = NULL;
    if (!value) return true;
    if (!yyjson_is_obj(value)) return bongo_cat_config_type_error(
        description, error, key, "an object");
    *target = value;
    return true;
}

bool bongo_cat_config_read_array(const char *description,
    yyjson_val *parent, const char *key, yyjson_val **target,
    BongoCatError *error) {
    yyjson_val *value;
    if (!bongo_cat_config_read_value(
            description, parent, key, &value, error)) return false;
    *target = NULL;
    if (!value) return true;
    if (!yyjson_is_arr(value)) return bongo_cat_config_type_error(
        description, error, key, "an array");
    *target = value;
    return true;
}

bool bongo_cat_config_read_bool(const char *description,
    yyjson_val *object, const char *key, bool *target, BongoCatError *error) {
    yyjson_val *value;
    if (!bongo_cat_config_read_value(
            description, object, key, &value, error)) return false;
    if (!value) return true;
    if (!yyjson_is_bool(value)) return bongo_cat_config_type_error(
        description, error, key, "a boolean");
    *target = yyjson_get_bool(value);
    return true;
}

static bool integer_value(yyjson_val *value, int *target) {
    if (yyjson_is_sint(value)) {
        int64_t number = yyjson_get_sint(value);
        if (number >= INT_MIN && number <= INT_MAX) {
            *target = (int)number;
            return true;
        }
    } else if (yyjson_is_uint(value)) {
        uint64_t number = yyjson_get_uint(value);
        if (number <= INT_MAX) {
            *target = (int)number;
            return true;
        }
    }
    return false;
}

bool bongo_cat_config_read_int(const char *description,
    yyjson_val *object, const char *key, int *target, bool required,
    BongoCatError *error) {
    yyjson_val *value;
    if (!bongo_cat_config_read_value(
            description, object, key, &value, error)) return false;
    if (!value && !required) return true;
    if (!value || !integer_value(value, target))
        return bongo_cat_config_type_error(description, error, key,
            "an integer in the supported range");
    return true;
}

bool bongo_cat_config_read_float(const char *description,
    yyjson_val *object, const char *key, float *target, BongoCatError *error) {
    yyjson_val *value;
    if (!bongo_cat_config_read_value(
            description, object, key, &value, error)) return false;
    if (!value) return true;
    if (!yyjson_is_num(value)) return bongo_cat_config_type_error(
        description, error, key, "a number");
    double number = yyjson_get_num(value);
    if (!isfinite(number) || number < -FLT_MAX || number > FLT_MAX)
        return bongo_cat_config_type_error(description, error, key,
            "a finite number in the supported range");
    *target = (float)number;
    return true;
}

bool bongo_cat_config_read_string(const char *description,
    yyjson_val *object, const char *key, const char **target, size_t *length,
    BongoCatError *error) {
    yyjson_val *value;
    *target = NULL;
    *length = 0;
    if (!bongo_cat_config_read_value(
            description, object, key, &value, error)) return false;
    if (!value) return true;
    if (!yyjson_is_str(value)) return bongo_cat_config_type_error(
        description, error, key, "a string");
    const char *text = yyjson_get_str(value);
    size_t text_length = yyjson_get_len(value);
    if (strlen(text) != text_length)
        return bongo_cat_config_type_error(description, error, key,
            "a string without embedded nulls");
    *target = text;
    *length = text_length;
    return true;
}

bool bongo_cat_config_copy_text(const char *description, char *target,
    size_t capacity, const char *value, size_t length, const char *field,
    BongoCatError *error) {
    if (!value) return true;
    if (length >= capacity) return bongo_cat_config_type_error(
        description, error, field, "a shorter string");
    memset(target, 0, capacity);
    memcpy(target, value, length);
    return true;
}

bool bongo_cat_config_read_text(const char *description,
    yyjson_val *object, const char *key, char *target, size_t capacity,
    BongoCatError *error) {
    const char *value;
    size_t length;
    return bongo_cat_config_read_string(description, object, key, &value,
            &length, error) &&
        bongo_cat_config_copy_text(description, target, capacity, value,
            length, key, error);
}
