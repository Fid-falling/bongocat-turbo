#include "model_import.h"
#include "model_storage.h"
#include "test_mver_support.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>

const char *test_mver_pointer_config(bool live2d);

int test_slim_package(void) {
    int failures = 0;
#define CHECK_SLIM(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
        __FILE__, __LINE__, #value); failures++; \
} } while (0)
    char *temporary = SDL_GetCurrentDirectory();
    CHECK_SLIM(temporary != NULL);
    if (!temporary) return failures;
    char source[BONGO_CAT_PATH_CAP], data[BONGO_CAT_PATH_CAP];
    char config[BONGO_CAT_PATH_CAP], slim[BONGO_CAT_PATH_CAP];
    unsigned long long nonce = (unsigned long long)SDL_GetTicksNS();
    snprintf(source, sizeof(source), "%s/bongocat-slim-%llu",
        temporary, nonce);
    snprintf(data, sizeof(data), "%s/bongocat-slim-data-%llu",
        temporary, nonce);
    CHECK_SLIM(mver_fixture(source));
    CHECK_SLIM(SDL_CreateDirectory(data));
    CHECK_SLIM(child(config, sizeof(config), source, "config.json", false));
    CHECK_SLIM(child(slim, sizeof(slim), source,
        BONGO_CAT_SKIN_CONFIG_FILE, false));
    CHECK_SLIM(SDL_RenamePath(config, slim));
    char compact[1400];
    snprintf(compact, sizeof(compact),
        "{\"schemaVersion\":1,\"kind\":\"bongocat-skin\","
        "\"sourceFormat\":\"bongo-cat-mver\",%s",
        test_mver_pointer_config(true) + 1);
    CHECK_SLIM(write_text(slim, compact));

    BongoCatImportReceipt receipt = {0};
    BongoCatError error = {0};
    char models_root[BONGO_CAT_PATH_CAP];
    CHECK_SLIM(child(models_root, sizeof(models_root), data, "models", true));
    CHECK_SLIM(bongo_cat_import_install(source, models_root, &receipt, &error) ==
        BONGO_CAT_OK);
    CHECK_SLIM(receipt.count == 1 && receipt.installed_count == 1);
    char installed[BONGO_CAT_PATH_CAP], models[BONGO_CAT_PATH_CAP];
    char package[BONGO_CAT_PATH_CAP];
    snprintf(models, sizeof(models), "%s", models_root);
    CHECK_SLIM(bongo_cat_path_join(package, sizeof(package), models,
        receipt.ids[0]));
    CHECK_SLIM(child(installed, sizeof(installed), package,
        BONGO_CAT_SKIN_CONFIG_FILE, false));
    CHECK_SLIM(bongo_cat_path_is_file(installed));
    CHECK_SLIM(bongo_cat_model_remove_tree(source, NULL));
    CHECK_SLIM(bongo_cat_model_remove_tree(data, NULL));
    SDL_free(temporary);
#undef CHECK_SLIM
    return failures;
}
