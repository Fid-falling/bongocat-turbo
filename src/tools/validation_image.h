#ifndef BONGO_CAT_VALIDATION_IMAGE_H
#define BONGO_CAT_VALIDATION_IMAGE_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

typedef struct BongoCatValidationImage {
    int width;
    int height;
    unsigned char *bgra;
} BongoCatValidationImage;

typedef struct BongoCatValidationRect {
    int left;
    int top;
    int right;
    int bottom;
} BongoCatValidationRect;

int bongo_cat_validation_image_load(const wchar_t *path,
    BongoCatValidationImage *image);
int bongo_cat_validation_image_save(const wchar_t *path,
    const BongoCatValidationImage *image, int png);
void bongo_cat_validation_image_free(BongoCatValidationImage *image);
int bongo_cat_validation_image_normalize_file(const wchar_t *path,
    BongoCatValidationRect anchor, BongoCatValidationImage *output);
int bongo_cat_validation_image_draw_label(BongoCatValidationImage *image,
    const wchar_t *label, float x, float y);
int bongo_cat_validation_image_keep_largest(BongoCatValidationImage *image);
int bongo_cat_validation_image_bounds(const BongoCatValidationImage *image,
    BongoCatValidationRect *bounds);

#endif
