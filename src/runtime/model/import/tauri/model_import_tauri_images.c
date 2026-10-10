/*
 * 图像归一化: 把 Tauri 源包里名字各异的图片铺成 Mver 规范包要求的固定文件名。
 * 核心约定是“缺图不能缺文件”: 任何一层找不到源图就写一张 1x1 透明 PNG 占位, 让包的
 * 结构始终完整 —— 下游按固定文件名取图, 而且会重新扫描产物。
 * 源图既可能在包根的 resources/ 下, 也可能就在包根 (见 model_import_tauri_resources.c)。
 * 调用顺序有约束: copy_preview 必须早于 copy_runtime_images。
 */
#include "model_import_tauri_internal.h"

#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdio.h>

/* 1x1 灰度 + alpha 的透明 PNG, 内嵌字节以避免依赖任何编码器: 占位图必须能被
 * bongo_cat_image_info 解出尺寸, 所以不能写空文件。 */
static bool write_placeholder(const char *path) {
    static const unsigned char png[] = {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,
        0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x04,0x00,0x00,0x00,0xb5,0x1c,0x0c,0x02,0x00,0x00,0x00,
        0x0b,0x49,0x44,0x41,0x54,0x78,0xda,0x63,0x60,0x60,0x00,0x00,
        0x00,0x03,0x00,0x01,0x2b,0x09,0x4d,0x84,0x00,0x00,0x00,0x00,
        0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
    };
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) return false;
    bool ok = fwrite(png, 1, sizeof(png), file) == sizeof(png);
    if (fclose(file) != 0) ok = false;
    return ok;
}

/* 源图存在就复制, 否则写占位图 —— 因此 source 不存在也算成功。返回 false 只在写目标
 * 文件失败时发生, 并统一报 IO 错误。 */
bool bongo_cat_tauri_copy_image_or_placeholder(const char *source,
    const char *target, BongoCatError *error) {
    bool ok = source && bongo_cat_path_is_file(source)
        ? bongo_cat_path_copy_file(source, target) : write_placeholder(target);
    if (!ok && error) bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
        "Cannot create normalized Mver image: %s", target);
    return ok;
}

/* 两个参数语义不同: source_name 是要在源包里查找的资源名, fallback 则是已经算好的
 * 可用路径 (例如 standard 模式的背景先写成 mousebg.png, 再给别的图层兜底);
 * fallback 为 NULL 表示允许直接写占位图。 */
static bool copy_mver_image(const BongoCatImportCandidate *candidate,
    const char *mode_root, const char *source_name, const char *target_name,
    const char *fallback, BongoCatError *error) {
    char source[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
    const char *selected = fallback;
    if (source_name && bongo_cat_tauri_find_resource_file(candidate,
            source_name, source, sizeof(source))) selected = source;
    if (!bongo_cat_path_join(target, sizeof(target), mode_root, target_name))
        return false;
    return bongo_cat_tauri_copy_image_or_placeholder(selected, target, error);
}

/* 补齐 Mver 运行时需要的图层。前提: copy_preview 已经跑过 —— standard 分支拿它写出的
 * mousebg.png 当 tabletbg/l2dmousebg/l2dtabletbg 的兜底图源, 顺序反了会退化成占位图。 */
bool bongo_cat_tauri_copy_runtime_images(
    const BongoCatImportCandidate *candidate, const char *mode_root,
    BongoCatError *error) {
    char background[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(background, sizeof(background), mode_root,
            candidate->mode == BONGO_CAT_MODE_STANDARD ? "mousebg.png" :
                "bg.png")) return false;
    /* Tauri omits these Mver-only layers. Preserve real files when supplied;
       otherwise use transparent PNGs so the normalized package still loads. */
    if (candidate->mode == BONGO_CAT_MODE_STANDARD) {
        static const char *const backgrounds[] = {
            "tabletbg.png", "l2dmousebg.png", "l2dtabletbg.png"
        };
        static const char *const layers[] = {
            "arm.png", "up.png", "mouse.png", "mouse_left.png",
            "mouse_right.png", "mouse_side.png", "tablet.png",
            "tablet_left.png", "tablet_right.png"
        };
        for (size_t i = 0; i < sizeof(backgrounds) / sizeof(backgrounds[0]);
                ++i)
            if (!copy_mver_image(candidate, mode_root, backgrounds[i],
                    backgrounds[i], background, error)) return false;
        for (size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); ++i)
            if (!copy_mver_image(candidate, mode_root, layers[i], layers[i],
                    NULL, error)) return false;
        return true;
    }
    static const char *const idle_hands[] = {
        "lefthand/leftup.png", "righthand/rightup.png"
    };
    for (size_t i = 0; i < sizeof(idle_hands) / sizeof(idle_hands[0]); ++i)
        if (!copy_mver_image(candidate, mode_root, idle_hands[i],
                idle_hands[i], NULL, error)) return false;
    if (candidate->mode != BONGO_CAT_MODE_GAMEPAD) return true;
    static const char *const gamepad[] = {
        "arm_L.png", "arm_R.png", "left_stick.png", "left_stick_down.png",
        "right_stick.png", "right_stick_down.png"
    };
    for (size_t i = 0; i < sizeof(gamepad) / sizeof(gamepad[0]); ++i)
        if (!copy_mver_image(candidate, mode_root, gamepad[i], gamepad[i],
                NULL, error)) return false;
    return true;
}

/* 预览资源: cover.png 归一化成 cat.png (预览封面), background.png 归一化成
 * mousebg.png / bg.png (按模式当背景); 两个源图都允许缺失, 缺失时写占位图。 */

bool bongo_cat_tauri_copy_preview(const BongoCatImportCandidate *candidate,
    const char *mode_root, BongoCatError *error) {
    char source[BONGO_CAT_PATH_CAP], cat[BONGO_CAT_PATH_CAP];
    char target[BONGO_CAT_PATH_CAP];
    bool have_cover = bongo_cat_tauri_find_resource_file(candidate,
        "cover.png", source, sizeof(source));
    if (!bongo_cat_path_join(cat, sizeof(cat), mode_root, "cat.png") ||
        !bongo_cat_tauri_copy_image_or_placeholder(
            have_cover ? source : NULL, cat, error)) return false;
    bool have_background = bongo_cat_tauri_find_resource_file(candidate,
        "background.png", source, sizeof(source));
    const char *background_name = candidate->mode == BONGO_CAT_MODE_STANDARD
        ? "mousebg.png" : "bg.png";
    if (!bongo_cat_path_join(target, sizeof(target), mode_root,
            background_name)) return false;
    return bongo_cat_tauri_copy_image_or_placeholder(
        have_background ? source : NULL, target, error);
}
