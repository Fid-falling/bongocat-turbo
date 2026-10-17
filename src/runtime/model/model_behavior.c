/* 行为条目的执行入口: 快捷键、托盘菜单、设置界面试看都走这里。
 * 四类条目的成功语义不同 —— 特效与声音立即生效并直接返回, 只有动作 (motion) 与表情会走到末尾,
 * 把可见状态写回会话 (bongo_cat_app_capture_behavior_state) 并置 dirty; false = 这次没执行。
 */
#include "runtime.h"
#include "model_sound.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/overlay.h"

/* 返回 false = 本次没生效; 动作成功后会顺带播放绑定音效 (音效失败只记日志, 不回滚动作);
 * 表情是"再点一次取消"的开关语义, 用 -1 走淡出清除。
 * 所有真实出声都经过 bongo_cat_app_play_sound: 它先判"全部静音 / 该音效被禁用",
 * 再按总音量下发增益 (见 model_sound.c), 于是快捷键、右键菜单、随机音效三条路
 * 共享同一套策略, 不会出现"菜单里还响"的漏网之鱼。
 */
bool bongo_cat_app_run_behavior(BongoCatApp *app,
    const BongoCatBehaviorEntry *behavior) {
    if (!app || !behavior) return false;
    if (behavior->kind == BONGO_CAT_BEHAVIOR_EFFECT) {
        if (!bongo_cat_overlay_effect(app->overlay, behavior->effect)) return false;
    } else if (behavior->kind == BONGO_CAT_BEHAVIOR_SOUND) {
        if (behavior->sound_clear) {
            bongo_cat_audio_stop(app->audio);
            return true;
        }
        if (!behavior->sound[0]) return false;
        if (!bongo_cat_app_play_sound(app, behavior->sound,
            behavior->sound_overlap)) return false;
        return true;
    } else if (behavior->kind == BONGO_CAT_BEHAVIOR_MOTION) {
        bool started = bongo_cat_live2d_start_motion(app->live2d,
            behavior->group, behavior->index);
        if (!started) return false;
        /* 音效被用户关掉不影响动作本身: 动作照常完成, 只是不出声。 */
        if (behavior->sound[0])
            bongo_cat_app_play_sound(app, behavior->sound, false);
    } else {
        int expression = bongo_cat_live2d_expression(app->live2d) ==
            behavior->index ? -1 : behavior->index;
        if (!bongo_cat_live2d_set_expression(app->live2d, expression)) return false;
    }
    bongo_cat_app_capture_behavior_state(app);
    app->dirty = true;
    return true;
}

