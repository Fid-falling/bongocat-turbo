/*
 * 模型封面 (cover.png) 的纹理缓存。
 *
 * 为什么要有缓存+预算: 模型页会一次铺开几十张卡片, 每张都要一张封面纹理。全部
 * 同步加载会让打开这一页卡住好几秒, 所以每帧只允许加载一张 (cover_load_budget),
 * 剩下的留到下一帧 —— 表现是封面逐张淡入, 而界面全程可交互。
 * 缓存键是 (app, 路径, 请求像素尺寸): 同一个模型在高 DPI 下请求的像素尺寸不同,
 * 尺寸对不上就得重建, 不能拿小图顶替 (会糊)。
 * 生命周期: covers_begin 开一帧 -> 卡片取图 -> covers_prune 回收本帧没被用到的。
 * prune 之前取过的图必须一直被"用过", 否则会被当垃圾回收掉 (封面闪烁)。
 */
#include "preferences_model_cover.h"
#include "preferences_internal.h"
#include "preferences_state.h"
#include "ui_backend.h"
#include "bongo_cat/image.h"
#include "bongo_cat/path.h"
#include "bongo_cat/preferences.h"

#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <string.h>

/*
 * 跨帧存活的缓存槽, 所以必须是静态数组而不是挂在 preferences 上: 客户端的 UI 后端
 * 重建时缓存要能一起作废 (见 cache_clear/abandon 的两种语义)。
 */
typedef struct ModelCoverSlot {
    BongoCatModelCover image;
    BongoCatApp *app;
    char path[BONGO_CAT_PATH_CAP];
    int requested_width;
    int requested_height;
    uint64_t generation;
} ModelCoverSlot;

static ModelCoverSlot cover_cache[BONGO_CAT_MODEL_CAP];
static uint64_t cover_generation;
static unsigned cover_load_budget;
static bool cover_load_deferred;

/*
 * 释放封面纹理。传 app 只清这个实例的, 传 NULL 清所有实例的 —— 两种用法分别对应
 * "关设置窗口"和"整个后端要销毁"。
 * 传 NULL 那一支不能再调 release_id: 那时后端已经不在了, 只能把槽位清零。
 */
void bongo_cat_preferences_model_cover_cache_clear(BongoCatApp *app) {
    for (size_t i = 0; i < BONGO_CAT_MODEL_CAP; ++i) {
        ModelCoverSlot *slot = &cover_cache[i];
        if ((!app || slot->app == app) && slot->image.texture) {
            if (slot->app && slot->app->preferences)
                bongo_cat_ui_texture_release_id(&slot->app->preferences->ui,
                    (int)slot->image.texture);
        }
        if (!app || slot->app == app) memset(slot, 0, sizeof(*slot));
    }
}

/*
 * 磁盘上的封面变了 (用户换图/重新导入) 时重载这一张。
 * 用"先建新的、再换指针、最后释放旧的"的顺序: 反过来的话中间任何一步失败都会让
 * 卡片出现一个空档。路径不在缓存里也算成功 (返回 true): 下次绘制自然会去加载它。
 */
bool bongo_cat_preferences_model_cover_reload(BongoCatApp *app,
    const char *path) {
    if (!app || !path || !path[0]) return false;
    for (size_t i = 0; i < BONGO_CAT_MODEL_CAP; ++i) {
        ModelCoverSlot *slot = &cover_cache[i];
        if (!slot->image.texture || slot->app != app ||
            strcmp(slot->path, path) != 0) continue;
        int width = 0, height = 0;
        BongoCatError ignored = {0};
        BongoCatUIBackend *ui = app->preferences ? &app->preferences->ui : NULL;
        unsigned int texture = ui ? (unsigned int)bongo_cat_ui_texture_from_file(ui,
            path, slot->requested_width, slot->requested_height, &width,
            &height, &ignored) : 0;
        if (!texture) {
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                "Model cover cache reload failed: %s", path);
            return false;
        }
        unsigned int previous = slot->image.texture;
        slot->image.texture = texture;
        slot->image.width = width;
        slot->image.height = height;
        slot->generation = cover_generation;
        if (app->preferences)
            bongo_cat_ui_texture_release_id(&app->preferences->ui, (int)previous);
        if (app->preferences)
            bongo_cat_preferences_invalidate(app->preferences);
        SDL_Log("Model cover cache reloaded: %s (%dx%d)", path,
            width, height);
        return true;
    }
    if (app->preferences)
        bongo_cat_preferences_invalidate(app->preferences);
    return true;
}

/*
 * 关设置窗口时的正常清理: 顺手把"删除确认框"也清掉, 它持有的是模型 id, 窗口没了
 * 再弹出来就是无主状态。
 */
void bongo_cat_preferences_model_cache_clear(BongoCatApp *app) {
    bongo_cat_preferences_remove_dialog_clear(app);
    bongo_cat_preferences_model_cover_cache_clear(app);
}

