#ifndef BONGO_CAT_GL_SCALE_H
#define BONGO_CAT_GL_SCALE_H

#include "bongo_cat/common.h"

/*
 * OpenGL 侧的"渲染精度"实现: 以 N 倍分辨率离屏渲染, 再用线性过滤缩回窗口尺寸。
 *
 * 为什么单独一个文件: 这一段是纯 GL 资源 (FBO + 颜色纹理 + 深度 renderbuffer) 与
 * 一次 blit, 与叠加层的绘制逻辑没有交集; 放进 app_loop.c 会把那条呈现路径的注释
 * 挤散 (那里已经贴着 500 行的行数策略)。
 *
 * 调用顺序 (见 app_loop.c 的 render):
 *   float factor = bongo_cat_gl_scale_begin(settings.render_scale, width, height);
 *   ... 所有的 glViewport 换成 bongo_cat_gl_scale_viewport(factor, ...) ...
 *   bongo_cat_gl_scale_end(factor, width, height);
 * 之后的所有读帧操作 (模型封面截帧 / 圆角遮罩 / 帧诊断 / 命中回读) 读的都是默认
 * 帧缓冲 —— 也就是"已经缩回窗口尺寸的那一帧", 所以它们一行都不用改。
 *
 * factor 为 1.0 (或离屏不可用) 时所有函数都是 no-op, 直接画到默认帧缓冲:
 * 默认配置下这条路径与 1.17.0 完全一致 (逐像素相同)。
 */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * 开始一帧。返回本帧**实际**生效的倍率:
 *   - 1.0 = 没走离屏 (请求 1.0 / 驱动不支持 FBO / 离屏创建失败), 调用方照旧直接画;
 *   - >1.0 或 <1.0 = 已绑定离屏目标并设好视口, 调用方的视口都要过 viewport()。
 * 请求值超出 GPU 能力 (最大纹理尺寸 / 总像素预算) 时会自动降到可行倍率并记一条 WARN。
 */
float bongo_cat_gl_scale_begin(float requested, int width, int height);
/* 按倍率放大视口矩形并 glViewport (倍率 1.0 时就等于直接 glViewport)。 */
void bongo_cat_gl_scale_viewport(float factor, int x, int y, int width, int height);
/* 解绑离屏并把颜色纹理线性缩回默认帧缓冲 (倍率 1.0 时什么都不做)。 */
void bongo_cat_gl_scale_end(float factor, int width, int height);
/* 释放 GL 资源 (窗口/上下文销毁时调用; 之后再 begin 会重建)。 */
void bongo_cat_gl_scale_release(void);

#ifdef __cplusplus
}
#endif

#endif
