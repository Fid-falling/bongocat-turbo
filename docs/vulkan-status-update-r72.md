# Vulkan 状态更新（第 72 轮）— 模型缩放/叠加层/径向菜单

`docs/vulkan-status-update-r44-53.md` 的后续。本轮修掉三个用户可见的 Vulkan 缺陷，
并把径向菜单的绘制通路真正接上。

## 一、模型"没跟着缩放走"的根因：贴图 alpha 掩码为空

用户报告：K 渲染下导入的模型不与缩放一起变化，内置模型的元素也画不对。

根因在模型加载的 Vulkan 分支：它只把贴图上传成 GPU 纹理，没有像 GL 分支那样
生成 CPU 侧的 alpha 掩码（`texture_alpha_`）。而 `prepare_expression_frame()`
正是靠这个掩码剔除全透明三角形来算模型的"可见包络"，包络再决定窗口的 frame
内边距。掩码为空 → 透明画布被当成可见内容 → 窗口按错误尺寸开出。

实测（内置 `standard` 模型，同一存储）：

| | frame 内边距 | 内容 1037x600 对应的窗口 |
|---|---|---|
| OpenGL | `l0 r0 t0.5114 b0` | 1037x907 |
| Vulkan（修前） | `l0.1170 r0.1170 t1.0596 b0.3812` | **1280x1464** |
| Vulkan（修后） | `l0 r0 t0.5114 b0` | 1037x907 |

修后 `--ci-live2d-scenario=visual-consistency` 的 27 行数据在两个后端的
`fit_scale` / `translate` / `visible_min/max` 等全部几何列上逐字节相同
（`edge_pixels` 列除外，见第四节）。

## 二、离屏目标不跟随窗口尺寸

Live2D 的离屏目标只在 attach 时建一次，改缩放/拖窗口都不会重建：SDK 按旧
extent 投影，合成又按 `image/reference = 目标/窗口` 算比例，模型于是不再缩放。

- `ensure_target_size()`：每帧对比交换链 extent，不一致就重建（重建前
  `vkDeviceWaitIdle`）；
- `vk_scene.c` 在画 Live2D 之前先 `bongo_cat_vk_resize()`，交换链不再滞后一帧。

实测：attach 600x600 → 自适应 1037x907 → 手动缩到 900x900，交换链与目标同时跟随，
不崩溃。

## 三、叠加层视口：猫与键盘对不上的第二个原因

修完第一、二节之后模型的比例已经和 OpenGL 一致，但**叠加层的位置仍然不同**：
同尺寸（1037x907）对比截图，背景图/键盘在 Vulkan 下比 GL 高约 140 px，
所以猫看起来浮在键盘上方（用户反馈的"猫的位置不对"）。

原因在 `app_loop.c` 与 `vk_scene.c` 的视口处理不一致：

| | OpenGL (`app_loop.c`) | Vulkan (`vk_scene.c`，修前) |
|---|---|---|
| 背景/按键/特效/指针 | 模型内容视口 `glViewport(content_x, content_y, content_w, content_h)` | 整窗视口 |
| Live2D | 整窗视口（投影里自带内容变换） | 整窗视口 |

背景/按键/特效/指针都是按 NDC 画的整屏四边形，视口不同 → 它们被拉伸到整窗并
整体偏上，与模型错位。

修法：新增 `bongo_cat_vk_set_viewport(x, y, w, h)`（左上角像素坐标、y 向下，
对应 GL 的 `glViewport`），`vk_scene.c` 复刻 GL 的视口切换顺序。

验证（内置 `standard` 模型，1037x907）：

- 桌面/键盘所在列的亮度剖面两后端**逐字节相同**（`[0,0,9,253,215]`），右侧列同样；
- 全窗像素差异从 17.4% 降到 **2.1%**，残差只是 Live2D 待机动作与屏幕上的
  RivaTuner OSD 文本（两次运行本来就会变）。

## 四、叠加层按键/特效贴图在 Vulkan 下整块消失

Cubism 构建里 `overlay.left/right/effect` 一直是裸 `GLuint`（GL 上传创建，绘制时
包成 `BongoCatTexture{.gl=...}`）。Vulkan 下既没有 GL 上下文可以创建它们，
`texture->vk` 也是空的，`draw()` 直接返回 —— 按键高亮/特效这些元素全部丢失。

改成与背景图同一套后端无关句柄：缓存槽存 `BongoCatTexture`，用
`bongo_cat_texture_from_file()` 加载、`bongo_cat_texture_release()` 释放，
`left/right/effect` 改为指向缓存槽的指针（拷贝句柄会在槽位复用后变成野指针）。

