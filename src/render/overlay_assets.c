#include "overlay_internal.h"
#include "bongo_cat/image.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL_log.h>
#include <stdio.h>
#include <string.h>

/*
 * 把 overlay 的贴图与一切"跟着模型走"的元数据全部失效。它**不**碰 program/vao/vbo
 * (与模型无关的一次性对象, 见 overlay.c 的 destroy), 也**不**清 mver_pointer 子层
 * (那是子层自己的 clear) —— 换模型必须两件事都做, 只做一件会留下上一套模型的贴图
 * 或指针资源。也不动 multi_key/key_limit: 那两个来自设置, 与当前模型无关。
 *
 * sides[*].keys[*].texture 是**指向 cache 槽位内部**的指针而不是句柄拷贝: 上面的循环
 * 一释放槽位, 它们就变成了悬垂指针, 所以紧接着必须清空。这两步之间不允许插入任何会
 * 解引用它们的代码。清空按住列表是安全的: 换模型后上层会把仍然按着的键重新施加一遍
 * (见 app_state.c 的 active_inputs 与 bongo_cat_app_reapply_input)。
 */
void bongo_cat_overlay_clear_textures(BongoCatOverlay *value) {
    bongo_cat_texture_release(&value->background);
    bongo_cat_texture_release(&value->composite);
    value->clean_paws = false;
    value->composed_cover = false;
    value->composite_dirty = false;
    value->model_pointer_preferred = false;
    value->reference_width = value->reference_height = 0;
    for (size_t i = 0; i < BONGO_CAT_OVERLAY_CACHE_CAP; ++i) {
        bongo_cat_texture_release(&value->cache[i].texture);
        memset(&value->cache[i], 0, sizeof(value->cache[i]));
    }
    for (size_t side = 0; side < 2; ++side) {
        memset(value->sides[side].keys, 0, sizeof(value->sides[side].keys));
        value->sides[side].count = 0;
        /* 气泡素材是"跟着模型走"的元数据: 换模型必须重新探测 (见下面 side_bubble) */
        value->bubble_state[side] = 0;
        memset(value->bubble_rect[side], 0, sizeof(value->bubble_rect[side]));
        /* 单气泡合成图也一样: 它画的是上一套模型的键名文字, 留着就是错的内容 */
        bongo_cat_texture_release(&value->bubble_composite[side]);
        value->bubble_dirty[side] = false;
    }
    value->effect = NULL;
    value->left_path[0] = value->right_path[0] = '\0';
    value->effect_path[0] = '\0';
    value->background_path[0] = '\0';
}

/*
 * 装载一个模型的叠加层资源 (换模型走这里, 它会先把旧状态清干净)。不变量与坑:
 *
 *  - 背景图按构建分叉: Cubism 用 background.png (动画模型画在它前面, 这里只当
 *    底板), 非 Cubism 用 cover.png (没有模型渲染器, 整只猫就在这张图里)。
 *  - 背景缺失**不算失败**: 只记一条警告, 桌宠没有底板也还能画手和指针; 但通道/
 *    参数错误算失败。
 *  - Mver 指针层装载失败算**整体失败**: 那是模型元数据明确要求的功能, 静默降级会
 *    让用户以为"这个皮肤坏了"。失败路径要把已经加载好的背景释放掉, 不留半套资源。
 *  - model_pointer_preferred 为真 (模型自带指针) 时跳过 Mver 装载, 两者互斥。
 *  - reference_width/height 只有 Cubism 且启用 mver 投影时才从渲染选项取值,
 *    否则保持 0 —— 0 在绘制侧的语义是"不缩放, 铺满视口", 不是"尺寸未知的兜底"。
 *  - error 允许为 NULL, 此时错误信息落在本地变量里 (调用方明确表示不关心原因)。
 */
