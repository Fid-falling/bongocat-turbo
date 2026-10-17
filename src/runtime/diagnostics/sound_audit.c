/*
 * --ci-sound-test / --ci-sound-toggle=N 的诊断实现 (见 app.h 的字段说明)。
 *
 * 它解决的是"音效管理怎么验收": 静音/逐项禁用/音量都不产生画面, 真听声音既不可
 * 自动化也不可靠。这里在真实模型加载完成之后, 用真实播放入口把每条音效走一遍,
 * 把 id / 判定 / 有效增益写成 INFO 日志 (Release 也可见), 于是"被跳过"与"仍播放"
 * 都有据可查, 而不用依赖调试构建的 DEBUG 级别。
 *
 * toggle 钩子是为了验证 settings_hash 覆盖了新字段: 它走设置页同一条 set 接口,
 * 退出时 config_store 的 flush 必须把新值写进 settings.json —— 哈希漏字段时这里
 * 不会落盘, 下次启动读回来还是旧值。
 */
#include "runtime.h"
#include "model_sound.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

/* 一条"真的会出声"的条目: sound 条目 (sound-clear 没有文件) 与带 Sound 的动作。
   与设置页清单、播放路径的口径保持一致。 */
static bool playable(const BongoCatBehaviorEntry *entry) {
    if (!entry->sound[0]) return false;
    if (entry->kind == BONGO_CAT_BEHAVIOR_SOUND) return !entry->sound_clear;
    return entry->kind == BONGO_CAT_BEHAVIOR_MOTION;
}

/* 同一个音频文件可能同时挂在 sound 与 motion 上; 清单只列一次, toggle 的序号也
   按去重后的顺序算, 两边必须一致。 */
static bool duplicate(const BongoCatApp *app, size_t index) {
    for (size_t i = 0; i < index; ++i)
        if (playable(&app->behaviors.entries[i]) &&
            !strcmp(app->behaviors.entries[i].sound,
                app->behaviors.entries[index].sound))
            return true;
    return false;
}

static const char *decision_name(int decision) {
    if (decision == BONGO_CAT_SOUND_DECISION_MUTED) return "muted";
    if (decision == BONGO_CAT_SOUND_DECISION_DISABLED) return "disabled";
    return "play";
}

/* 把第 position 个 (1 基) 音效的禁用状态翻过来。返回是否成功找到并写入。 */
static bool toggle_sound(BongoCatApp *app, unsigned position) {
    size_t current = 0;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (!playable(entry) || duplicate(app, i)) continue;
        if (++current != position) continue;
        char id[BONGO_CAT_SOUND_ID_CAP];
        if (!bongo_cat_app_sound_id(app, entry->sound, id, sizeof(id))) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[sound-test] toggle failed: cannot build id for %s",
                entry->sound);
            return false;
        }
        bool disabled = bongo_cat_settings_sound_disabled(&app->settings, id);
        if (!bongo_cat_settings_set_sound_disabled(&app->settings, id,
                !disabled)) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[sound-test] toggle failed: settings table is full");
            return false;
        }
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[sound-test] toggle id=%s label=%s disabled=%d", id,
            entry->label[0] ? entry->label : bongo_cat_path_name(entry->sound),
            disabled ? 0 : 1);
        return true;
    }
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
        "[sound-test] toggle index %u is out of range (sounds=%zu)", position,
        current);
    return false;
}

void bongo_cat_sound_self_test(BongoCatApp *app) {
    if (!app || !app->smoke_sound_test) return;
    if (app->smoke_sound_toggle) toggle_sound(app, app->smoke_sound_toggle);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[sound-test] model=%s volume=%d muted=%d disabled=%zu",
        app->loaded_model[0] ? app->loaded_model : "(none)",
        bongo_cat_settings_sound_volume(&app->settings),
        app->settings.sound.muted != 0 ? 1 : 0,
        app->settings.sound.disabled_count);
    size_t count = 0, allowed = 0, blocked = 0;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (!playable(entry) || duplicate(app, i)) continue;
        BongoCatSoundStatus status;
        bongo_cat_sound_status(app, entry->sound, &status);
        const char *label = entry->label[0] ? entry->label :
            bongo_cat_path_name(entry->sound);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[sound-test] sound id=%s label=%s path=%s volume=%d gain=%.3f "
            "decision=%s", status.id, label, entry->sound, status.volume,
            (double)status.gain, decision_name(status.decision));
        /* 走真实入口: sound 条目 = run_behavior (与快捷键/菜单同一条路);
           动作的绑定音效 = 播放函数 (与 run_behavior 里那一步同一个实现)。 */
        bool played = entry->kind == BONGO_CAT_BEHAVIOR_SOUND
            ? bongo_cat_app_run_behavior(app, entry)
            : bongo_cat_app_play_sound(app, entry->sound, false);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[sound-test] result id=%s played=%d", status.id, played ? 1 : 0);
        count++;
        if (status.decision == BONGO_CAT_SOUND_DECISION_ALLOWED) allowed++;
        else blocked++;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[sound-test] summary model=%s sounds=%zu allowed=%zu blocked=%zu",
        app->loaded_model[0] ? app->loaded_model : "(none)", count, allowed,
        blocked);
}
