function(bongo_cat_configure_embedded_cubism_assets)
  # 命令累积在局部变量里, 最后一次性写回父作用域 —— 不能用
  # "set(X ... PARENT_SCOPE) 之后再 set(X ${X} ... PARENT_SCOPE) 追加" 的写法:
  # 带 PARENT_SCOPE 的赋值会把当前作用域的 X 一并定义成空值, 于是第二次读 ${X}
  # 拿到的是空串, 前面那条命令被整条丢掉。OpenGL 的着色器源码 (.vert/.frag) 就是
  # 这么从内嵌资源里消失的: FrameworkShaders 里只进去了 Vulkan 的 .spv, 而 GL
  # 后端必须从文件读着色器源码 (CubismShader_OpenGLES2::LoadShaderProgramFromFile)
  # —— exe 旁边没有这份文件时 (用户只复制了 exe) 所有着色器程序都是 0, 遮罩绘制
  # 整块不画, 猫的身体就没了。
  set(shader_inputs)
  set(extra_commands)
  if(BONGO_CAT_CUBISM_ENABLED)
    file(GLOB_RECURSE shader_inputs CONFIGURE_DEPENDS
      "${BONGO_CAT_CUBISM_SHADER_SOURCE_DIR}/*")
    list(APPEND extra_commands
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${BONGO_CAT_CUBISM_SHADER_SOURCE_DIR}"
        "${BONGO_CAT_ASSET_STAGE}/assets/FrameworkShaders")
    # Vulkan 后端的 Cubism 渲染器按相对路径 "FrameworkShaders/*.spv" 加载编译好的
    # 着色器。把它们也打进内嵌资源: 用户只把 exe 复制到别处 (桌面) 时, 磁盘上那份
    # 就没了, 但解出来的内嵌资源里还有一份, 桥会切到那个目录去 (见
    # src/live2d/cubism_vulkan_bridge.cpp 的 shader_parent_directory)。
    if(BONGO_CAT_ENABLE_VULKAN AND BONGO_CAT_CUBISM_VK_SHADER_DIR)
      file(GLOB vk_shader_inputs CONFIGURE_DEPENDS
        "${BONGO_CAT_CUBISM_VK_SHADER_DIR}/*")
      list(APPEND shader_inputs ${vk_shader_inputs})
      list(APPEND extra_commands
        COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${BONGO_CAT_CUBISM_VK_SHADER_DIR}"
          "${BONGO_CAT_ASSET_STAGE}/assets/FrameworkShaders")
    endif()
  endif()
  set(BONGO_CAT_ASSET_INPUTS ${BONGO_CAT_ASSET_INPUTS} ${shader_inputs}
    PARENT_SCOPE)
  set(BONGO_CAT_ASSET_EXTRA_COMMANDS "${extra_commands}" PARENT_SCOPE)
endfunction()

function(bongo_cat_stage_cubism_assets target)
  if(NOT BONGO_CAT_CUBISM_ENABLED)
    return()
  endif()
  get_target_property(is_bundle ${target} MACOSX_BUNDLE)
  if(APPLE AND is_bundle)
    set(shader_destination
      "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/assets/FrameworkShaders")
  else()
    set(shader_destination "$<TARGET_FILE_DIR:${target}>/FrameworkShaders")
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${BONGO_CAT_CUBISM_SHADER_SOURCE_DIR}"
      "${shader_destination}"
    VERBATIM)
  # Vulkan 后端的 Cubism 渲染器按相对路径 FrameworkShaders/*.spv 加载着色器,
  # 所以编译好的 SPIR-V 也要放进同一个目录 (由 FrameworkShaders 目标先生成)。
  if(BONGO_CAT_ENABLE_VULKAN AND BONGO_CAT_CUBISM_VK_SHADER_DIR)
    add_dependencies(${target} FrameworkShaders)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${BONGO_CAT_CUBISM_VK_SHADER_DIR}"
        "${shader_destination}"
      VERBATIM)
  endif()
  if(UNIX AND NOT APPLE AND target STREQUAL "bongo_cat")
    install(DIRECTORY
      "${BONGO_CAT_CUBISM_SHADER_SOURCE_DIR}/"
      DESTINATION assets/FrameworkShaders COMPONENT Runtime)
  endif()
endfunction()