BongoCatResult bongo_cat_overlay_load(BongoCatOverlay *value,
    const char *directory, bool model_pointer_preferred,
    const BongoCatLive2DRenderOptions *render_options, BongoCatError *error) {
    if (!value || !directory) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Missing model overlay state or directory");
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    BongoCatError local = {0};
    BongoCatError *failure = error ? error : &local;
    *failure = (BongoCatError){0};
    char path[BONGO_CAT_PATH_CAP];
    /* Without the licensed Cubism runtime there is no model renderer.  Use the
       model's composed preview so the desktop pet remains visually complete;
       Cubism builds keep the background-only layer behind the animated model. */
#ifdef BONGO_CAT_HAS_CUBISM
    if (!bongo_cat_path_join(path, sizeof(path), directory,
        "resources/background.png")) {
        bongo_cat_error_set(failure, BONGO_CAT_ERROR_IO, "Model overlay path is too long");
        return BONGO_CAT_ERROR_IO;
    }
#else
    if (!bongo_cat_path_join(path, sizeof(path), directory,
        "resources/cover.png")) {
        bongo_cat_error_set(failure, BONGO_CAT_ERROR_IO, "Model overlay path is too long");
        return BONGO_CAT_ERROR_IO;
    }
#endif
    bongo_cat_overlay_clear_textures(value);
    bongo_cat_mver_pointer_overlay_clear(value->mver_pointer);
    BongoCatError background_error = {0};
    bool has_background = bongo_cat_path_is_file(path) &&
        bongo_cat_texture_from_file(&value->background, path, &background_error);
    BongoCatError pointer_error = {0};
    if (!model_pointer_preferred &&
        !bongo_cat_mver_pointer_overlay_load(value->mver_pointer, directory,
            &pointer_error)) {
        bongo_cat_texture_release(&value->background);
        *failure = pointer_error;
        if (!failure->message[0] && background_error.message[0])
            *failure = background_error;
        if (!failure->message[0]) bongo_cat_error_set(failure, BONGO_CAT_ERROR_IO,
            "Unable to load model overlay");
        return BONGO_CAT_ERROR_IO;
    }
    if (!has_background && background_error.message[0])
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", background_error.message);
    /* SFML sprites use original pixels at the top-left of the configured view.
       Their image size is not necessarily the size of that view. */
#ifdef BONGO_CAT_HAS_CUBISM
    if (render_options && render_options->mver_projection) {
        value->reference_width = render_options->reference_width;
        value->reference_height = render_options->reference_height;
    }
#else
    (void)render_options;
#endif
    value->model_pointer_preferred = model_pointer_preferred;
    value->composed_cover = false;
    value->clean_paws = false;
#ifndef BONGO_CAT_HAS_CUBISM
    value->composed_cover = true;
    value->clean_paws = true;
#endif
    snprintf(value->directory, sizeof(value->directory), "%s", directory);
    snprintf(value->background_path, sizeof(value->background_path), "%s", path);
    return BONGO_CAT_OK;
}

#ifdef BONGO_CAT_HAS_CUBISM
/*
 * 这个槽位是否正被某个按键或特效引用。被引用的槽位**永远不参与淘汰**: 引用方保存的是
 * 指向槽位内部的指针, 一旦被回收就成了悬垂指针 (这也是 cache 里最危险的一条)。
 * 多键显示之后引用方从"左右手各一个"变成"每侧最多 KEYS_CAP 个", 所以容量要严格大于
 * 2*KEYS_CAP+1 (见 overlay_internal.h 的 BONGO_CAT_OVERLAY_CACHE_CAP)。
 */
static bool slot_pinned(const BongoCatOverlay *value, const TextureSlot *slot) {
    for (size_t side = 0; side < 2; ++side)
        for (size_t i = 0; i < value->sides[side].count; ++i)
            if (value->sides[side].keys[i].texture == &slot->texture) return true;
    return value->effect == &slot->texture;
}

/*
 * 按路径取按键/特效贴图, 带 LRU 缓存 (只在 Cubism 构建里用: 非 Cubism 把贴图
 * 烧进 cover, 只需要哨兵标记)。不变量如下, 动这个函数前请逐条确认:
 *
 *  - 命中同一路径时返回**同一个句柄**并刷新使用时间: 多个键共用一张图只加载一次,
 *    代价是调用方不能各自释放它 (所有权在缓存, 释放由 clear/destroy 统一做)。
 *  - 被 sides[*].keys[*] 或 effect 引用着的槽位永远不参与淘汰 (见 slot_pinned)。
 *  - 淘汰顺序: 优先空槽, 否则 used 最小者; clock 单调递增, 不依赖真实时间或帧号。
 *  - 槽位全被钉住时返回 NULL: 宁可这一层不画, 也不能回收正在被引用的贴图。
 *  - 加载失败同样清空 path 并刷新 used, 否则坏路径会在每次查询时被反复重试。
 */
