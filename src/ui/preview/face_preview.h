#ifndef BONGO_CAT_FACE_PREVIEW_H
#define BONGO_CAT_FACE_PREVIEW_H

#include "bongo_cat/app.h"

#include <stdbool.h>

/*
 * 面捕预览窗 (独立 SDL 窗口 + 自己的 renderer): 把采集线程的预览帧贴成纹理。
 * 与设置窗口无关 —— 可以一边开着预览一边调参数, 也可以把预览拖到副屏。
 */
bool bongo_cat_face_preview_open(void);
bool bongo_cat_face_preview_show(BongoCatApp *app);
void bongo_cat_face_preview_close(void);
/* 主循环每帧调用一次 (只在窗口存在时做事)。 */
void bongo_cat_face_preview_tick(BongoCatApp *app);

#endif
