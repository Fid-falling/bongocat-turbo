set(CUBISM_CORE_PATH "${BONGO_CAT_CUBISM_SDK}/Core")
set(CUBISM_FRAMEWORK_PATH "${BONGO_CAT_CUBISM_SDK}/Framework")
set(CUBISM_GLEW_PATH "${BONGO_CAT_CUBISM_SDK}/Samples/OpenGL/thirdParty/glew")

foreach(PATH IN ITEMS CUBISM_FRAMEWORK_PATH CUBISM_GLEW_PATH)
  if(NOT EXISTS "${${PATH}}")
    message(FATAL_ERROR "Incomplete Cubism SDK: ${${PATH}} is missing")
  endif()
endforeach()

# Live2D Cubism Core 是 Live2D Inc. 的专有库: **任何发行物都不得包含它**, 也不得静态链接它
# (见 LIVE2D-PLUGIN-PLAN.md §9.1 的规则 1)。所以这里不再有 `Live2DCubismCore` 导入目标 ——
# csm* 符号由 cmake/GenCoreShim.cmake 生成的运行期 shim 提供, shim 再去用户自备的 Core
# 动态库里解析真实符号。SDK 目录里只需要 Framework 源码、GLEW 与 Core 的**头文件**。
if(WIN32 AND MSVC_VERSION LESS 1930)
  message(FATAL_ERROR "Cubism Windows build requires Visual Studio 2022 (v143) or newer")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/GenCoreShim.cmake")

add_library(glew_s STATIC "${CUBISM_GLEW_PATH}/src/glew.c")
target_include_directories(glew_s SYSTEM PUBLIC "${CUBISM_GLEW_PATH}/include")
target_compile_definitions(glew_s PUBLIC GLEW_STATIC GLEW_NO_GLU)
target_link_libraries(glew_s PUBLIC OpenGL::GL)
if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  target_link_libraries(glew_s PRIVATE X11::X11 ${CMAKE_DL_LIBS})
endif()
# SDK 的后端目录只能一个一个 add_subdirectory (它用的是单一的
# FRAMEWORK_SOURCE), 所以这里保留 OpenGL, Vulkan 的源文件在下面显式补进
# Framework 目标。
set(FRAMEWORK_SOURCE OpenGL)
# SDK 的 CubismRenderer::Create/StaticRelease 在两个后端文件里各有一份且无条件
# 定义, 而我们的实现固定由 src/live2d/cubism_renderer_factory.cpp 提供, 所以配置
# 阶段必须把 SDK 那两份关掉 (与是否启用 Vulkan 后端无关)。
include(cmake/CubismRendererSelect.cmake)
bongo_cat_prepare_cubism_renderer_selection()
if(BONGO_CAT_ENABLE_VULKAN)
endif()
add_subdirectory("${CUBISM_FRAMEWORK_PATH}" "${CMAKE_BINARY_DIR}/cubism-framework")
include(cmake/CubismUserModelSafety.cmake)
bongo_cat_harden_cubism_user_model(Framework)
include(cmake/CubismCoreProfile.cmake)
include(cmake/CubismShaderOptimize.cmake)
bongo_cat_optimize_cubism_shaders(Framework)
if(APPLE)
  bongo_cat_core_profile_prepare_shaders()
  bongo_cat_core_profile_patch_renderer(Framework)
endif()

if(WIN32)
  target_compile_definitions(Framework PUBLIC CSM_TARGET_WIN_GL)
elseif(APPLE)
  target_compile_definitions(Framework PUBLIC CSM_TARGET_MAC_GL)
else()
  target_compile_definitions(Framework PUBLIC CSM_TARGET_LINUX_GL)
endif()
target_include_directories(Framework SYSTEM PUBLIC
  "${CUBISM_FRAMEWORK_PATH}/src"
  "${CUBISM_CORE_PATH}/include")
# Framework 只链**shim**与 glew: 它调用 csm* 时命中的是我们生成的转发函数(运行期再到用户
# 自备的 Core 里解析), 所以 Framework 与宿主都不再需要 Core 的库文件。
target_link_libraries(Framework PUBLIC bongo_cat_core_shim glew_s)

if(BONGO_CAT_ENABLE_VULKAN)
  target_link_libraries(Framework PUBLIC Vulkan::Vulkan)
  # SDK 自带的 Vulkan 渲染器源文件 (对应其 Rendering/Vulkan/CMakeLists.txt,
  # 这里显式列出, 因为 add_subdirectory 只能加一个后端目录)。
  target_sources(Framework PRIVATE
    "${CUBISM_FRAMEWORK_PATH}/src/Rendering/Vulkan/CubismClass_Vulkan.cpp"
    "${CUBISM_FRAMEWORK_PATH}/src/Rendering/Vulkan/CubismDeviceInfo_Vulkan.cpp"
    "${CUBISM_FRAMEWORK_PATH}/src/Rendering/Vulkan/CubismOffscreenManager_Vulkan.cpp"
    "${CUBISM_FRAMEWORK_PATH}/src/Rendering/Vulkan/CubismOffscreenRenderTarget_Vulkan.cpp"
    "${CUBISM_FRAMEWORK_PATH}/src/Rendering/Vulkan/CubismRenderTarget_Vulkan.cpp"
    "${CUBISM_FRAMEWORK_PATH}/src/Rendering/Vulkan/CubismRenderer_Vulkan.cpp")
  # 关掉 SDK 里两份工厂定义, 用我们自己的运行时选择实现
  target_compile_definitions(Framework PUBLIC BONGO_CAT_RUNTIME_RENDERER_SELECT)
  # SDK 的 Vulkan 着色器: 按 (色彩混合 x alpha 混合) 组合编出一整套 .spv,
  # 规则直接复用 SDK 自己的 CMakeLists (依赖 $ENV{VK_SDK_PATH}/Bin/glslc.exe,
  # CMake 的 FindVulkan 找到的正是同一个 SDK)。
  add_subdirectory(
    "${CUBISM_FRAMEWORK_PATH}/src/Rendering/Vulkan/Shaders"
    "${CMAKE_BINARY_DIR}/cubism-vulkan-shaders")
  set(BONGO_CAT_CUBISM_VK_SHADER_DIR
    "${CMAKE_BINARY_DIR}/cubism-vulkan-shaders/compiledShaders")
  message(STATUS "Cubism Vulkan 渲染器已启用, 着色器输出: ${BONGO_CAT_CUBISM_VK_SHADER_DIR}")
endif()
