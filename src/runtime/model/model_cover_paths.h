/* 封面图在模型目录里的相对路径, 只在这里用宏定义; 而 UI 与渲染侧还有几处硬编码的
 * "resources/cover.png" (设置窗口卡片、覆盖层资源、表盘封面), 改名要一起改。
 * 它同时是导入的校验项 —— 缺 cover.png 与 background.png 的包会被判为不完整。
 */
#ifndef BONGO_CAT_MODEL_COVER_PATHS_H
#define BONGO_CAT_MODEL_COVER_PATHS_H

#define BONGO_CAT_MODEL_COVER_RESOURCES "resources"
#define BONGO_CAT_MODEL_COVER_FILE BONGO_CAT_MODEL_COVER_RESOURCES "/cover.png"

#endif