BongoCatTexture *bongo_cat_overlay_cached_texture(BongoCatOverlay *value,
    const char *path) {
    value->clock++;
    TextureSlot *oldest = NULL;
    for (size_t i = 0; i < BONGO_CAT_OVERLAY_CACHE_CAP; ++i) {
        TextureSlot *slot = &value->cache[i];
        if (bongo_cat_texture_ready(&slot->texture) &&
            strcmp(slot->path, path) == 0) {
            slot->used = value->clock;
            return &slot->texture;
        }
        if (bongo_cat_texture_ready(&slot->texture) && slot_pinned(value, slot))
            continue;
        if (!oldest || !bongo_cat_texture_ready(&slot->texture) ||
            slot->used < oldest->used) oldest = slot;
    }
    if (!oldest) return NULL;
    /* 后端无关加载: OpenGL 走 GL 上传, Vulkan 走 vk_backend 的纹理 */
    /* 淘汰旧句柄是安全的: Vulkan 侧会把它送进延迟销毁队列 (等引用它的帧栅栏过去),
       OpenGL 侧因为当前帧还没提交, 也不会有命令缓冲在用它。 */
    bongo_cat_texture_release(&oldest->texture);
    BongoCatError ignored = {0};
    if (!bongo_cat_texture_from_file(&oldest->texture, path, &ignored)) {
        oldest->path[0] = '\0';
        oldest->used = value->clock;
        return NULL;
    }
    snprintf(oldest->path, sizeof(oldest->path), "%s", path);
    oldest->used = value->clock;
    return &oldest->texture;
}
#endif

/*
 * 该侧按键图里是否自带"按键气泡"素材。BongoCat-X 的模型每键一张"气泡 + 手"整图, 气泡
 * 固定画在同一位置、只有里面的字母随键变化。
 *
 * 多键显示时第二个键起只画这块气泡、并逐个平移排开 —— 否则 N 个气泡完全重叠, 只看得见
 * 最上面那个, 用户看到的现象就是"同侧多键仍然只显示一个键"。这类模型的手本身是彩色的,
 * 所以颜色判据 ("高饱和像素") 选中的是手的描边而不是气泡, 必须按几何位置来取。
 *
 * 判据 (每侧只探测一次, 结果进 bubble_state):
 *  - 左上角这一块 (x < 40% 宽, y < 55% 高) 存在一块非透明像素;
 *  - 它与其余非透明像素在 x 上**分离** (中间有空白带): 内置模型的按键图是"键盘 + 手"的
 *    一整块, 左边没有孤立图形, 因此不会被误判成气泡;
 *  - 命中后按整图归一化 (取像素右下角 +1, 让矩形的右/下边界落在最后一个像素之后)。
 * 路径为空 (这个键没有对应贴图) 时保持"未探测", 下次按键再试; 读图失败或判据不成立则记
 * "确认没有"并不再重试 —— 每次按键都解码一张大图是不可接受的代价。
 */
bool bongo_cat_overlay_side_bubble(BongoCatOverlay *value, int side) {
    if (!value || (side != 0 && side != 1)) return false;
    if (value->bubble_state[side] != 0) return value->bubble_state[side] == 1;
    const char *path = side ? value->right_path : value->left_path;
    if (!path[0]) return false;
    value->bubble_state[side] = 2;
    BongoCatError ignored = {0};
    BongoCatImage image = {0};
    if (bongo_cat_image_load(path, &image, &ignored) != BONGO_CAT_OK) return false;
    const int width = image.width, height = image.height;
    const int left_width = width * 40 / 100, top_height = height * 55 / 100;
    int x0 = width, y0 = height, x1 = -1, y1 = -1, rest_x = width;
    for (int y = 0; y < height; ++y) {
        const unsigned char *row = image.pixels + (size_t)y * (size_t)width * 4;
        for (int x = 0; x < width; ++x) {
            if (row[x * 4 + 3] <= 1) continue;
            if (x < left_width && y < top_height) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            } else if (x < rest_x) rest_x = x;
        }
    }
    bongo_cat_image_free(&image);
    if (width <= 0 || height <= 0) return false;
    if (x1 - x0 < 8 || y1 - y0 < 8) return false;
    if (rest_x == width || rest_x <= x1 + 16) return false;
    value->bubble_rect[side][0] = (float)x0 / (float)width;
    value->bubble_rect[side][1] = (float)y0 / (float)height;
    value->bubble_rect[side][2] = (float)(x1 + 1) / (float)width;
    value->bubble_rect[side][3] = (float)(y1 + 1) / (float)height;
    value->bubble_state[side] = 1;
    return true;
}
