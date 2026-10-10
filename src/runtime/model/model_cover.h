/* 模型封面任务的接口: 排程、到期判定、失败退避、完成摘链, 以及真正的抓取入口。
 * 任务以模型 id 为键存在 BongoCatApp 的定长数组里 (容量 BONGO_CAT_MODEL_COVER_PENDING_CAP),
 * 只有 app->loaded_model 对应的那一条会被 capture 处理; capture 返回 false 只表示"这一帧没抓到",
 * 任务仍挂起, 调用方必须配一次 defer 退避。
 */
#ifndef BONGO_CAT_MODEL_COVER_H
#define BONGO_CAT_MODEL_COVER_H

#include "bongo_cat/app.h"
#include "model_cover_paths.h"

/* Pending cover queue and retry policy. */
void bongo_cat_model_cover_schedule(BongoCatApp *app,
    const BongoCatModelEntry *entry);
bool bongo_cat_model_cover_pending(const BongoCatApp *app);
bool bongo_cat_model_cover_capture_due(const BongoCatApp *app, uint64_t now);
const char *bongo_cat_model_cover_pending_path(const BongoCatApp *app);
void bongo_cat_model_cover_defer(BongoCatApp *app, const char *reason);
void bongo_cat_model_cover_finish(BongoCatApp *app);

/* Capture workflow and framebuffer readback. */
void bongo_cat_model_cover_capture_before_switch(BongoCatApp *app);
bool bongo_cat_model_cover_capture(BongoCatApp *app, int width, int height);

#endif
