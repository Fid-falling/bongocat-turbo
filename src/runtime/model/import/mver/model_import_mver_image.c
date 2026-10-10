/*
 * Mver 按键贴图的像素合成：把键盘底图与手势图按非预乘 alpha 的 source-over 叠成一张 PNG，再按绑定需要的文件名复制出多个副本。
 * 位置：由 bongo_cat_import_mver_assets 在适配生成阶段逐行调用，左右修饰键共用同一张画面，输出直接落进适配目录的
 * resources/left-keys 与 right-keys，消费端只按文件名取图，不再做任何图像处理。
 * 契约：全程纯 CPU，不依赖 GL 上下文；失败返回 false 并尽量写 BongoCatError。易错点：画布尺寸取两张图的最大值，
 * 键盘底图按左上角对齐而不是居中，改对齐方式会让皮肤整体错位。
 */
#include "model_import_mver_internal.h"
#include "bongo_cat/file.h"
#include "bongo_cat/image.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stb_image_write.h>

/* PNG 写出回调的上下文：file 由 compose 负责关闭，ok 是粘滞失败标志，短写一次就置 false。 */
typedef struct PngWriter { FILE *file; bool ok; } PngWriter;

static void write_png(void *context, void *data, int size) {
    PngWriter *writer = context;
    if (writer->ok && fwrite(data, 1, (size_t)size, writer->file) != (size_t)size)
        writer->ok = false;
}

/* 把 hand 叠在 base 之上写成 target。原地复用 base 的像素缓冲当画布（仅当 base 已经是两者尺寸的最
   大值），否则新分配一块全透明缓冲并把 base 拷到左上角；两张输入图都在本函数内释放。 */
static bool compose(const char *base_path, const char *hand_path,
    const char *target, BongoCatError *error) {
    BongoCatImage base, hand;
    if (bongo_cat_image_load(base_path, &base, error) != BONGO_CAT_OK) return false;
    if (bongo_cat_image_load(hand_path, &hand, error) != BONGO_CAT_OK) {
        bongo_cat_image_free(&base); return false;
    }
    /* width、height 取两者最大值；两个上限分别保证 stbi 的行跨度 width * 4 与总字节数不溢出。 */
    int width = SDL_max(base.width, hand.width), height = SDL_max(base.height, hand.height);
    bool dimensions_valid = width > 0 && height > 0 && width <= INT_MAX / 4 &&
        (size_t)width <= SIZE_MAX / (size_t)height / 4;
    size_t bytes = dimensions_valid ? (size_t)width * (size_t)height * 4 : 0;
    bool in_place = dimensions_valid && width == base.width &&
        height == base.height;
    unsigned char *pixels = in_place ? base.pixels :
        (dimensions_valid ? malloc(bytes) : NULL);
    if (pixels && !in_place) {
        memset(pixels, 0, bytes);
        for (int y = 0; y < base.height; ++y)
            memcpy(pixels + (size_t)y * width * 4,
                base.pixels + (size_t)y * base.width * 4,
                (size_t)base.width * 4);
    }
    if (pixels) for (int y = 0; y < hand.height; ++y) for (int x = 0; x < hand.width; ++x) {
        unsigned char *dst = pixels + ((size_t)y * width + x) * 4;
        const unsigned char *src = hand.pixels + ((size_t)y * hand.width + x) * 4;
        /* 非预乘 alpha 的 source-over：alpha 用 0 到 255 * 255 的定点值表示，最后逐通道除回去并取整。 */
        unsigned alpha = src[3], inverse = 255 - alpha, destination_alpha = dst[3];
        unsigned output_alpha = alpha * 255 + destination_alpha * inverse;
        if (!output_alpha) { memset(dst, 0, 4); continue; }
        for (int channel = 0; channel < 3; ++channel) {
            unsigned color = src[channel] * alpha * 255 +
                dst[channel] * destination_alpha * inverse;
            dst[channel] = (unsigned char)((color + output_alpha / 2) / output_alpha);
        }
        dst[3] = (unsigned char)((output_alpha + 127) / 255);
    }
    FILE *file = pixels ? bongo_cat_file_open(target, "wb") : NULL;
    PngWriter writer = {file, file != NULL};
    bool buffer_ready = pixels != NULL;
    bool ok = file && stbi_write_png_to_func(write_png, &writer, width, height, 4,
        pixels, width * 4) && writer.ok;
    if (file && fclose(file) != 0) ok = false;
    if (!in_place) free(pixels);
    bongo_cat_image_free(&hand); bongo_cat_image_free(&base);
    if (!ok) bongo_cat_error_set(error, buffer_ready ? BONGO_CAT_ERROR_IO : BONGO_CAT_ERROR_MEMORY,
        "Cannot compose Mver input image: %s", target);
    return ok;
}

/* 为一个输入行产出贴图：names 里每个名字对应一张同名 PNG，第一个真正合成或复制，其余直接复制第一个
   （左右修饰键文件不同但画面相同）。keyboard 为 NULL 表示这一行没有键盘底图，直接复制手势图。
   返回 true 也包含 names.count 为 0 的"无事可做"；error 已有内容时不覆盖，避免盖掉更具体的诊断。 */
bool bongo_cat_mver_emit_pair(const char *hand, const char *keyboard,
    const char *directory, BongoCatMverKeyNames names, BongoCatError *error) {
    if (!names.count) return true;
    char first_name[32], first[BONGO_CAT_PATH_CAP];
    /* items 只在共享键名表命中时才有值，数字、字母、F 键与未收录键码都走 generated 缓冲区。 */
    const char *first_item = names.items[0] ? names.items[0] : names.generated;
    snprintf(first_name, sizeof(first_name), "%s.png", first_item);
    if (!bongo_cat_path_join(first, sizeof(first), directory, first_name)) return false;
    bool ok;
    if (keyboard) ok = compose(keyboard, hand, first, error);
    else {
        ok = bongo_cat_image_info(hand, NULL, NULL) &&
            bongo_cat_path_copy_file(hand, first);
    }
    if (!ok && error && !error->message[0])
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot copy Mver input image: %s", hand);
    /* 从第二个名字起只是复制首个文件：比重复合成便宜，也让同组贴图逐字节一致（绑定只认文件名）。 */
    for (size_t i = 1; ok && i < names.count; ++i) {
        char filename[32], target[BONGO_CAT_PATH_CAP];
        const char *item = names.items[i] ? names.items[i] : names.generated;
        snprintf(filename, sizeof(filename), "%s.png", item);
        ok = bongo_cat_path_join(target, sizeof(target), directory, filename) &&
            bongo_cat_path_copy_file(first, target);
    }
    return ok;
}
