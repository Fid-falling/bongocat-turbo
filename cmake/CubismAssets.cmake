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

# 把渲染组件(cores/bongo_live2d.dll)也装进内嵌资源包, 于是发行形态回到"只发单个 exe":
# 组件随 exe 一起走, 首次启动时由宿主按需释放到 <存储根>/cores/ (见
# src/platform/windows/windows_assets.c 的 bongo_cat_platform_embedded_assets_release_component)。
#
# 包内路径刻意是 `cores/...` 而不是 `assets/cores/...`: 包里两个首段各有一条去向 ——
# `assets/` 解到 <存储根>/cache/embedded-assets-<版本>/ 当资源用, `cores/` 是程序自身的一部分,
# 只有"该文件不存在或不是这一版"时才去写 <存储根>/cores/。用路径首段区分, 比再加一个 PE 资源
# 或第二套打包器简单, 也让 asset_packer 只有一条规则(ROOT 下的结构就是包内结构)。
#
# 只拷组件文件本身, **不**整目录拷 cores/: 那个目录里可能有 .staging(导入过程中的临时区)
# 之类的瞬态内容, 不该进包。组件不存在时(无 SDK 的诊断构建)什么都不做 —— 那种构建里
# Live2D 本来就走 live2d_stub.c 的兜底实现。
function(bongo_cat_configure_embedded_renderer_component)
  if(NOT TARGET bongo_live2d)
    return()
  endif()
  set(extra_commands
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "${BONGO_CAT_ASSET_STAGE}/cores"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bongo_live2d>"
      "${BONGO_CAT_ASSET_STAGE}/cores/"
  )
  set(BONGO_CAT_ASSET_EXTRA_COMMANDS
    "${BONGO_CAT_ASSET_EXTRA_COMMANDS}" ${extra_commands} PARENT_SCOPE)
  # 依赖要显式写上目标名: 少了它, 改了组件源码只重建组件、不重打资源包, 包里的组件就会是上一版
  # —— 而"包里是旧组件"正是这次改造要消灭的问题。
  set(BONGO_CAT_ASSET_EXTRA_DEPENDS
    "${BONGO_CAT_ASSET_EXTRA_DEPENDS}" bongo_live2d PARENT_SCOPE)
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
