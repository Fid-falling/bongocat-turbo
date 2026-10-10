#ifndef BONGO_CAT_WINDOWS_SNAPSHOT_H
#define BONGO_CAT_WINDOWS_SNAPSHOT_H

#include <SDL3/SDL.h>

/*
 * "交互预览"(snapshot)模式: 拖动或缩放窗口期间, 把窗口内容抓成一张静态图, 用一个
 * 覆盖整个虚拟桌面的透明窗口按新几何显示它, 源窗口在此期间完全透明。
 *
 * 为什么这么做: 拖动过程中反复移动/改尺寸真实窗口会让 DWM 不断重建合成表面, 画面会
 * 闪; 改成"移动一张静态图"之后 DWM 只看到图像内容变化, 手感也稳定。
 * 生命周期: create(抓图 + 建预览窗口) -> present(几何每次变化后重绘) -> destroy(恢复
 * 源窗口的不透明度与 DWM 合成)。整段期间源窗口不透明度被置 0, 由预览窗口代表它。
 */
typedef struct BongoCatWindowsSnapshot BongoCatWindowsSnapshot;

/* Capture requires the source OpenGL context to be current. */
BongoCatWindowsSnapshot *bongo_cat_windows_snapshot_create(SDL_Window *source,
    float opacity);
/* 按当前几何重绘一次预览(内部会跳过几何没变化的调用)。返回 false 表示这一帧没画成功。 */
bool bongo_cat_windows_snapshot_present(BongoCatWindowsSnapshot *snapshot);
/* 指定预览图出现的位置与大小(屏幕坐标); 传 NULL 表示回到"跟随源窗口"的默认行为。 */
void bongo_cat_windows_snapshot_geometry(BongoCatWindowsSnapshot *snapshot,
    const SDL_FRect *rect);
void bongo_cat_windows_snapshot_pointer(BongoCatWindowsSnapshot *snapshot);
/* 用源窗口客户区坐标做命中测试: 判据是抓图时留下的 alpha(> 8 才算命中), 所以猫周围
   的透明区域不会吃掉鼠标。 */
bool bongo_cat_windows_snapshot_hit(BongoCatWindowsSnapshot *snapshot,
    float x, float y);
/* 释放并恢复现场: 先还原源窗口的不透明度与 DWM 透明合成, 再销毁预览窗口 / 纹理 /
   像素缓冲。 */
void bongo_cat_windows_snapshot_destroy(BongoCatWindowsSnapshot *snapshot);

#endif
