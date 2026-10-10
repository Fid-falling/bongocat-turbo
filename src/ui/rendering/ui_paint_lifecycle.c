/*
 * 绘制缓存的对外生命周期入口 (实现都在 ui_paint_cache.c)。
 *
 * 调用时序是有契约的, 别调换:
 *  每帧只调一次 begin_frame(), 且必须在"本帧任何 paint_* 绘制之前" ——
 *  它先把上一帧的快照收走, 再记录本帧起点; 顺序反了会让刚画过的条目
 *  被当成上一帧的旧货清掉, 等于每帧重新烘焙 (设置界面 20 FPS)。
 *  destroy() 只在真正要拆 GPU 资源时调 (关闭窗口 / 重建字体图集 / 空闲回收);
 *  上下文已经没了的时候要改用 paint_cache_abandon()。
 */
#include "ui_paint.h"
#include "ui_paint_cache.h"

void bongo_cat_ui_paint_begin_frame(BongoCatUIBackend *backend) {
    bongo_cat_ui_paint_cache_begin_frame(backend);
}

void bongo_cat_ui_paint_destroy(BongoCatUIBackend *backend) {
    bongo_cat_ui_paint_cache_destroy(backend);
}
