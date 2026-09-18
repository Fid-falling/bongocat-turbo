#include "bongo_cat/file.h"
#include "bongo_cat/image.h"

#include <stb_image.h>

bool bongo_cat_image_info(const char *path, int *width, int *height) {
    if (width) *width = 0;
    if (height) *height = 0;
    FILE *file = path ? bongo_cat_file_open(path, "rb") : NULL;
    int image_width = 0, image_height = 0, channels = 0;
    bool known = file && stbi_info_from_file(file, &image_width,
        &image_height, &channels) && image_width > 0 && image_height > 0 &&
        channels > 0;
    if (file) fclose(file);
    if (known && width) *width = image_width;
    if (known && height) *height = image_height;
    return known;
}
