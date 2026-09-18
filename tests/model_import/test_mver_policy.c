#include "model_import.h"
#include "model_storage.h"
#include "test_mver_support.h"

#include <SDL3/SDL.h>
#include <stdio.h>

static int failures;
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #value); \
    failures++; \
} } while (0)

int test_mver_policy(void) {
    failures = 0;
    char *temporary = SDL_GetCurrentDirectory();
    CHECK(temporary != NULL);
    if (!temporary) return failures;
    static const struct {
        const char *name;
        BongoCatModelMode mode;
    } fixtures[] = {
        {"standard", BONGO_CAT_MODE_STANDARD},
        {"keyboard", BONGO_CAT_MODE_KEYBOARD},
        {"gamepad", BONGO_CAT_MODE_GAMEPAD}
    };
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        BongoCatImportCandidate candidate = {0};
        snprintf(candidate.directory, sizeof(candidate.directory),
            "%s/resources/assets/models/%s", BONGO_CAT_NATIVE_SOURCE_DIR,
            fixtures[i].name);
        snprintf(candidate.assets, sizeof(candidate.assets), "%s",
            candidate.directory);
        snprintf(candidate.package_root, sizeof(candidate.package_root), "%s",
            candidate.directory);
        snprintf(candidate.setting, sizeof(candidate.setting),
            "cat.model3.json");
        candidate.mode = fixtures[i].mode;
        candidate.format = BONGO_CAT_IMPORT_MVER;
        char digest[65];
        bool placeholder = false;
        BongoCatError error = {0};
        CHECK(bongo_cat_import_candidate_inspect(&candidate, digest,
            &placeholder, &error) && placeholder);
    }

    char source[BONGO_CAT_PATH_CAP], custom[BONGO_CAT_PATH_CAP];
    char texture[BONGO_CAT_PATH_CAP], replacement[BONGO_CAT_PATH_CAP];
    snprintf(source, sizeof(source), "%s/resources/assets/models/keyboard",
        BONGO_CAT_NATIVE_SOURCE_DIR);
    snprintf(custom, sizeof(custom), "%s/bongocat-retextured-stock-%llu",
        temporary, (unsigned long long)SDL_GetTicksNS());
    BongoCatError error = {0};
    CHECK(bongo_cat_model_copy_directory(source, custom, &error) == BONGO_CAT_OK);
    CHECK(child(texture, sizeof(texture), custom,
        "demomodel2.1024/texture_00.png", false));
    snprintf(replacement, sizeof(replacement),
        "%s/resources/assets/bongocat.png", BONGO_CAT_NATIVE_SOURCE_DIR);
    CHECK(SDL_CopyFile(replacement, texture));
    BongoCatImportCandidate candidate = {0};
    snprintf(candidate.directory, sizeof(candidate.directory), "%s", custom);
    snprintf(candidate.assets, sizeof(candidate.assets), "%s", custom);
    snprintf(candidate.package_root, sizeof(candidate.package_root), "%s",
        custom);
    snprintf(candidate.setting, sizeof(candidate.setting), "cat.model3.json");
    candidate.mode = BONGO_CAT_MODE_KEYBOARD;
    candidate.format = BONGO_CAT_IMPORT_MVER;
    char digest[65];
    bool placeholder = true;
    CHECK(bongo_cat_import_candidate_inspect(&candidate, digest, &placeholder,
        &error) && !placeholder);
    CHECK(bongo_cat_model_remove_tree(custom, NULL));
    SDL_free(temporary);
    return failures;
}
