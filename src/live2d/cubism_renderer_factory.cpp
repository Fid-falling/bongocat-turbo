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
#ifdef BONGO_CAT_HAS_VULKAN
#include <Rendering/Vulkan/CubismRenderer_Vulkan.hpp>
#endif

namespace bongo_cat {
namespace {
/* 进程级的后端开关。默认 false (OpenGL): 即使 Vulkan 路径还没接通, 行为也和以前完全一样。
 * 由 cubism_vulkan_bridge.cpp 的 remember() 在窗口创建时打开 —— 那时还没有任何模型, 所以
 * 不存在"建到一半切后端"的竞态。 */
bool g_use_vulkan = false;
}

void bongo_cat_cubism_renderer_use_vulkan(bool enabled) {
    g_use_vulkan = enabled;
}

bool bongo_cat_cubism_renderer_uses_vulkan() {
    return g_use_vulkan;
}
} // namespace bongo_cat

/*
 * C 链接的薄包装: 上面两个函数在 namespace bongo_cat 里(名字带 C++ 修饰), 而宿主服务表
 * (src/runtime/cores/renderer_module_services.c)是 C 文件, 取不到它们的地址。加一层
 * extern "C" 比在服务表那边拼修饰名可靠, 也让"什么能跨界"一眼可见。
 */
extern "C" void bongo_cat_renderer_select_use_vulkan(bool enabled) {
    bongo_cat::bongo_cat_cubism_renderer_use_vulkan(enabled);
}

extern "C" bool bongo_cat_renderer_select_uses_vulkan(void) {
    return bongo_cat::bongo_cat_cubism_renderer_uses_vulkan();
}

namespace Live2D {
namespace Cubism {
namespace Framework {
namespace Rendering {

CubismRenderer *CubismRenderer::Create(csmUint32 width, csmUint32 height) {
#ifdef BONGO_CAT_HAS_VULKAN
    if (bongo_cat::bongo_cat_cubism_renderer_uses_vulkan())
        return CSM_NEW CubismRenderer_Vulkan(width, height);
#endif
    return CSM_NEW CubismRenderer_OpenGLES2(width, height);
}

/* StaticRelease 必须把两个后端都放掉: 编译期两个后端的静态资源都可能被建过
 * (例如先跑 GL 再切 Vulkan), 只释放当前那个会留下进程退出时的泄漏报告。 */
void CubismRenderer::StaticRelease() {
    CubismRenderer_OpenGLES2::DoStaticRelease();
#ifdef BONGO_CAT_HAS_VULKAN
    CubismRenderer_Vulkan::DoStaticRelease();
#endif
}

} // namespace Rendering
} // namespace Framework
} // namespace Cubism
} // namespace Live2D
