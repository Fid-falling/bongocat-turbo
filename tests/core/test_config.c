#include "test.h"
#include "test_config_validation.h"
#include "bongo_cat/config.h"
#include "bongo_cat/file.h"
#include "bongo_cat/model.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

static void write_text(const char *path, const char *text) {
    FILE *file = bongo_cat_file_open(path, "wb");
    CHECK(file != NULL);
    if (file) { CHECK(fputs(text, file) >= 0); CHECK(fclose(file) == 0); }
}

static bool contains_text(const char *path, const char *needle) {
    char content[8192] = {0};
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) return false;
    size_t length = fread(content, 1, sizeof(content) - 1, file);
    fclose(file);
    content[length] = '\0';
    return strstr(content, needle) != NULL;
}

void test_config(void) {
    test_config_validation();
    BongoCatLanguage language;
    CHECK(!strcmp(bongo_cat_language_name(BONGO_CAT_LANG_ZH_HANT),
        "zh-Hant"));
    CHECK(bongo_cat_language_parse("zh-Hant", &language) &&
        language == BONGO_CAT_LANG_ZH_HANT);
    CHECK(bongo_cat_language_parse("zh-TW", &language) &&
        language == BONGO_CAT_LANG_ZH_HANT);
    CHECK(!bongo_cat_language_parse("zh-Hans", &language));
    const uint32_t colors[] = {0x00ff00, 0x0000ff, 0xff0000, 0xff00ff};
    for (int i = 0; i < BONGO_CAT_OBS_BACKGROUND_COLOR_COUNT; ++i)
        CHECK(bongo_cat_obs_background_color_rgb(i) == colors[i]);

    static BongoCatSettings settings;
    static BongoCatSessionState session;
    bongo_cat_settings_defaults(&settings);
    bongo_cat_session_defaults(&session);
    CHECK(!settings.window.pass_through);
    settings.model.max_fps = 30;
    settings.model.multiple_pets = true;
    settings.model.mirror = true;
    settings.model.mouse_centered = false;
    settings.window.pass_through = true;
    settings.window.obs_background = true;
    settings.window.random_expression = true;
    settings.window.random_expression_interval_seconds = 12.0f;
    settings.window.obs_background_color = BONGO_CAT_OBS_BACKGROUND_BLUE;
    settings.app.language = BONGO_CAT_LANG_ZH_CN;
    memcpy(settings.extensions_json, "{\"example\":{\"enabled\":true}}",
        sizeof("{\"example\":{\"enabled\":true}}"));
    settings.behavior_shortcut_count = 1;
    memcpy(settings.behavior_shortcuts[0].id, "model:motion:Tap:0",
        sizeof("model:motion:Tap:0"));
    memcpy(settings.behavior_shortcuts[0].shortcut, "Control+1",
        sizeof("Control+1"));
    memcpy(settings.behavior_shortcuts[0].label, "Happy tap",
        sizeof("Happy tap"));
    CHECK(bongo_cat_settings_set_model_label(&settings, "model", "Display"));
    CHECK(bongo_cat_settings_set_model_removed(&settings, "model~2", true));
    CHECK(bongo_cat_settings_set_model_removed(&settings, "model-extra", true));
    session.window.x = -321;
    session.window.position_known = true;
    session.window.opacity_percent = 75.0f;
    session.window.width = 700;
    session.window.height = 500;
    session.window.content_width = 612;
    session.window.content_height = 354;
    memcpy(session.active_model_id, "model", sizeof("model"));
    session.last_update_check_day = 20260827;
    memcpy(session.last_update_check_version, "0.1.0", sizeof("0.1.0"));
    CHECK(bongo_cat_session_add_model(&session, "keyboard"));
    CHECK(bongo_cat_session_add_model(&session, "gamepad"));
    session.active_behavior_count = 2;
    memcpy(session.active_behaviors[0].model_id, "model",
        sizeof("model"));
    memcpy(session.active_behaviors[0].behavior_id,
        "model:motion:Tap:0", sizeof("model:motion:Tap:0"));
    memcpy(session.active_behaviors[1].model_id, "model",
        sizeof("model"));
    memcpy(session.active_behaviors[1].behavior_id,
        "model:expression:2", sizeof("model:expression:2"));

    const char *settings_path = "bongocat-settings.json";
    const char *session_path = "bongocat-session.json";
    BongoCatError error = {0};
    CHECK(bongo_cat_settings_save(settings_path, &settings, &error) ==
        BONGO_CAT_OK);
    CHECK(bongo_cat_session_save(session_path, &session, &error) ==
        BONGO_CAT_OK);
    CHECK(contains_text(settings_path, "\"format\": \"bongocat/settings\""));
    CHECK(contains_text(settings_path, "\"captureBackground\": true"));
    CHECK(contains_text(settings_path, "\"randomExpression\": true"));
    CHECK(contains_text(settings_path,
        "\"randomExpressionIntervalSeconds\": 12.0"));
    CHECK(contains_text(settings_path, "\"multiplePets\": true") && !contains_text(settings_path, "inputReleaseDelaySeconds"));
    CHECK(contains_text(settings_path, "\"removedModels\"") &&
        contains_text(settings_path, "\"model~2\""));
    CHECK(contains_text(settings_path, "\"example\""));
    CHECK(!contains_text(settings_path, "activeModelId"));
    CHECK(contains_text(session_path, "\"format\": \"bongocat/session\""));
    CHECK(contains_text(session_path, "\"contentWidth\": 612") &&
        contains_text(session_path, "\"contentHeight\": 354"));
    CHECK(contains_text(session_path, "\"activeModelId\": \"model\""));
    CHECK(contains_text(session_path, "\"lastUpdateCheckDay\": 20260827"));
    CHECK(contains_text(session_path, "\"additionalModelIds\""));
    CHECK(contains_text(session_path, "\"activeBehaviors\""));
    CHECK(contains_text(session_path,
        "\"behaviorId\": \"model:expression:2\""));
    CHECK(!contains_text(session_path, "clickThrough"));

    static BongoCatSettings loaded_settings;
    static BongoCatSessionState loaded_session;
    bongo_cat_settings_defaults(&loaded_settings);
    bongo_cat_session_defaults(&loaded_session);
    CHECK(bongo_cat_settings_load(settings_path, &loaded_settings, &error) ==
        BONGO_CAT_OK);
    CHECK(bongo_cat_session_load(session_path, &loaded_session, &error) ==
        BONGO_CAT_OK);
    CHECK(loaded_settings.model.max_fps == 30 && loaded_settings.model.mirror &&
        loaded_settings.model.multiple_pets);
    CHECK(loaded_settings.window.pass_through &&
        loaded_settings.window.obs_background &&
        loaded_settings.window.random_expression &&
        loaded_settings.window.random_expression_interval_seconds == 12.0f);
    CHECK(loaded_settings.app.language == BONGO_CAT_LANG_ZH_CN);
    CHECK(strstr(loaded_settings.extensions_json,
        "\"enabled\":true") != NULL);
    CHECK(strcmp(bongo_cat_model_name(&loaded_settings,
        &(BongoCatModelEntry){.id = "model", .display_name = "Imported"}),
        "Display") == 0);
    CHECK(bongo_cat_settings_model_removed(&loaded_settings, "model~2") &&
        bongo_cat_settings_model_removed(&loaded_settings, "model-extra"));
    CHECK(bongo_cat_settings_restore_model_package(&loaded_settings, "model"));
    CHECK(!bongo_cat_settings_model_removed(&loaded_settings, "model~2") &&
        bongo_cat_settings_model_removed(&loaded_settings, "model-extra"));
    CHECK(loaded_session.window.x == -321 &&
        loaded_session.window.opacity_percent == 75.0f &&
        loaded_session.window.width == 700 &&
        loaded_session.window.height == 500 &&
        loaded_session.window.content_width == 612 &&
        loaded_session.window.content_height == 354);
    CHECK(strcmp(loaded_session.active_model_id, "model") == 0);
    CHECK(loaded_session.last_update_check_day == 20260827 &&
        strcmp(loaded_session.last_update_check_version, "0.1.0") == 0);
    CHECK(loaded_session.additional_model_count == 2 &&
        bongo_cat_session_model_active(&loaded_session, "keyboard") &&
        bongo_cat_session_model_active(&loaded_session, "gamepad"));
    CHECK(loaded_session.active_behavior_count == 2);
    CHECK(strcmp(loaded_session.active_behaviors[0].behavior_id,
        "model:motion:Tap:0") == 0);
    CHECK(strcmp(loaded_session.active_behaviors[1].behavior_id,
        "model:expression:2") == 0);

    const char *unsupported = "bongocat-unsupported.json";
    write_text(unsupported,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":2}");
    CHECK(bongo_cat_settings_load(unsupported, &loaded_settings, &error) ==
        BONGO_CAT_ERROR_FORMAT);
    write_text(unsupported,
        "{\"format\":\"bongocat/session\",\"schemaVersion\":2}");
    CHECK(bongo_cat_session_load(unsupported, &loaded_session, &error) ==
        BONGO_CAT_ERROR_FORMAT);

    write_text(unsupported, "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,\"rendering\":{\"inputReleaseDelaySeconds\":3,\"maximumFps\":30}}");
    CHECK(bongo_cat_settings_load(unsupported, &loaded_settings, &error) == BONGO_CAT_OK && loaded_settings.model.max_fps == 30);

    write_text(unsupported,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"rendering\":{\"maximumFps\":\"fast\"}}");
    loaded_settings.model.max_fps = 73;
    CHECK(bongo_cat_settings_load(unsupported, &loaded_settings, &error) ==
        BONGO_CAT_ERROR_FORMAT);
    CHECK(loaded_settings.model.max_fps == 73);
    write_text(unsupported,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"rendering\":{\"maximumFps\":1.5}}");
    CHECK(bongo_cat_settings_load(unsupported, &loaded_settings, &error) ==
        BONGO_CAT_ERROR_FORMAT);
    write_text(unsupported,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"rendering\":{\"maximumFps\":30,\"maximumFps\":60}}");
    CHECK(bongo_cat_settings_load(unsupported, &loaded_settings, &error) ==
        BONGO_CAT_ERROR_FORMAT);

    /* "垂直同步"是新增键: 老配置里没有它, 行为必须和加这个键之前完全一样
       (那时同步是由最大帧率推出来的: 限帧 -> 开, "无限" -> 关, 见 frame_clock.c)。
       所以下面这条只写 maximumFps, 期望读出来的是**推出来的**同步状态, 而不是新增
       字段的原始默认值 —— 这是"老用户升级后不突变"的回归测试。 */
    write_text(unsupported,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"rendering\":{\"maximumFps\":30}}");
    bongo_cat_settings_defaults(&loaded_settings);
    CHECK(bongo_cat_settings_load(unsupported, &loaded_settings, &error) ==
        BONGO_CAT_OK && loaded_settings.model.max_fps == 30 &&
        loaded_settings.model.vsync == 1);
    write_text(unsupported,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"rendering\":{\"maximumFps\":0}}");
    bongo_cat_settings_defaults(&loaded_settings);
    CHECK(bongo_cat_settings_load(unsupported, &loaded_settings, &error) ==
        BONGO_CAT_OK && loaded_settings.model.max_fps == 0 &&
        loaded_settings.model.vsync == 0);
    /* 显式写了 vsync 就听它的, 即使和"跟随帧率"的老规则相反 */
    write_text(unsupported,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"rendering\":{\"maximumFps\":0,\"vsync\":1}}");
    bongo_cat_settings_defaults(&loaded_settings);
    CHECK(bongo_cat_settings_load(unsupported, &loaded_settings, &error) ==
        BONGO_CAT_OK && loaded_settings.model.max_fps == 0 &&
        loaded_settings.model.vsync == 1);
    write_text(unsupported,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"rendering\":{\"maximumFps\":60,\"vsync\":0}}");
    bongo_cat_settings_defaults(&loaded_settings);
    CHECK(bongo_cat_settings_load(unsupported, &loaded_settings, &error) ==
        BONGO_CAT_OK && loaded_settings.model.vsync == 0);
    /* 类型不对 / 越界都是配置损坏, 不能悄悄当成某个开关状态; 失败时保留原设置 */
    write_text(unsupported,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"rendering\":{\"vsync\":\"on\"}}");
    loaded_settings.model.vsync = 1;
    CHECK(bongo_cat_settings_load(unsupported, &loaded_settings, &error) ==
        BONGO_CAT_ERROR_FORMAT);
    CHECK(loaded_settings.model.vsync == 1);
    write_text(unsupported,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"rendering\":{\"vsync\":2.5}}");
    CHECK(bongo_cat_settings_load(unsupported, &loaded_settings, &error) ==
        BONGO_CAT_ERROR_FORMAT);
    write_text(unsupported,
        "{\"format\":\"bongocat/session\",\"schemaVersion\":1,"
        "\"window\":{\"visible\":123}}");
    CHECK(bongo_cat_session_load(unsupported, &loaded_session, &error) ==
        BONGO_CAT_ERROR_FORMAT);
    write_text(unsupported,
        "{\"format\":\"bongocat/session\",\"schemaVersion\":1,"
        "\"window\":{\"size\":{\"width\":700,\"height\":500}}}");
    bongo_cat_session_defaults(&loaded_session);
    CHECK(bongo_cat_session_load(unsupported, &loaded_session, &error) ==
        BONGO_CAT_OK && loaded_session.window.content_width == 700 &&
        loaded_session.window.content_height == 500);

    static BongoCatSettings canonical_settings;
    static BongoCatSessionState canonical_session;
    bongo_cat_settings_defaults(&canonical_settings);
    bongo_cat_session_defaults(&canonical_session);
    canonical_settings.model.max_fps = 900;
    canonical_session.window.scale_percent = -1.0f;
    CHECK(bongo_cat_settings_save(settings_path, &canonical_settings, &error) ==
        BONGO_CAT_OK);
    CHECK(bongo_cat_session_save(session_path, &canonical_session, &error) ==
        BONGO_CAT_OK);
    bongo_cat_settings_defaults(&loaded_settings);
    bongo_cat_session_defaults(&loaded_session);
    CHECK(bongo_cat_settings_load(settings_path, &loaded_settings, &error) ==
        BONGO_CAT_OK);
    CHECK(bongo_cat_session_load(session_path, &loaded_session, &error) ==
        BONGO_CAT_OK);
    CHECK(loaded_settings.model.max_fps == 60);
    /* 落盘的是收口后的具体开关状态(0/1), 不是 sentinel: 本版本写过的配置不再依赖
       "同步跟随最大帧率"那条兼容规则。 */
    CHECK(contains_text(settings_path, "\"vsync\": 1"));
    CHECK(loaded_settings.model.vsync == 1);
    CHECK(loaded_session.window.scale_percent == 10.0f);

    const char *append_path = "bongocat-append.log";
    if (bongo_cat_path_is_file(append_path))
        CHECK(bongo_cat_file_remove(append_path));
    CHECK(bongo_cat_file_append(append_path, "first\n", 6));
    CHECK(bongo_cat_file_append(append_path, "second\n", 7));
    CHECK(contains_text(append_path, "first\nsecond\n"));

    CHECK(bongo_cat_file_remove(settings_path));
    CHECK(bongo_cat_file_remove(session_path));
    CHECK(bongo_cat_file_remove(unsupported));
    CHECK(bongo_cat_file_remove(append_path));
}
