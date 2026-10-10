#include "bongo_cat/sound_shortcut.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * "按住某几个键就触发声音 / 动作"的匹配逻辑。
 *
 * 两个输入来源: 键盘与鼠标用 SDL 风格的事件名(KeyA、LeftArrow), 手柄事件名
 * 统一加 "Gamepad:" 前缀。而用户在配置里写的是简短 token(A、Ctrl、Shift),
 * 所以匹配要经过 canonical / token_matches 归一化。
 */
static bool equal(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return false;
    }
    return *a == *b;
}

/* 把平台给的事件名折叠成用户习惯的写法: "KeyA" / "Num1" 去掉前缀, 旧的
   UpArrow / Return 映射到新名字。只折叠前缀后正好一个字符的键 —— "KeyCode"
   这类多字符名字要原样保留。 */
static const char *canonical(const char *key) {
    if ((strncmp(key, "Key", 3) == 0 || strncmp(key, "Num", 3) == 0) &&
        key[3] && !key[4]) return key + 3;
    if (strcmp(key, "UpArrow") == 0) return "ArrowUp";
    if (strcmp(key, "DownArrow") == 0) return "ArrowDown";
    if (strcmp(key, "LeftArrow") == 0) return "ArrowLeft";
    if (strcmp(key, "RightArrow") == 0) return "ArrowRight";
    if (strcmp(key, "Return") == 0) return "Enter";
    return key;
}

/* 用户写的 token 与实际按住的键名比较。修饰键和符号键有别名: Ctrl =
   ControlLeft/Right, Super = Command = Meta, "-" = Minus, "=" = Equal。
   别名表属于配置兼容性的一部分, 只能加不能改。 */
static bool token_matches(const char *token, const char *held) {
    if (equal(token, canonical(held))) return true;
    if (equal(token, "Control") || equal(token, "Ctrl"))
        return equal(held, "ControlLeft") || equal(held, "ControlRight");
    if (equal(token, "Shift"))
        return equal(held, "ShiftLeft") || equal(held, "ShiftRight");
    if (equal(token, "Alt")) return equal(held, "Alt") || equal(held, "AltGr");
    if (equal(token, "Super") || equal(token, "Command")) return equal(held, "Meta");
    if (equal(token, "-")) return equal(held, "Minus");
    if (equal(token, "=")) return equal(held, "Equal");
    return false;
}

/*
 * 维护"当前按住了哪些键"的集合。返回值表示集合是否变化, 调用方据此决定要不要
 * 重新检查所有快捷键。
 * 手柄按键必须加 "Gamepad:" 前缀, 否则手柄的 "South" 会和键盘按键撞名。
 * 集合用"与末位交换"的方式删除, 所以顺序不稳定, 但调用方不应该依赖顺序。
 */
bool bongo_cat_sound_shortcut_update(BongoCatSoundShortcutState *state,
    const BongoCatInputEvent *event) {
    if (!state || !event || !event->name[0]) return false;
    bool down;
    char name[BONGO_CAT_ID_CAP];
    if (event->kind == BONGO_CAT_INPUT_GAMEPAD_BUTTON) {
        down = event->value > 0.5f;
        if (strlen(event->name) + 8 >= sizeof(name)) return false;
        snprintf(name, sizeof(name), "Gamepad:%s", event->name);
    } else {
        if (event->kind != BONGO_CAT_INPUT_KEY_DOWN && event->kind != BONGO_CAT_INPUT_KEY_UP &&
            event->kind != BONGO_CAT_INPUT_MOUSE_DOWN && event->kind != BONGO_CAT_INPUT_MOUSE_UP)
            return false;
        down = event->kind == BONGO_CAT_INPUT_KEY_DOWN || event->kind == BONGO_CAT_INPUT_MOUSE_DOWN;
        snprintf(name, sizeof(name), "%s", event->name);
    }
    for (size_t i = 0; i < state->count; ++i) {
        if (strcmp(state->held[i], name) != 0) continue;
        if (down) return false;
        --state->count;
        if (i != state->count) memcpy(state->held[i], state->held[state->count], sizeof(name));
        return true;
    }
    if (!down || state->count >= BONGO_CAT_INPUT_KEY_STATE_CAP) return false;
    snprintf(state->held[state->count++], sizeof(name), "%s", name);
    return true;
}

/* shortcut 是 "Ctrl+Shift+A" 这种以 '+' 分段的字符串: 每一段都必须能在按住
   集合里找到才算命中(不要求顺序)。空段或超长段直接判失败, 免得把损坏的配置
   当成"命中一切"。 */
bool bongo_cat_sound_shortcut_down(const BongoCatSoundShortcutState *state,
    const char *shortcut) {
    if (!state || !shortcut || !*shortcut) return false;
    const char *cursor = shortcut;
    while (*cursor) {
        const char *plus = strchr(cursor, '+');
        size_t length = plus ? (size_t)(plus - cursor) : strlen(cursor);
        char token[BONGO_CAT_ID_CAP];
        if (!length || length >= sizeof(token)) return false;
        memcpy(token, cursor, length); token[length] = '\0';
        bool found = false;
        for (size_t i = 0; i < state->count && !found; ++i)
            found = token_matches(token, state->held[i]);
        if (!found) return false;
        if (!plus) return true;
        cursor = plus + 1;
    }
    return false;
}
