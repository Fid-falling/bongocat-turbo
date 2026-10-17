#include "overlay_internal.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

/*
 * 按键贴图路径解析, 顺带决定这个键算左手还是右手。
 * 先找 right-keys 再找 left-keys: 两个目录都有同名贴图时按右手处理, 这个先后顺序
 * 就是"左右手"的判定依据, 调换会让所有同名键左右互换。
 * F1..F9 额外回落到 Fn.png: 皮肤作者通常不会为每个功能键单独出图, 只给一张通用图,
 * 没有这个回落到所有 F 键都会静默失效。判定要求 name[1] 是数字, 所以单字符 "F"
 * 与 "Foo" 之类不会被误判。
 */
static bool key_path(const BongoCatOverlay *value, const char *group,
    const char *name, char path[BONGO_CAT_PATH_CAP]) {
    char relative[BONGO_CAT_PATH_CAP];
    snprintf(relative, sizeof(relative), "resources/%s/%s.png", group, name);
    bongo_cat_path_join(path, BONGO_CAT_PATH_CAP, value->directory, relative);
    if (bongo_cat_path_is_file(path)) return true;
    if (name[0] == 'F' && name[1] >= '0' && name[1] <= '9') {
        snprintf(relative, sizeof(relative), "resources/%s/Fn.png", group);
        bongo_cat_path_join(path, BONGO_CAT_PATH_CAP, value->directory, relative);
        return bongo_cat_path_is_file(path);
    }
    return false;
}

/* 该键在这一侧列表里的下标; 没记过时返回 count, 调用方据此判断"这是个新键"。 */
static size_t side_index(const OverlaySide *hand, const char *name) {
    for (size_t i = 0; i < hand->count; ++i)
        if (strcmp(hand->keys[i].name, name) == 0) return i;
    return hand->count;
}

/* 对外的路径查询: 单气泡合成要拿到"某个键自己那张图"来抠键名文字, 用的是同一套查找规则,
   否则合成出来的字与单独按这个键时看到的可能来自不同文件 (例如 F 键的 Fn.png 回落)。 */
bool bongo_cat_overlay_key_path(const BongoCatOverlay *value, int side,
    const char *name, char path[BONGO_CAT_PATH_CAP]) {
    if (!value || (side != 0 && side != 1) || !name) return false;
    return key_path(value, side ? "right-keys" : "left-keys", name, path);
}

/*
 * 该侧这一帧最多画几个键。两种情况恒为 1:
 *  - 设置里关掉了多键显示 —— 退回"每侧只画最早按下的那个键";
 *  - 非 Cubism 构建 —— 那里没有独立按键贴图 (手是 CPU 合成进 cover 的), 而合成图只吃
 *    左右各一张 (见 overlay_draw.c 的 refresh_composite), 多键没有落脚点。
 */
int bongo_cat_overlay_display_limit(const BongoCatOverlay *value) {
    if (!value || !value->multi_key) return 1;
#ifndef BONGO_CAT_HAS_CUBISM
    return 1;
#else
    return value->key_limit > 0 ? value->key_limit : 1;
#endif
}

/*
 * 重新装载某一侧"显示窗口内"的按键贴图, 并同步 left_path/right_path。
 * 不变量: 超出显示数量的键只保留名字、贴图为 NULL —— 它们的贴图一旦装了就没人画, 只会
 * 白占 LRU 槽位; 松开前面的键之后它们补位进来, 在这里被重新装载。
 * 副作用是"两个后端一致": 非 Cubism 构建里贴图列是哨兵值, 真正的显示交给合成层。
 */
