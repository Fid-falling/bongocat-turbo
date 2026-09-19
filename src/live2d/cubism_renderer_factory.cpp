/*
 * OpenGL / Vulkan 两个 Cubism 渲染后端的唯一工厂实现。
 *
 * SDK 自带的两个后端文件都定义了 CubismRenderer::Create 与 StaticRelease,
 * 同时编译会重复符号, 所以那两个定义在配置阶段被
 * cmake/CubismRendererSelect.cmake 用 BONGO_CAT_RUNTIME_RENDERER_SELECT 关掉了,
 * 由这里统一提供, 并按运行时开关选择后端。
 */

#include "cubism_renderer_select.hpp"

#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <Rendering/Vulkan/CubismRenderer_Vulkan.hpp>

namespace bongo_cat {
namespace {
bool g_use_vulkan = false;
}

void bongo_cat_cubism_renderer_use_vulkan(bool enabled) {
    g_use_vulkan = enabled;
}

bool bongo_cat_cubism_renderer_uses_vulkan() {
    return g_use_vulkan;
}
} // namespace bongo_cat

namespace Live2D {
namespace Cubism {
namespace Framework {
namespace Rendering {

CubismRenderer *CubismRenderer::Create(csmUint32 width, csmUint32 height) {
    if (bongo_cat::bongo_cat_cubism_renderer_uses_vulkan())
        return CSM_NEW CubismRenderer_Vulkan(width, height);
    return CSM_NEW CubismRenderer_OpenGLES2(width, height);
}

void CubismRenderer::StaticRelease() {
    CubismRenderer_OpenGLES2::DoStaticRelease();
    CubismRenderer_Vulkan::DoStaticRelease();
}

} // namespace Rendering
} // namespace Framework
} // namespace Cubism
} // namespace Live2D
