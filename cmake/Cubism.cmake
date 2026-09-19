set(CUBISM_CORE_PATH "${BONGO_CAT_CUBISM_SDK}/Core")
set(CUBISM_FRAMEWORK_PATH "${BONGO_CAT_CUBISM_SDK}/Framework")
set(CUBISM_GLEW_PATH "${BONGO_CAT_CUBISM_SDK}/Samples/OpenGL/thirdParty/glew")

foreach(PATH IN ITEMS CUBISM_FRAMEWORK_PATH CUBISM_GLEW_PATH)
  if(NOT EXISTS "${${PATH}}")
    message(FATAL_ERROR "Incomplete Cubism SDK: ${${PATH}} is missing")
  endif()
endforeach()

add_library(Live2DCubismCore STATIC IMPORTED GLOBAL)
if(WIN32)
  # 官方只提供 v143 的 Windows Core 库, 上游这里要求 1930 <= MSVC_VERSION < 1950。
  # MSVC 14.x (v143 之后) 保持二进制兼容, 所以这里允许更新的工具集 (例如 VS 18 的
  # 14.51/v145) 直接链 143 的库, 只提示风险; 低于 v143 仍然拒绝。
  if(MSVC_VERSION LESS 1930)
    message(FATAL_ERROR "Cubism Windows build requires Visual Studio 2022 (v143) or newer")
  endif()
  if(MSVC_VERSION GREATER_EQUAL 1950)
    message(WARNING
      "Cubism Core 库是 v143 构建的, 当前工具集 MSVC_VERSION=${MSVC_VERSION}; "
      "MSVC 14.x 二进制兼容, 若链接或运行异常请换 -T v143。")
  endif()
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CUBISM_WINDOWS_ARCH "x86_64")
  else()
    set(CUBISM_WINDOWS_ARCH "x86")
  endif()
  set(CUBISM_CORE_LIBRARY
    "${CUBISM_CORE_PATH}/lib/windows/${CUBISM_WINDOWS_ARCH}/143/Live2DCubismCore_MT.lib")
elseif(APPLE)
  if(CMAKE_OSX_ARCHITECTURES)
    list(GET CMAKE_OSX_ARCHITECTURES 0 CUBISM_ARCH)
  else()
    set(CUBISM_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  set(CUBISM_CORE_LIBRARY
    "${CUBISM_CORE_PATH}/lib/macos/${CUBISM_ARCH}/libLive2DCubismCore.a")
else()
  set(CUBISM_CORE_LIBRARY
    "${CUBISM_CORE_PATH}/lib/linux/x86_64/libLive2DCubismCore.a")
endif()
if(NOT EXISTS "${CUBISM_CORE_LIBRARY}")
  message(FATAL_ERROR "Cubism Core library missing: ${CUBISM_CORE_LIBRARY}")
endif()
set_target_properties(Live2DCubismCore PROPERTIES
  IMPORTED_LOCATION "${CUBISM_CORE_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${CUBISM_CORE_PATH}/include")

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
if(BONGO_CAT_ENABLE_VULKAN)
  # 两个后端的 CubismRenderer::Create/StaticRelease 会重复符号, 配置阶段先给
  # 它们加上条件编译, 唯一的实现放在 src/live2d/cubism_renderer_factory.cpp。
  include(cmake/CubismRendererSelect.cmake)
  bongo_cat_prepare_cubism_renderer_selection()
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
target_link_libraries(Framework PUBLIC Live2DCubismCore glew_s)

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
