# Vulkan 状态更新（第 44–53 轮）— 当前口径

`docs/vulkan-cubism-status.md` 的后续补充，用于纠正/更新该文档中已过时的部分。

## 一、默认策略已翻转

- `bongo_cat_renderer_resolve(AUTO)` **优先 Vulkan**，不可用时回退 OpenGL
  （`src/render/render_backend.c`，提交 `e493f0a`）。
- 实测矩阵：
  - `auto` + Vulkan 可用 → Vulkan 渲染 Live2D（截图、帧率、视线/按键响应）；
  - `auto` + 强制 Vulkan 失败（`BONGO_CAT_TEST_VULKAN_FALLBACK=1`）→ 回退 OpenGL，
    `create_renderer: uses_vulkan=0`，启动完成且稳定；
  - 两个构建（Cubism / 无 Cubism）在 `auto` 下都走 Vulkan。

因此 `docs/vulkan-cubism-status.md` 第三节里的"⑤ `AUTO` 优先 Vulkan（待办）"**已完成**。

## 二、设置窗口：默认回到 OpenGL

- 设置窗口的 Vulkan 通路会在呈现阶段崩：
  阶段日志定位到 `vkQueuePresentKHR`（`endframe: idx=0 count=3 frame=0` → `submit…` → `present…` → 崩），
  索引与提交状态均正常；多轮修复（回读作废、信号量不在帧中途重建、GL 护栏）未根除。
- 因此**默认改回 OpenGL**，Vulkan 通路保留在 `BONGO_CAT_PREFERENCES_VULKAN=1` 开关后
  （提交 `a9c3eb8`）。该组合（Vulkan 主窗口 + GL 设置窗口）实测稳定：存活 14s+、句柄文件正常。

## 三、一处需要更正的旧判断

`nvoglv64.dll` 是 **NVIDIA 的统一驱动**（GL 与 Vulkan 共用同一个 DLL，Vulkan 的 ICD 也指向它），
并不是"GL 专用驱动"。先前据它推断出的"Vulkan 窗口上残留 GL 调用导致崩溃"方向不成立。
设置路径那批 GL 护栏（`SDL_GL_MakeCurrent` / `SDL_GL_SetSwapInterval` / GL 状态查询，共 8 处）
本身是正确改进，但不是该崩溃的解。

## 四、剩余工作的施工图与验证手段（均已入库）

| 项 | 施工图 | 验证手段 |
|---|---|---|
| 径向菜单移植 | `docs/vulkan-dial-port-recipe.md`（行号级锚点） | `--ci-menu`（`app.c:160`）+ 截图 |
| 封面截取 / 整帧回读 | `docs/vulkan-readback-recipe.md` | 导入模型后检查 `cover.png`；快照对比 |
| 设置窗口 Vulkan | 崩溃点 `vkQueuePresentKHR`（开关后） | `--ci-preferences` + 存活判据 |

## 五、当前可用性总览

| 维度 | 状态 |
|---|---|
| 默认策略 | AUTO 优先 Vulkan + 失败回退 OpenGL |
| 主窗口（含 Live2D） | 渲染 / 帧率 / 输入 / 遮罩 / 命中 均已验证 |
| 校验洁净度 | VUID 0（含 SDK 阶段掩码配置期补丁） |
| 三配置构建 | 全绿（Cubism+Vulkan / 无 Cubism / Cubism+Vulkan 关闭） |
| 设置窗口 | GL（默认）；Vulkan 为开关后的可复现 bug |
