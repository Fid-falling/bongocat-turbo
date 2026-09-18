#include "test_mver_support.h"

#include "bongo_cat/common.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

const char *test_mver_pointer_config(bool live2d);
bool test_mver_pointer_fixture_assets(const char *standard,
    const char *source);

bool write_text(const char *path, const char *text) {
    FILE *file = bongo_cat_file_open(path, "wb");
    bool ok = file && fwrite(text, 1, strlen(text), file) == strlen(text);
    if (file && fclose(file) != 0) ok = false;
    return ok;
}

bool child(char *output, size_t capacity, const char *root,
    const char *name, bool directory) {
    return bongo_cat_path_join(output, capacity, root, name) &&
        (!directory || SDL_CreateDirectory(output));
}

bool mver_fixture(const char *root) {
    char image[BONGO_CAT_PATH_CAP], standard[BONGO_CAT_PATH_CAP];
    char hand[BONGO_CAT_PATH_CAP], model[BONGO_CAT_PATH_CAP];
    char path[BONGO_CAT_PATH_CAP], source[BONGO_CAT_PATH_CAP];
    if (!SDL_CreateDirectory(root) ||
        !child(image, sizeof(image), root, "img", true) ||
        !child(standard, sizeof(standard), image, "standard", true) ||
        !child(hand, sizeof(hand), standard, "hand", true) ||
        !child(model, sizeof(model), standard, "cat_model", true))
        return false;
    if (!child(path, sizeof(path), root, "config.json", false) ||
        !write_text(path, test_mver_pointer_config(true))) return false;
    if (!child(path, sizeof(path), model, "cat.model3.json", false) ||
        !write_text(path, "{\"Version\":3,\"FileReferences\":"
            "{\"Moc\":\"cat.moc3\",\"Textures\":[\"texture.png\"]}}") ||
        !child(path, sizeof(path), model, "cat.moc3", false) ||
        !write_text(path, "MOC3")) return false;
    snprintf(source, sizeof(source), "%s/resources/assets/bongocat.png",
        BONGO_CAT_NATIVE_SOURCE_DIR);
    if (!child(path, sizeof(path), model, "texture.png", false) ||
        !SDL_CopyFile(source, path) ||
        !child(path, sizeof(path), hand, "0.png", false) ||
        !SDL_CopyFile(source, path) ||
        !child(path, sizeof(path), standard, "bg.png", false) ||
        !SDL_CopyFile(source, path)) return false;
    return test_mver_pointer_fixture_assets(standard, source);
}
