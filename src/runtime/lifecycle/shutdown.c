/*
 * 统一关闭路径, 正常退出与启动失败共用。顺序是按依赖排的, 不要随意重排:
 *   1. 先保存状态 (行为缓存 / session / 配置), 再拆会改状态的组件;
 *   2. 再停掉会产生回调或持有 UI 状态的组件 (多宠 / 模型热更新 / 更新检查 / 设置窗口 /
 *      托盘), 它们都可能在关闭过程中回头调用 app;
 *   3. 最后才销毁平台层、日志与窗口 —— 前面任何一种失败都还要靠日志记下来。
 * 每个被释放的指针都可能是 NULL (启动到一半就失败), 这也是 shutdown 必须幂等的原因。
 */
#include "runtime.h"
#include "bongo_cat/log.h"
#include "model_import_lock.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/overlay.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/tray.h"

#include <stdlib.h>

/* stage 与 exit_code 是"这次为什么退出"的唯一记录来源: 会写进 runtime-state.txt 与日志。 */
void bongo_cat_app_shutdown(BongoCatApp *app, const char *stage,
    int exit_code) {
    bongo_cat_runtime_stage(app, stage);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[runtime] Shutdown started: stage=%s exit_code=%d",
        stage, exit_code);
/* 先落盘再拆组件: 行为状态要写回 session, 而 session 的 flush 必须发生在这里, 之后不再
   有机会保存。 */
    bongo_cat_app_capture_behavior_state(app);
    /* 帧节奏统计落盘: 它必须在 state_root 与日志都还在的时候做 (后面会拆掉平台与日志)。 */
    bongo_cat_frame_interval_report(app);
    bongo_cat_window_snapshot_discard(app);
    bongo_cat_config_store_flush(app);
    bongo_cat_multi_pet_shutdown(app);
    bongo_cat_model_refresh_shutdown(app);
    bongo_cat_update_destroy(app->update);
    app->update = NULL;
    bongo_cat_preferences_stop_imports(app->preferences);
    bongo_cat_preferences_destroy(app->preferences);
    bongo_cat_import_storage_lock_shutdown();
    bongo_cat_i18n_destroy(app->i18n);
    bongo_cat_tray_destroy(app->tray);
    bongo_cat_gamepads_set_enabled(app, false);
    bongo_cat_audio_destroy(app->audio);
    bongo_cat_overlay_destroy(app->overlay);
    bongo_cat_live2d_destroy(app->live2d);
/* behavior_cache 由 app 直接持有 (不属于任何子模块), 所以在这里单独释放。 */
    free(app->behavior_cache);
    app->behavior_cache = NULL;
    bongo_cat_platform_shutdown(&app->platform);
    bongo_cat_runtime_log_stop();
    bongo_cat_window_destroy(app);
    bongo_cat_runtime_clean_shutdown(app, exit_code);
}
