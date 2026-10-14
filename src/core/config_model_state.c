#include "bongo_cat/config.h"
#include "bongo_cat/utf8.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * 模型级别的用户设置(自定义显示名 / 已删除的模型), 对应 settings.json 里的
 * modelOverrides 与 removedModels。
 *
 * 两个数组都是定长(CAP)紧凑列表: 顺序即写入顺序, 删除靠 memmove 前移并把
 * 尾巴清零, 保证磁盘上不会出现空条目。所有 set_* 都先调用 validate 兜底, 因此
 * 即使配置结构被外部改坏也不会越界写。
 */
const char *bongo_cat_settings_model_label(const BongoCatSettings *config,
    const char *id) {
    if (!config || !id) return NULL;
    size_t count = config->model_label_count;
    if (count > BONGO_CAT_MODEL_CAP) count = BONGO_CAT_MODEL_CAP;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(config->model_labels[i].id, id))
            return config->model_labels[i].label;
    return NULL;
}

/* label 为空 = 删除这条自定义名; 返回 false 表示"没有任何变化"而不是出错,
   调用方据此避免无意义地重写配置文件。 */
bool bongo_cat_settings_set_model_label(BongoCatSettings *config,
    const char *id, const char *label) {
    if (!config || !id || !id[0] || strlen(id) >= BONGO_CAT_ID_CAP ||
        !bongo_cat_utf8_valid(id)) return false;
    bongo_cat_settings_validate(config);
    size_t index = config->model_label_count;
    for (size_t i = 0; i < config->model_label_count; ++i)
        if (!strcmp(config->model_labels[i].id, id)) {
            index = i;
            break;
        }
    if (!label || !label[0]) {
        if (index == config->model_label_count) return false;
        if (index + 1 < config->model_label_count)
            memmove(&config->model_labels[index],
                &config->model_labels[index + 1],
                (config->model_label_count - index - 1) *
                sizeof(config->model_labels[0]));
        config->model_label_count--;
        memset(&config->model_labels[config->model_label_count], 0,
            sizeof(config->model_labels[0]));
        return true;
    }
    if (strlen(label) >= BONGO_CAT_ID_CAP || !bongo_cat_utf8_valid(label))
        return false;
    if (index < config->model_label_count) {
        if (!strcmp(config->model_labels[index].label, label)) return false;
        memset(config->model_labels[index].label, 0,
            sizeof(config->model_labels[index].label));
        snprintf(config->model_labels[index].label,
            sizeof(config->model_labels[index].label), "%s", label);
        return true;
    }
    if (config->model_label_count >= BONGO_CAT_MODEL_CAP) return false;
    BongoCatModelLabel *value =
        &config->model_labels[config->model_label_count++];
    memset(value, 0, sizeof(*value));
    snprintf(value->id, sizeof(value->id), "%s", id);
    snprintf(value->label, sizeof(value->label), "%s", label);
    return true;
}

bool bongo_cat_settings_model_removed(const BongoCatSettings *config,
    const char *id) {
    if (!config || !id) return false;
    size_t count = config->removed_model_count;
    if (count > BONGO_CAT_MODEL_CAP) count = BONGO_CAT_MODEL_CAP;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(config->removed_models[i].id, id)) return true;
    return false;
}

bool bongo_cat_settings_set_model_removed(BongoCatSettings *config,
    const char *id, bool removed) {
    if (!config || !id || !id[0] || strlen(id) >= BONGO_CAT_ID_CAP ||
        !bongo_cat_utf8_valid(id)) return false;
    bongo_cat_settings_validate(config);
    size_t index = config->removed_model_count;
    for (size_t i = 0; i < config->removed_model_count; ++i)
        if (!strcmp(config->removed_models[i].id, id)) {
            index = i;
            break;
        }
    if (removed) {
        if (index < config->removed_model_count) return false;
        if (config->removed_model_count >= BONGO_CAT_MODEL_CAP) return false;
        BongoCatRemovedModel *entry =
            &config->removed_models[config->removed_model_count++];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->id, sizeof(entry->id), "%s", id);
        return true;
    }
    if (index == config->removed_model_count) return false;
    if (index + 1 < config->removed_model_count)
        memmove(&config->removed_models[index],
            &config->removed_models[index + 1],
            (config->removed_model_count - index - 1) *
            sizeof(config->removed_models[0]));
    config->removed_model_count--;
    memset(&config->removed_models[config->removed_model_count], 0,
        sizeof(config->removed_models[0]));
    return true;
}

bool bongo_cat_settings_model_hidden(const BongoCatSettings *config,
    const char *id) {
    if (!config || !id) return false;
    size_t count = config->hidden_model_count;
    if (count > BONGO_CAT_MODEL_CAP) count = BONGO_CAT_MODEL_CAP;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(config->hidden_models[i].id, id)) return true;
    return false;
}

