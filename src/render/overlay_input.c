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
static bool key_path(BongoCatOverlay *value, const char *group, const char *name,
    char path[BONGO_CAT_PATH_CAP]) {
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

/*
 * 按键状态入口。返回值: 1 = 右手, 0 = 左手, -1 = 这个键没有对应贴图或参数无效 ——
 * 上层据此判断"这个按键要不要算作模型有反应", 所以不要为了省事把 -1 改成 0。
 *
 * 两条构建路径的差别是这里最容易踩的坑:
 *  - Cubism: 真的去取/缓存贴图, left/right 指向缓存槽位里的句柄;
 *  - 非 Cubism: 贴图在 cover 合成阶段烧进背景, 这里只存一个**哨兵值**
 *    (BongoCatTexture *)1 当作"该手处于按下状态"的标记。哨兵不是可解引用指针,
 *    任何新代码都不能对它调用纹理接口 —— 现有的读取方都只做 NULL 判断。
 *
 * 释放按键时只在"当前记录的确实是这个键"时才清 (strcmp): 同一个键的按下/抬起可能
 * 因为丢焦而乱序到达, 无条件清会把后来按下的另一个键一起清掉。
 */
int bongo_cat_overlay_key(BongoCatOverlay *value, const char *name, bool pressed) {
    if (!value || !name) return -1;
    bool right;
    char path[BONGO_CAT_PATH_CAP];
    if (key_path(value, "right-keys", name, path)) right = true;
    else if (key_path(value, "left-keys", name, path)) right = false;
    else return -1;
    char *active_name = right ? value->right_name : value->left_name;
    BongoCatTexture **active = right ? &value->right : &value->left;
    char *active_path = right ? value->right_path : value->left_path;
    if (pressed) {
        snprintf(active_name, BONGO_CAT_ID_CAP, "%s", name);
#ifdef BONGO_CAT_HAS_CUBISM
        *active = bongo_cat_overlay_cached_texture(value, path);
#else
        snprintf(active_path, BONGO_CAT_PATH_CAP, "%s", path);
        /* 非 Cubism 构建把左右手合成进一张图, 这里只需要一个"有效"标记 */
        *active = (BongoCatTexture *)1;
#endif
    } else if (strcmp(active_name, name) == 0) {
        active_name[0] = '\0';
        *active = NULL;
        active_path[0] = '\0';
    }
#ifndef BONGO_CAT_HAS_CUBISM
    value->composite_dirty = true;
#endif
    return right ? 1 : 0;
}

/* 只判断指针非空: 非 Cubism 构建下这里是非 NULL 的哨兵值, 也算"手在按下状态" */
bool bongo_cat_overlay_hand_active(const BongoCatOverlay *value, bool right) {
    return value && (right ? value->right : value->left) != NULL;
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
