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

typedef struct PngWriter { FILE *file; bool ok; } PngWriter;

static void write_png(void *context, void *data, int size) {
    PngWriter *writer = context;
    if (writer->ok && fwrite(data, 1, (size_t)size, writer->file) != (size_t)size)
        writer->ok = false;
}

static bool compose(const char *base_path, const char *hand_path,
    const char *target, BongoCatError *error) {
    BongoCatImage base, hand;
    if (bongo_cat_image_load(base_path, &base, error) != BONGO_CAT_OK) return false;
    if (bongo_cat_image_load(hand_path, &hand, error) != BONGO_CAT_OK) {
        bongo_cat_image_free(&base); return false;
    }
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

bool bongo_cat_mver_emit_pair(const char *hand, const char *keyboard,
    const char *directory, BongoCatMverKeyNames names, BongoCatError *error) {
    if (!names.count) return true;
    char first_name[32], first[BONGO_CAT_PATH_CAP];
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
    for (size_t i = 1; ok && i < names.count; ++i) {
        char filename[32], target[BONGO_CAT_PATH_CAP];
        const char *item = names.items[i] ? names.items[i] : names.generated;
        snprintf(filename, sizeof(filename), "%s.png", item);
        ok = bongo_cat_path_join(target, sizeof(target), directory, filename) &&
            bongo_cat_path_copy_file(first, target);
    }
    return ok;
}