void bongo_cat_overlay_refresh_keys(BongoCatOverlay *value, int side) {
    if (!value || (side != 0 && side != 1)) return;
    OverlaySide *hand = &value->sides[side];
    char *side_path = side ? value->right_path : value->left_path;
    /* 这一侧的按键集合刚刚变过: 单气泡里的 "A + S" 必须重新拼 (见 overlay_assets.c)。
       放在这里而不是合成函数内部判断, 是因为"集合变了"只有输入层知道。 */
    value->bubble_dirty[side] = true;
    if (hand->count == 0) {
        side_path[0] = '\0';
        return;
    }
    const int limit = bongo_cat_overlay_display_limit(value);
    for (size_t i = 0; i < hand->count; ++i) {
        OverlayKey *key = &hand->keys[i];
        key->texture = NULL;
        if ((int)i >= limit) continue;
        char path[BONGO_CAT_PATH_CAP];
        if (!key_path(value, side ? "right-keys" : "left-keys", key->name, path))
            continue;
        if (i == 0) snprintf(side_path, BONGO_CAT_PATH_CAP, "%s", path);
#ifdef BONGO_CAT_HAS_CUBISM
        key->texture = bongo_cat_overlay_cached_texture(value, path);
#else
        /* 非 Cubism 构建把左右手合成进一张图, 这里只需要一个"有效"标记 */
        key->texture = (BongoCatTexture *)1;
#endif
    }
}

/*
 * 按键状态入口。返回值: 1 = 右手, 0 = 左手, -1 = 这个键没有对应贴图或参数无效 ——
 * 上层据此判断"这个按键要不要算作模型有反应", 所以不要为了省事把 -1 改成 0。
 *
 * 同一侧按下的键按**按下顺序**追加进该手列表, 不再互相覆盖 —— 这是"同侧多键只看得见
 * 最后一个"的修复点。松开时按名字删除, 而不是清空列表头: 同一个键的按下/抬起可能因为
 * 丢焦而乱序到达, 无条件清会把后来按下的另一个键一起清掉。
 */
int bongo_cat_overlay_key(BongoCatOverlay *value, const char *name, bool pressed) {
    if (!value || !name) return -1;
    bool right;
    char path[BONGO_CAT_PATH_CAP];
    if (key_path(value, "right-keys", name, path)) right = true;
    else if (key_path(value, "left-keys", name, path)) right = false;
    else return -1;
    OverlaySide *hand = &value->sides[right ? 1 : 0];
    size_t index = side_index(hand, name);
    if (pressed) {
        if (index == hand->count) {
            /* 单手按住的键数超过上限 (正常用法到不了): 不记这个键, 但对外仍算"有反应" */
            if (hand->count >= BONGO_CAT_OVERLAY_HELD_CAP) return right ? 1 : 0;
            index = hand->count++;
            snprintf(hand->keys[index].name, BONGO_CAT_ID_CAP, "%s", name);
            hand->keys[index].texture = NULL;
        }
    } else {
        if (index == hand->count) return right ? 1 : 0;
        if (index + 1 < hand->count)
            memmove(&hand->keys[index], &hand->keys[index + 1],
                (hand->count - index - 1) * sizeof(hand->keys[0]));
        memset(&hand->keys[hand->count - 1], 0, sizeof(hand->keys[0]));
        hand->count--;
    }
    bongo_cat_overlay_refresh_keys(value, right ? 1 : 0);
#ifndef BONGO_CAT_HAS_CUBISM
    value->composite_dirty = true;
#endif
    return right ? 1 : 0;
}

/* 该手是否按着键 (非 Cubism 构建下贴图列是哨兵值, 所以只判数量、不判句柄) */
bool bongo_cat_overlay_hand_active(const BongoCatOverlay *value, bool right) {
    return value && value->sides[right ? 1 : 0].count > 0;
}

/*
 * 设置/清除按键特效贴图。path 为空是"清除特效"的**正常用法**, 返回 true (而不是
 * 错误); 只有路径存在但文件读不出来才算失败。与按键一样, 非 Cubism 构建存的是
 * 哨兵值而不是真句柄。
 */
bool bongo_cat_overlay_effect(BongoCatOverlay *value, const char *path) {
    if (!value) return false;
    value->effect = NULL;
    value->effect_path[0] = '\0';
    if (!path || !*path) return true;
    if (!bongo_cat_path_is_file(path)) return false;
#ifdef BONGO_CAT_HAS_CUBISM
    value->effect = bongo_cat_overlay_cached_texture(value, path);
#else
    value->effect = (BongoCatTexture *)1;
#endif
    snprintf(value->effect_path, sizeof(value->effect_path), "%s", path);
    return value->effect != NULL;
}