/* 上下文已不可用时的放弃版本: 只清槽位, 绝不碰后端 (对应 assets_abandon 的语义) */
void bongo_cat_preferences_model_cache_abandon(BongoCatApp *app) {
    bongo_cat_preferences_remove_dialog_clear(app);
    for (size_t i = 0; i < BONGO_CAT_MODEL_CAP; ++i)
        if (!app || cover_cache[i].app == app)
            memset(&cover_cache[i], 0, sizeof(cover_cache[i]));
}

/*
 * 每帧开头调用: 换一个 generation 并重置加载预算。
 * generation 是"本帧用过"的标记 —— 卡片取图时会把自己的 generation 刷成当前值,
 * prune 就回收那些还停留在旧 generation 的槽位 (说明这一帧没出现过, 比如滚出视野
 * 或换页了)。generation 回绕时跳过 0, 因为 0 是"未使用"的初值。
 */
void bongo_cat_preferences_model_covers_begin(BongoCatApp *app) {
    cover_generation++;
    if (!cover_generation) cover_generation++;
    cover_load_budget = 1;
    cover_load_deferred = false;
    (void)app;
}

/*
 * 取一张封面 (找不到就返回 NULL, 调用方画占位图)。
 * 命中条件必须同时满足路径相同且请求的像素尺寸相同: 尺寸不同说明 DPI 或布局变了,
 * 旧纹理得丢掉重建, 直接复用会糊或者被拉伸。
 * 尺寸不匹配时顺手把旧纹理释放掉并复用这个槽位, 免得白占一个格子。
 * "封面文件不存在"的日志做了去重 (last_missing): 这条路径每帧都会被问到, 不去重
 * 会把日志刷爆。
 * 预算用尽时置 cover_load_deferred, prune 会据此再要一帧 —— 这是"逐张加载"能
 * 继续下去的动力, 少了它封面会永远停在第一张。
 */
const BongoCatModelCover *bongo_cat_preferences_model_cover(
    BongoCatApp *app, const BongoCatModelEntry *entry,
    int pixel_width, int pixel_height) {
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), entry->adapter_directory,
        "resources/cover.png")) return NULL;
    if (!bongo_cat_path_is_file(path)) {
        static char last_missing[BONGO_CAT_PATH_CAP];
        if (strcmp(last_missing, path)) {
            snprintf(last_missing, sizeof(last_missing), "%s", path);
            SDL_Log("Model cover unavailable: id=%s path=%s",
                entry->id, path);
        }
        return NULL;
    }
    ModelCoverSlot *empty = NULL;
    for (size_t i = 0; i < BONGO_CAT_MODEL_CAP; ++i) {
        ModelCoverSlot *slot = &cover_cache[i];
        if (slot->image.texture && slot->app == app && !strcmp(slot->path, path)) {
            if (slot->requested_width == pixel_width &&
                slot->requested_height == pixel_height) {
                slot->generation = cover_generation; return &slot->image;
            }
            if (app->preferences)
                bongo_cat_ui_texture_release_id(&app->preferences->ui,
                    (int)slot->image.texture);
            memset(slot, 0, sizeof(*slot));
            empty = slot;
            break;
        }
        if (!slot->image.texture && !empty) empty = slot;
    }
    if (!empty) return NULL;
    if (!cover_load_budget) {
        cover_load_deferred = true;
        return NULL;
    }
    cover_load_budget--;
    BongoCatError ignored = {0};
    empty->image.texture = app->preferences ? (unsigned int)bongo_cat_ui_texture_from_file(
        &app->preferences->ui, path, pixel_width, pixel_height,
        &empty->image.width, &empty->image.height, &ignored) : 0;
    if (!empty->image.texture) return NULL;
    empty->app = app; empty->generation = cover_generation;
    empty->requested_width = pixel_width;
    empty->requested_height = pixel_height;
    snprintf(empty->path, sizeof(empty->path), "%s", path);
    return &empty->image;
}

/*
 * 每帧末尾调用, 回收本帧没被用到的封面。
 * 必须在所有卡片画完之后调 (页面里就是按这个顺序排的): 提前调会把这一帧还要用的
 * 图释放掉, 表现为封面一闪一闪。
 * 如果这一帧因为预算用尽而没能加载完, 这里补一次 invalidate —— 也就是说打开模型页
 * 时会连续要若干帧, 直到所有封面都加载好。
 */
void bongo_cat_preferences_model_covers_prune(BongoCatApp *app) {
    for (size_t i = 0; i < BONGO_CAT_MODEL_CAP; ++i) {
        ModelCoverSlot *slot = &cover_cache[i];
        if (slot->app != app || !slot->image.texture ||
            slot->generation == cover_generation) continue;
        if (app->preferences)
            bongo_cat_ui_texture_release_id(&app->preferences->ui,
                (int)slot->image.texture);
        memset(slot, 0, sizeof(*slot));
    }
    if (cover_load_deferred && app)
        bongo_cat_preferences_invalidate(app->preferences);
}
