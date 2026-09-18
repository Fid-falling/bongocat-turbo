#include "bongo_cat/image.h"

#include <string.h>

void bongo_cat_image_make_alpha_mask_progress(const BongoCatImage *image,
    BongoCatImageAlphaMask *mask, BongoCatImageProgress progress,
    void *userdata) {
    if (!mask) {
        if (progress) progress(userdata, 1.0f);
        return;
    }
    memset(mask, 0, sizeof(*mask));
    if (!image || !image->pixels || image->width <= 0 || image->height <= 0) {
        if (progress) progress(userdata, 1.0f);
        return;
    }
    mask->width = image->width < BONGO_CAT_ALPHA_MASK_SIZE ?
        image->width : BONGO_CAT_ALPHA_MASK_SIZE;
    mask->height = image->height < BONGO_CAT_ALPHA_MASK_SIZE ?
        image->height : BONGO_CAT_ALPHA_MASK_SIZE;
    for (int target_y = 0; target_y < mask->height; ++target_y) {
        int y_begin = (int)(((int64_t)target_y * image->height +
            mask->height - 1) / mask->height);
        int y_end = (int)(((int64_t)(target_y + 1) * image->height +
            mask->height - 1) / mask->height);
        for (int target_x = 0; target_x < mask->width; ++target_x) {
            int x_begin = (int)(((int64_t)target_x * image->width +
                mask->width - 1) / mask->width);
            int x_end = (int)(((int64_t)(target_x + 1) * image->width +
                mask->width - 1) / mask->width);
            unsigned char maximum = 0;
            for (int y = y_begin; y < y_end && maximum < 255; ++y) {
                const unsigned char *pixel = image->pixels +
                    ((size_t)y * image->width + x_begin) * 4 + 3;
                for (int x = x_begin; x < x_end; ++x, pixel += 4) {
                    if (*pixel > maximum) maximum = *pixel;
                    if (maximum == 255) break;
                }
            }
            mask->pixels[(size_t)target_y * mask->width + target_x] = maximum;
        }
        if (progress)
            progress(userdata, (float)(target_y + 1) / mask->height);
    }
}

void bongo_cat_image_make_alpha_mask(const BongoCatImage *image,
    BongoCatImageAlphaMask *mask) {
    bongo_cat_image_make_alpha_mask_progress(image, mask, NULL, NULL);
}
