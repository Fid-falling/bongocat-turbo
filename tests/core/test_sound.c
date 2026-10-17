#include "test.h"
#include "bongo_cat/config.h"
#include "bongo_cat/sound.h"

#include <stdio.h>
#include <string.h>

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file) { CHECK(fputs(text, file) >= 0); CHECK(fclose(file) == 0); }
}

static void test_sound_id(void) {
    char id[BONGO_CAT_SOUND_ID_CAP];
    /* Windows 分隔符统一成 '/', 模型目录前缀被去掉 */
    CHECK(bongo_cat_sound_id(id, sizeof(id), "A-清宵",
        "C:\\models\\A-清宵", "C:\\models\\A-清宵\\resources\\sounds\\0.flac"));
    CHECK(strcmp(id, "A-清宵:resources/sounds/0.flac") == 0);
    /* 目录大小写不同也算同一个目录 (Windows 语义) */
    CHECK(bongo_cat_sound_id(id, sizeof(id), "pet",
        "G:\\STORAGE\\pet", "g:\\storage\\pet\\resources\\sounds\\a.ogg"));
    CHECK(strcmp(id, "pet:resources/sounds/a.ogg") == 0);
    /* 不在模型目录下: 退回文件名, 仍然稳定 */
    CHECK(bongo_cat_sound_id(id, sizeof(id), "pet", "C:\\models\\pet",
        "D:\\elsewhere\\voice.wav"));
    CHECK(strcmp(id, "pet:voice.wav") == 0);
    /* 没有模型 id / 目录时也不崩 */
    CHECK(bongo_cat_sound_id(id, sizeof(id), "", NULL, "sounds/x.flac"));
    CHECK(strcmp(id, "x.flac") == 0);
    CHECK(!bongo_cat_sound_id(id, sizeof(id), "pet", NULL, ""));
    CHECK(id[0] == '\0');
    /* 放不下就失败, 不返回半截 id (半截 id 会被写进配置, 下次又匹配不上) */
    char tiny[8];
    CHECK(!bongo_cat_sound_id(tiny, sizeof(tiny), "pet", NULL,
        "resources/sounds/0.flac"));
    CHECK(tiny[0] == '\0');
}

static void test_sound_settings(void) {
    static BongoCatSettings settings;
    bongo_cat_settings_defaults(&settings);
    CHECK(settings.sound.volume == BONGO_CAT_SOUND_VOLUME_DEFAULT);
    CHECK(settings.sound.muted == 0 && settings.sound.disabled_count == 0);
    /* 默认: 与加这个功能之前一致, 所有音效都允许播 */
    CHECK(bongo_cat_settings_sound_decision(&settings, "pet:x.flac") ==
        BONGO_CAT_SOUND_DECISION_ALLOWED);
    CHECK(bongo_cat_settings_set_sound_disabled(&settings, "pet:x.flac", true));
    CHECK(bongo_cat_settings_sound_disabled(&settings, "pet:x.flac"));
    CHECK(bongo_cat_settings_sound_decision(&settings, "pet:x.flac") ==
        BONGO_CAT_SOUND_DECISION_DISABLED);
    CHECK(bongo_cat_settings_sound_decision(&settings, "pet:y.flac") ==
        BONGO_CAT_SOUND_DECISION_ALLOWED);
    /* 重复追加是幂等的; 清除后恢复允许 */
    CHECK(bongo_cat_settings_set_sound_disabled(&settings, "pet:x.flac", true));
    CHECK(settings.sound.disabled_count == 1);
    CHECK(bongo_cat_settings_set_sound_disabled(&settings, "pet:x.flac", false));
    CHECK(!bongo_cat_settings_sound_disabled(&settings, "pet:x.flac"));
    CHECK(settings.sound.disabled_count == 0);
    CHECK(bongo_cat_settings_set_sound_disabled(&settings, "pet:x.flac", false));
    CHECK(settings.sound.disabled_count == 0);
    /* 全部静音优先于逐项开关, 且不改变逐项状态 */
    CHECK(bongo_cat_settings_set_sound_disabled(&settings, "pet:x.flac", true));
    settings.sound.muted = 1;
    CHECK(bongo_cat_settings_sound_decision(&settings, "pet:x.flac") ==
        BONGO_CAT_SOUND_DECISION_MUTED);
    CHECK(bongo_cat_settings_sound_decision(&settings, "pet:y.flac") ==
        BONGO_CAT_SOUND_DECISION_MUTED);
    CHECK(bongo_cat_settings_sound_disabled(&settings, "pet:x.flac"));
    /* 音量钳制 */
    settings.sound.volume = -5;
    CHECK(bongo_cat_settings_sound_volume(&settings) == 0);
    settings.sound.volume = 999;
    CHECK(bongo_cat_settings_sound_volume(&settings) == 100);
    settings.sound.volume = 37;
    CHECK(bongo_cat_settings_sound_volume(&settings) == 37);
}