实测：Vulkan 下按 A 键窗口像素变化 23234 px，键盘上的 A 键高亮与 GL 一致。

## 五、径向菜单的 Vulkan 绘制通路

菜单窗口在 `renderer=vulkan` 时改用自己的一份 Vulkan 上下文。之前打不开，
因为 `dial_paint_init` 无条件调用 `bongo_cat_gl_load()`，在没有 GL 上下文的
进程里直接报 `Missing OpenGL function: glCreateShader`。

补齐后仍有第二个坑：菜单顶点是 `pos 2f + uv 2f + RGBA8`（20 字节）且坐标是窗口
逻辑像素，一帧约 6 万个顶点；把它交给鼠标叠加层用的 `draw_triangles` 有两处不匹配
（那条路要求 32 字节浮点颜色顶点、位置已在裁剪空间），而且 256 KB 的通用顶点缓冲
装不下 1.2 MB 数据 → 绘制静默失败、菜单整片空白。

现在新增 `bongo_cat_vk_draw_dial()`：独立 `dial.vert`/`dial.frag` 管线（复用
`ui_layout` 的 scale/offset push constant，片元做普通 RGBA 采样 + 预乘 alpha）+
一份 4 MB 的菜单顶点缓冲。

实测：Vulkan 下右键宠物窗口，菜单正常打开并绘制 12 个扇区与彩色图标；
与 OpenGL 的稳定帧逐格对比，菜单区域差异 ≤0.8%（其余差异来自宠物动画与任务栏时钟）。

## 六、"无限"帧率：垂直同步被无条件打开

界面里的最大帧率是 30 / 60 / 无限（`BONGO_CAT_MAX_FPS_UNLIMITED = 0`），但两个后端
都无条件开着垂直同步：

- Vulkan 的交换链固定用 `VK_PRESENT_MODE_FIFO_KHR`（`vk_swapchain.c` 里那句
  "支持 FIFO 就用 FIFO" 的循环永远走同一分支）→ 帧率被锁在显示器刷新率上，
  100 Hz 屏就是 100 FPS；
- OpenGL 在窗口创建时固定 `SDL_GL_SetSwapInterval(1)`。

帧节奏本来就由 `frame_clock.c` 的 `frame_interval_ns()` 决定，呈现侧只需如实反映
设置。现在由 `bongo_cat_app_apply_frame_pacing()` 统一下发：无限 → 关同步，
30/60 → 保持开启；值变化时 Vulkan 重建交换链（呈现模式是创建时定下的）。

实测（内置 `standard` 模型，统计 10 秒窗口内的帧数）：

| 后端 | 最大帧率 | 修前 | 修后 |
|---|---|---|---|
| Vulkan | 无限 | 锁在刷新率 | 5927 / 10000 ms ≈ 593 FPS |
| Vulkan | 60 | ~60 | ~62（保持同步，`present_mode=2`） |
| OpenGL | 无限 | 锁在刷新率 | 7402 / 10000 ms ≈ 740 FPS |

## 七、已知遗留

| 项 | 状态 |
|---|---|
| 设置窗口 Vulkan | 仍默认 GL；Vulkan 通路崩溃点 `vkQueuePresentKHR`，未根除（开关 `BONGO_CAT_PREFERENCES_VULKAN=1`） |
| 封面截取 / 整帧回读 | 仍走 GL；配方见 `docs/vulkan-readback-recipe.md` |
| `live2d-visual-audit` 的 `edge_pixels` | 该列用 `glReadPixels`，Vulkan 下读到的是未初始化数据；几何列可信，`edge_pixels` 列在 Vulkan 下不可用 |

## 八、当前可用性总览

| 维度 | 状态 |
|---|---|
| 默认策略 | AUTO 优先 Vulkan + 失败回退 OpenGL |
| 主窗口（含 Live2D） | 渲染 / 缩放 / 输入 / 遮罩 / 命中 / 叠加层元素 均已验证 |
| 窗口几何 | 与 OpenGL 完全一致（frame 内边距、窗口尺寸、可见边界） |
| 径向菜单 | Vulkan 通路已启用并验证（截图 + 与 GL 逐格对比） |
| 校验洁净度 | VUID 0（含 SDK 阶段掩码配置期补丁） |
| 三配置构建 | 全绿（Cubism+Vulkan / Cubism+Vulkan 关闭 / 另一 Vulkan 配置） |
| 设置窗口 | GL（默认）；Vulkan 为开关后的可复现 bug |
