# 让 OpenGL 与 Vulkan 两个 Cubism 渲染后端同时编进同一个二进制。
#
# SDK 的两个后端实现文件里都无条件定义了工厂函数:
#     CubismRenderer* CubismRenderer::Create(csmUint32, csmUint32)
#     void CubismRenderer::StaticRelease()
# 同时编译就会重复符号。这里在配置阶段给这两段加上条件编译
# (BONGO_CAT_RUNTIME_RENDERER_SELECT), 唯一的实现在
# src/live2d/cubism_renderer_factory.cpp 里, 运行时按后端选择。
#
# 这是配置期补丁, 与 cmake/CubismCoreProfile.cmake / CubismShaderOptimize.cmake
# 的做法一致: 重新配置就会重新应用, SDK 被换掉也不会留下半成品状态。

set(BONGO_CAT_CUBISM_RENDERER_SELECT_DEFINE BONGO_CAT_RUNTIME_RENDERER_SELECT)

function(bongo_cat_guard_cubism_renderer_factory backend relative class)
  set(path "${BONGO_CAT_CUBISM_SDK}/Framework/src/Rendering/${relative}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Cubism renderer source missing: ${path}")
  endif()
  file(READ "${path}" text)
  # 统一成 LF 再匹配, 避免 CRLF 环境下匹配失败
  string(REPLACE "\r\n" "\n" text "${text}")
  set(guard "#ifndef ${BONGO_CAT_CUBISM_RENDERER_SELECT_DEFINE}")
  set(endguard "#endif  // ${BONGO_CAT_CUBISM_RENDERER_SELECT_DEFINE}")
  if(text MATCHES "${BONGO_CAT_CUBISM_RENDERER_SELECT_DEFINE}")
    message(STATUS "Cubism ${backend} 渲染器工厂已条件编译")
    return()
  endif()
  set(plain
"CubismRenderer* CubismRenderer::Create(csmUint32 width, csmUint32 height)
{
    return CSM_NEW ${class}(width, height);
}

void CubismRenderer::StaticRelease()
{
    ${class}::DoStaticRelease();
}")
  set(guarded
"${guard}
CubismRenderer* CubismRenderer::Create(csmUint32 width, csmUint32 height)
{
    return CSM_NEW ${class}(width, height);
}

void CubismRenderer::StaticRelease()
{
    ${class}::DoStaticRelease();
}
${endguard}")
  string(FIND "${text}" "${plain}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR
      "无法在 ${path} 中定位 ${class} 的工厂实现; SDK 版本可能不匹配, "
      "请检查 CubismRenderer::Create / StaticRelease 的写法。")
  endif()
  string(REPLACE "${plain}" "${guarded}" text "${text}")
  file(WRITE "${path}" "${text}")
  message(STATUS "Cubism ${backend} 渲染器工厂已条件编译 (${path})")
endfunction()

function(bongo_cat_prepare_cubism_renderer_selection)
  if(NOT BONGO_CAT_ENABLE_VULKAN)
    return()
  endif()
  bongo_cat_guard_cubism_renderer_factory(
    "OpenGL" "OpenGL/CubismRenderer_OpenGLES2.cpp" "CubismRenderer_OpenGLES2")
  bongo_cat_guard_cubism_renderer_factory(
    "Vulkan" "Vulkan/CubismRenderer_Vulkan.cpp" "CubismRenderer_Vulkan")
endfunction()