static void test_sound_round_trip(void) {
    static BongoCatSettings settings;
    static BongoCatSettings loaded;
    bongo_cat_settings_defaults(&settings);
    settings.sound.volume = 42;
    settings.sound.muted = 1;
    CHECK(bongo_cat_settings_set_sound_disabled(&settings,
        "A-清宵:resources/sounds/0.flac", true));
    CHECK(bongo_cat_settings_set_sound_disabled(&settings,
        "B:resources/sounds/1.flac", true));
    BongoCatError error = {0};
    const char *path = "bongocat-sound-settings.json";
    CHECK(bongo_cat_settings_save(path, &settings, &error) == BONGO_CAT_OK);
    bongo_cat_settings_defaults(&loaded);
    CHECK(bongo_cat_settings_load(path, &loaded, &error) == BONGO_CAT_OK);
    CHECK(loaded.sound.volume == 42);
    CHECK(loaded.sound.muted == 1);
    CHECK(loaded.sound.disabled_count == 2);
    CHECK(bongo_cat_settings_sound_disabled(&loaded,
        "A-清宵:resources/sounds/0.flac"));
    CHECK(bongo_cat_settings_sound_disabled(&loaded,
        "B:resources/sounds/1.flac"));

    /* 老配置没有 "sound" 对象: 必须原样保持默认 (音量 100 / 不静音 / 全开)。 */
    const char *legacy = "bongocat-legacy-settings.json";
    write_text(legacy,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"rendering\":{\"maximumFps\":30}}");
    bongo_cat_settings_defaults(&loaded);
    CHECK(bongo_cat_settings_load(legacy, &loaded, &error) == BONGO_CAT_OK);
    CHECK(loaded.sound.volume == 100 && loaded.sound.muted == 0 &&
        loaded.sound.disabled_count == 0);
    CHECK(bongo_cat_settings_sound_decision(&loaded, "anything") ==
        BONGO_CAT_SOUND_DECISION_ALLOWED);

    /* 手改文件写越界音量/重复 id: 校验收口而不是让配置读失败 */
    const char *dirty = "bongocat-dirty-sound-settings.json";
    write_text(dirty,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"sound\":{\"volume\":999,\"muted\":7,"
        "\"disabled\":[\"a\",\"a\",\"\",\"b\"]}}");
    bongo_cat_settings_defaults(&loaded);
    CHECK(bongo_cat_settings_load(dirty, &loaded, &error) == BONGO_CAT_OK);
    CHECK(loaded.sound.volume == 100);
    CHECK(loaded.sound.muted == 1);
    CHECK(loaded.sound.disabled_count == 2);
    CHECK(!strcmp(loaded.sound.disabled[0], "a") &&
        !strcmp(loaded.sound.disabled[1], "b"));

    /* 类型错误仍然整体拒绝读取, 不能留下"读了一半"的配置 */
    const char *broken = "bongocat-broken-sound-settings.json";
    write_text(broken,
        "{\"format\":\"bongocat/settings\",\"schemaVersion\":1,"
        "\"sound\":{\"volume\":\"loud\"}}");
    bongo_cat_settings_defaults(&loaded);
    loaded.sound.volume = 73;
    CHECK(bongo_cat_settings_load(broken, &loaded, &error) ==
        BONGO_CAT_ERROR_FORMAT);
    CHECK(loaded.sound.volume == 73);
}

void test_sound(void) {
    test_sound_id();
    test_sound_settings();
    test_sound_round_trip();
}
