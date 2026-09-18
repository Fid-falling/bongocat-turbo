#include "runtime.h"

#include <stdlib.h>

int bongo_cat_app_run(int argc, char **argv) {
    if (bongo_cat_platform_update_shutdown_argument(argc, argv)) return 0;
    bool secondary = bongo_cat_multi_pet_secondary_argument(argc, argv);
    if (!secondary && !bongo_cat_platform_single_instance_begin()) return 0;
    BongoCatApp *app = calloc(1, sizeof(*app));
    if (!app) {
        BongoCatError memory = {0};
        bongo_cat_error_set(&memory, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate application state");
        bongo_cat_startup_failure(NULL, &memory);
        if (!secondary) bongo_cat_platform_single_instance_end();
        return 1;
    }
    BongoCatError error = {0};
    if (!bongo_cat_app_initialize(app, argc, argv, &error)) {
        bongo_cat_startup_failure(app, &error);
        if (app->smoke) bongo_cat_startup_ci_failure(app, &error);
        bongo_cat_app_shutdown(app, "shutdown:startup-failure", 1);
        free(app);
        if (!secondary) bongo_cat_platform_single_instance_end();
        return 1;
    }
    bongo_cat_app_loop(app);
    int exit_code = app->exit_code;
    bongo_cat_app_shutdown(app, "shutdown:normal", exit_code);
    free(app);
    if (!secondary) bongo_cat_platform_single_instance_end();
    return exit_code;
}
