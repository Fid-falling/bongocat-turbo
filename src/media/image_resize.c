#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#include "bongo_cat/gl_api.h"
#include "bongo_cat/image.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static float rounded_distance(float x, float y, float width, float height,
    float radius) {
    radius = SDL_clamp(radius, 0.0f, SDL_min(width, height) * .5f);
    float qx = fabsf(x - width * .5f) - (width * .5f - radius);
    float qy = fabsf(y - height * .5f) - (height * .5f - radius);
    float ox = SDL_max(qx, 0.0f), oy = SDL_max(qy, 0.0f);
    return sqrtf(ox * ox + oy * oy) + SDL_min(SDL_max(qx, qy), 0.0f) - radius;
}

static void apply_rounding(unsigned char *pixels, int width, int height,
    float radius) {
    if (radius <= 0.0f) return;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float distance = rounded_distance(x + .5f, y + .5f,
                (float)width, (float)height, radius);
            float coverage = SDL_clamp(.5f - distance, 0.0f, 1.0f);
            unsigned char *alpha = pixels + ((size_t)y * width + x) * 4 + 3;
            *alpha = (unsigned char)(*alpha * coverage + .5f);
        }
    }
}

static GLuint upload(const unsigned char *pixels, int width, int height,
    BongoCatError *error) {
    bongo_cat_gl_clear_errors();
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
        GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    GLenum status = glGetError();
    if (status == GL_NO_ERROR) return texture;
    if (texture) glDeleteTextures(1, &texture);
    bongo_cat_error_set(error, status == GL_OUT_OF_MEMORY
        ? BONGO_CAT_ERROR_MEMORY : BONGO_CAT_ERROR_PLATFORM,
        "OpenGL resampled texture upload failed (0x%x)", (unsigned)status);
    return 0;
}

unsigned int bongo_cat_image_texture_resampled(const char *path,
    int max_width, int max_height, float rounding, int *width, int *height,
    BongoCatError *error) {
    if (!path || max_width < 1 || max_height < 1)
        return 0;
    BongoCatImage source;
    if (bongo_cat_image_load(path, &source, error) != BONGO_CAT_OK)
        return 0;
    float scale = SDL_min((float)max_width / source.width,
        (float)max_height / source.height);
    int target_width = SDL_max(1, (int)lroundf(source.width * scale));
    int target_height = SDL_max(1, (int)lroundf(source.height * scale));
    size_t count = (size_t)target_width * target_height;
    unsigned char *pixels = count <= SIZE_MAX / 4 ? malloc(count * 4) : NULL;
    if (!pixels || !stbir_resize_uint8_srgb(source.pixels,
        source.width, source.height, source.width * 4, pixels,
        target_width, target_height, target_width * 4, STBIR_RGBA)) {
        free(pixels);
        bongo_cat_image_free(&source);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot resize image: %s", path);
        return 0;
    }
    bongo_cat_image_free(&source);
    apply_rounding(pixels, target_width, target_height,
        rounding * SDL_min((float)target_width / max_width,
            (float)target_height / max_height));
    GLuint texture = upload(pixels, target_width, target_height, error);
    free(pixels);
    if (width) *width = target_width;
    if (height) *height = target_height;
    return texture;
}
