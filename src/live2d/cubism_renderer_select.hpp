#ifndef BONGO_CAT_CUBISM_RENDERER_SELECT_HPP
#define BONGO_CAT_CUBISM_RENDERER_SELECT_HPP

#include <Rendering/CubismRenderer.hpp>

namespace bongo_cat {

/*
 * Cubism 渲染后端的运行时选择。
 *
 * SDK 的 CubismRenderer::Create 是编译期固定的 (每个后端各有一份实现), 同时编
 * 两个后端会重复符号, 所以两个工厂都在配置阶段被 cmake/CubismRendererSelect.cmake
 * 加了条件编译, 唯一的实现在 cubism_renderer_factory.cpp 里, 由这里控制走哪个。
 *
 * 默认 OpenGL: 即使 Vulkan 后端还没接通, 行为也和以前完全一样。
 */
void bongo_cat_cubism_renderer_use_vulkan(bool enabled);
bool bongo_cat_cubism_renderer_uses_vulkan();

} // namespace bongo_cat

#endif
