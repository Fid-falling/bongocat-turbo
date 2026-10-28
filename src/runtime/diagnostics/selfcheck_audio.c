/*
 * 自检第 6 组: 音频。
 *
 * 只回答一个问题: 这台机器上 miniaudio 能不能把播放设备打开。探测用一个**临时引擎**
 * (bongo_cat_audio_probe), 与正在播放的引擎完全隔离 —— 自检不能在用户按键音效还在播的
 * 时候把引擎重建掉。失败时把 miniaudio 的返回码与描述原样写进报告, 那是这类问题唯一
 * 有意义的线索 (没有音频设备 / 被独占 / 驱动拒绝格式)。
 */
#include "selfcheck_internal.h"

#include "bongo_cat/app.h"
#include "bongo_cat/audio.h"

void bongo_cat_selfcheck_audio(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app) {
    (void)app;
    bongo_cat_selfcheck_section(builder, "音频");
    BongoCatError error = {0};
    if (bongo_cat_audio_probe(&error))
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "音频设备",
            "miniaudio 设备初始化成功，音效可以正常播放");
    else
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "音频设备",
            "%s", error.message[0] ? error.message :
                "miniaudio 设备初始化失败（原因未记录）");
}
