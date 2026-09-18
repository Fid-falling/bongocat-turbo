# Vulkan 着色器: 配置阶段用 glslc 编译成 SPIR-V, 再嵌成一个 C 头文件。
#
# 选择在配置阶段生成而不是 add_custom_command: 生成的数组直接 #include 进
# vk_draw.c, 目标依赖最简单, 也不需要把 .spv 随程序分发。CMAKE_CONFIGURE_DEPENDS
# 保证改了 GLSL 之后重新配置。

set(BONGO_CAT_VK_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/render/vk/shaders")
set(BONGO_CAT_VK_SHADERS
  overlay.vert
  overlay.frag
  pointer.vert
  pointer.frag)

function(bongo_cat_embed_vulkan_shaders header)
  if(NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR
      "BONGO_CAT_ENABLE_VULKAN=ON 需要 glslc 把 GLSL 编译成 SPIR-V; "
      "请安装包含着色器工具的完整 Vulkan SDK")
  endif()
  file(MAKE_DIRECTORY "${BONGO_CAT_GENERATED_INCLUDE_DIR}")
  set(binary_dir "${CMAKE_CURRENT_BINARY_DIR}/vk-shaders")
  file(MAKE_DIRECTORY "${binary_dir}")
  set(content "/* 由 cmake/Vulkan.cmake 调用 glslc 生成, 请勿手工修改。 */\n")
  string(APPEND content "#include <stdint.h>\n\n")
  foreach(shader IN LISTS BONGO_CAT_VK_SHADERS)
    get_filename_component(extension "${shader}" EXT)
    get_filename_component(name "${shader}" NAME_WE)
    string(REPLACE "." "_" suffix "${extension}")
    set(source "${BONGO_CAT_VK_SHADER_DIR}/${shader}")
    set(binary "${binary_dir}/${shader}.inc")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    execute_process(
      COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -O -mfmt=c "${source}" -o "${binary}"
      RESULT_VARIABLE compile_result
      ERROR_VARIABLE compile_error)
    if(NOT compile_result EQUAL 0)
      message(FATAL_ERROR "着色器编译失败 (${shader}): ${compile_error}")
    endif()
    file(READ "${binary}" body)
    string(APPEND content
      "static const uint32_t bongo_cat_spv_${name}${suffix}[] =\n${body};\n\n")
  endforeach()
  file(WRITE "${header}.tmp" "${content}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${header}.tmp" "${header}")
endfunction()
