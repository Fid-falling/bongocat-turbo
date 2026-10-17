#include "bongo_cat/sound.h"
#include "bongo_cat/path.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

/* Windows 路径大小写不敏感, 而模型目录与音效路径都是由 path_join 从同一个根拼出来的;
   逐个字符比较 (只折叠 ASCII) 既覆盖了盘符/目录名大小写差异, 又不会误伤 UTF-8 字节。 */
static bool path_prefix(const char *path, const char *directory,
    size_t *offset) {
    size_t length = strlen(directory);
    if (!length || strncmp(path, directory, length) == 0 &&
        (path[length] == '\0' || path[length] == '/' || path[length] == '\\')) {
        *offset = path[length] == '\0' ? length : length + 1;
        return true;
    }
    for (size_t i = 0; i < length; ++i)
        if (!path[i] || ascii_lower(path[i]) != ascii_lower(directory[i]))
            return false;
    if (path[length] == '\0') {
        *offset = length;
        return true;
    }
    if (path[length] != '/' && path[length] != '\\') return false;
    *offset = length + 1;
    return true;
}

bool bongo_cat_sound_id(char *output, size_t capacity,
    const char *model_id, const char *model_directory, const char *path) {
    if (!output || !capacity) return false;
    output[0] = '\0';
    if (!path || !path[0]) return false;
    const char *relative = path;
    size_t offset = 0;
    if (model_directory && model_directory[0] &&
        path_prefix(path, model_directory, &offset))
        relative = path + offset;
    else
        relative = bongo_cat_path_name(path);
    /* 目录本身以分隔符结尾、或 join 产生连续分隔符时, 相对路径可能以 '/' 开头。
       id 里不允许出现这种前导分隔符 —— 它没有语义, 只会让同一文件出现两种写法。 */
    while (relative[0] == '/' || relative[0] == '\\') relative++;
    if (!relative[0]) return false;
    int written = model_id && model_id[0]
        ? snprintf(output, capacity, "%s:%s", model_id, relative)
        : snprintf(output, capacity, "%s", relative);
    if (written < 0 || (size_t)written >= capacity) {
        output[0] = '\0';
        return false;
    }
    /* id 要跨平台稳定: 同一份配置在 Windows/Linux 之间来回应是同一个字符串。 */
    for (size_t i = 0; output[i]; ++i)
        if (output[i] == '\\') output[i] = '/';
    return true;
}

bool bongo_cat_settings_sound_disabled(const BongoCatSettings *settings,
    const char *id) {
    if (!settings || !id || !id[0]) return false;
    size_t count = settings->sound.disabled_count;
    if (count > BONGO_CAT_SOUND_DISABLED_CAP)
        count = BONGO_CAT_SOUND_DISABLED_CAP;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(settings->sound.disabled[i], id)) return true;
    return false;
}

bool bongo_cat_settings_set_sound_disabled(BongoCatSettings *settings,
    const char *id, bool disabled) {
    if (!settings || !id || !id[0] || strlen(id) >= BONGO_CAT_SOUND_ID_CAP)
        return false;
    size_t count = settings->sound.disabled_count;
    if (count > BONGO_CAT_SOUND_DISABLED_CAP)
        count = BONGO_CAT_SOUND_DISABLED_CAP;
    size_t found = count;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(settings->sound.disabled[i], id)) {
            found = i;
            break;
        }
    if (disabled) {
        if (found < count) return true;
        if (count >= BONGO_CAT_SOUND_DISABLED_CAP) return false;
        memset(settings->sound.disabled[count], 0, BONGO_CAT_SOUND_ID_CAP);
        snprintf(settings->sound.disabled[count], BONGO_CAT_SOUND_ID_CAP,
            "%s", id);
        settings->sound.disabled_count = count + 1;
        return true;
    }
    if (found >= count) return true;
    if (found + 1 < count)
        memmove(settings->sound.disabled[found],
            settings->sound.disabled[found + 1],
            (count - found - 1) * BONGO_CAT_SOUND_ID_CAP);
    memset(settings->sound.disabled[count - 1], 0, BONGO_CAT_SOUND_ID_CAP);
    settings->sound.disabled_count = count - 1;
    return true;
}

int bongo_cat_settings_sound_decision(const BongoCatSettings *settings,
    const char *id) {
    if (!settings) return BONGO_CAT_SOUND_DECISION_ALLOWED;
    if (settings->sound.muted) return BONGO_CAT_SOUND_DECISION_MUTED;
    return bongo_cat_settings_sound_disabled(settings, id)
        ? BONGO_CAT_SOUND_DECISION_DISABLED
        : BONGO_CAT_SOUND_DECISION_ALLOWED;
}

int bongo_cat_settings_sound_volume(const BongoCatSettings *settings) {
    if (!settings) return BONGO_CAT_SOUND_VOLUME_DEFAULT;
    int volume = settings->sound.volume;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    return volume;
}