bool bongo_cat_settings_set_model_hidden(BongoCatSettings *config,
    const char *id, bool hidden) {
    if (!config || !id || !id[0] || strlen(id) >= BONGO_CAT_ID_CAP ||
        !bongo_cat_utf8_valid(id)) return false;
    bongo_cat_settings_validate(config);
    size_t index = config->hidden_model_count;
    for (size_t i = 0; i < config->hidden_model_count; ++i)
        if (!strcmp(config->hidden_models[i].id, id)) {
            index = i;
            break;
        }
    if (hidden) {
        if (index < config->hidden_model_count) return false;
        if (config->hidden_model_count >= BONGO_CAT_MODEL_CAP) return false;
        BongoCatRemovedModel *entry =
            &config->hidden_models[config->hidden_model_count++];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->id, sizeof(entry->id), "%s", id);
        return true;
    }
    if (index == config->hidden_model_count) return false;
    if (index + 1 < config->hidden_model_count)
        memmove(&config->hidden_models[index],
            &config->hidden_models[index + 1],
            (config->hidden_model_count - index - 1) *
            sizeof(config->hidden_models[0]));
    config->hidden_model_count--;
    memset(&config->hidden_models[config->hidden_model_count], 0,
        sizeof(config->hidden_models[0]));
    return true;
}

bool bongo_cat_settings_random_disabled(const BongoCatSettings *config,
    const char *behavior_id) {
    if (!config || !behavior_id) return false;
    size_t count = config->random_disabled_count;
    if (count > BONGO_CAT_RANDOM_DISABLED_CAP)
        count = BONGO_CAT_RANDOM_DISABLED_CAP;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(config->random_disabled[i], behavior_id)) return true;
    return false;
}

bool bongo_cat_settings_set_random_disabled(BongoCatSettings *config,
    const char *behavior_id, bool disabled) {
    if (!config || !behavior_id || !behavior_id[0] ||
        strlen(behavior_id) >= BONGO_CAT_BEHAVIOR_ID_CAP ||
        !bongo_cat_utf8_valid(behavior_id)) return false;
    bongo_cat_settings_validate(config);
    if (disabled) {
        if (bongo_cat_settings_random_disabled(config, behavior_id))
            return false;
        if (config->random_disabled_count >= BONGO_CAT_RANDOM_DISABLED_CAP)
            return false;
        char *entry = config->random_disabled[config->random_disabled_count++];
        memset(entry, 0, BONGO_CAT_BEHAVIOR_ID_CAP);
        snprintf(entry, BONGO_CAT_BEHAVIOR_ID_CAP, "%s", behavior_id);
        return true;
    }
    size_t index = config->random_disabled_count;
    for (size_t i = 0; i < config->random_disabled_count; ++i)
        if (!strcmp(config->random_disabled[i], behavior_id)) {
            index = i;
            break;
        }
    if (index == config->random_disabled_count) return false;
    if (index + 1 < config->random_disabled_count)
        memmove(&config->random_disabled[index],
            &config->random_disabled[index + 1],
            (config->random_disabled_count - index - 1) *
            sizeof(config->random_disabled[0]));
    config->random_disabled_count--;
    memset(config->random_disabled[config->random_disabled_count], 0,
        BONGO_CAT_BEHAVIOR_ID_CAP);
    return true;
}

/* 模型 id 有两种形态: "<package>" 表示包本身, "<package>~<序号>" 表示包内
   的单条模型。恢复整个包时这两类条目都要清掉。 */
static bool package_model_id(const char *id, const char *package_id) {
    size_t length = package_id ? strlen(package_id) : 0;
    if (!id || !length || strncmp(id, package_id, length) != 0) return false;
    const char *suffix = id + length;
    if (!suffix[0]) return true;
    if (*suffix++ != '~' || !*suffix) return false;
    while (*suffix)
        if (!isdigit((unsigned char)*suffix++)) return false;
    return true;
}

/* 双指针原地压缩(一边扫描一边前移), 一趟完成过滤; 返回值表示是否真的删掉了
   条目, 调用方据此决定要不要落盘。 */
bool bongo_cat_settings_restore_model_package(BongoCatSettings *config,
    const char *package_id) {
    if (!config || !package_id || !package_id[0]) return false;
    bongo_cat_settings_validate(config);
    size_t output = 0;
    bool changed = false;
    for (size_t i = 0; i < config->removed_model_count; ++i) {
        BongoCatRemovedModel entry = config->removed_models[i];
        if (package_model_id(entry.id, package_id)) {
            changed = true;
            continue;
        }
        config->removed_models[output++] = entry;
    }
    memset(&config->removed_models[output], 0,
        (BONGO_CAT_MODEL_CAP - output) * sizeof(config->removed_models[0]));
    config->removed_model_count = output;
    return changed;
}
