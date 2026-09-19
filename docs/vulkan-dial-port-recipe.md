# 径向菜单（dial）移植到 Vulkan — 施工图

面向"让 Vulkan 完全替代 OpenGL"。总览见 `docs/vulkan-cubism-status.md`；
整帧回读（封面/快照）见 `docs/vulkan-readback-recipe.md`。

## 现状（勘察结论）

`src/ui/dial/` 共 11 个文件，**只有两处与 GL 绑定**：

| 文件 | GL 内容 | 处理 |
|---|---|---|
| `dial_window.c:73-74` | 窗口 flags 用 `SDL_WINDOW_OPENGL` | 按后端选 `SDL_WINDOW_VULKAN` |
| `dial_window.c:99-105` | `SDL_GL_GetAttribute/SetAttribute` + `SDL_GL_CreateContext` + `SDL_GL_MakeCurrent` + `SDL_GL_SetSwapInterval` | 换成 `bongo_cat_vk_create(d->window, &error)` |
| `dial_paint.c:19-78` | GL loader、着色器程序、白点纹理、`SDL_GL_SwapWindow` | 换成 Vulkan 纹理与三角绘制 |
| `dial_window.c:31/33/154/185` | 上下文切换时的 `SDL_GL_MakeCurrent` | Vulkan 下跳过 |

其余（`dial_geometry.c` 扇区三角化、`dial_fonts.c`、`dial_icons.c`、`dial_scene.c`、
`dial_draw.c`、`dial_items.c`、`dial_covers.c`、`dial_input.c`）**后端无关，不用改**。

## 施工步骤（三步）

### 1. `dial_internal.h`
dial 结构体加两个字段：

```c
    BongoCatVk *vk;      /* 非空表示这个菜单窗口走 Vulkan */
```

### 2. `dial_window.c`
- 第 73-74 行：

  ```c
  const bool vulkan = bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN;
  SDL_WindowFlags flags = (vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL)
      | SDL_WINDOW_TRANSPARENT | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
  ```
- 第 99-105 行整段：

  ```c
  if (vulkan) {
      BongoCatError error = {0};
      d->vk = bongo_cat_vk_create(d->window, &error);
      if (!d->vk) return false;
  } else {
      ... 原有 GL 上下文创建 ...
  }
  ```
- 上下文切换处（`:31/33/154/185`）：`d->vk` 非空时跳过 `SDL_GL_MakeCurrent`；
- 销毁处（`:182` 一带）：`d->vk` 非空时 `bongo_cat_vk_destroy(d->vk)`，否则原 `SDL_GL_DestroyContext`。

### 3. `dial_paint.c`
- 创建：Vulkan 分支用 `bongo_cat_vk_texture_from_pixels`（白点/图标）与
  `bongo_cat_vk_texture_from_alpha`（单通道图标）替代 `glGenTextures`/`glTexImage2D`；
- 每帧：`bongo_cat_vk_begin_frame(vk, pixel_width, pixel_height)` → 清屏色
  （`bongo_cat_vk_set_clear_color` + begin 的 clear）→ 逐项
  `bongo_cat_vk_draw_triangles(vk, texture, vertices, vertex_count, textured)`
  → `bongo_cat_vk_end_frame(vk)`（替代第 78 行的 `SDL_GL_SwapWindow`）；
- `dial_geometry.c` 产出的顶点格式若与 `bongo_cat_vk_draw_triangles` 期望的不一致，
  在 paint 层做一次转换（这是唯一可能需要适配的地方）。

## 验收判据

1. 构建：`build-cubism` / `build-vulkan` / `build-cubism-novk` 全绿；
2. 运行：Vulkan 后端下打开径向菜单（托盘菜单 → 径向菜单），窗口出现且扇区/图标/文字正常；
3. 交互：鼠标悬停切换扇区、点击触发对应动作（与 GL 路径行为一致）；
4. `BONGO_CAT_VULKAN_VALIDATION=1` 下无新增 VUID；
5. GL 路径零回归（`renderer=opengl` 时行为不变）。
