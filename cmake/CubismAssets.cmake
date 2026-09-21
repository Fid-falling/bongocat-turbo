function(bongo_cat_configure_embedded_cubism_assets)
  set(BONGO_CAT_ASSET_EXTRA_COMMANDS PARENT_SCOPE)
  if(NOT BONGO_CAT_CUBISM_ENABLED)
    return()
  endif()
  file(GLOB_RECURSE shader_inputs CONFIGURE_DEPENDS
    "${BONGO_CAT_CUBISM_SHADER_SOURCE_DIR}/*")
  set(BONGO_CAT_ASSET_INPUTS ${BONGO_CAT_ASSET_INPUTS} ${shader_inputs} PARENT_SCOPE)
  set(BONGO_CAT_ASSET_EXTRA_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${BONGO_CAT_CUBISM_SHADER_SOURCE_DIR}"
      "${BONGO_CAT_ASSET_STAGE}/assets/FrameworkShaders" PARENT_SCOPE)
  # Vulkan 后端的 Cubism 渲染器按相对路径 "FrameworkShaders/*.spv" 加载编译好的
  # 着色器。把它们也打进内嵌资源: 用户只把 exe 复制到别处 (桌面) 时, 磁盘上那份
  # 就没了, 但解出来的内嵌资源里还有一份, 桥会切到那个目录去 (见
  # src/live2d/cubism_vulkan_bridge.cpp 的 shader_parent_directory)。
  if(BONGO_CAT_ENABLE_VULKAN AND BONGO_CAT_CUBISM_VK_SHADER_DIR)
    file(GLOB vk_shader_inputs CONFIGURE_DEPENDS
      "${BONGO_CAT_CUBISM_VK_SHADER_DIR}/*")
    set(BONGO_CAT_ASSET_INPUTS ${BONGO_CAT_ASSET_INPUTS} ${vk_shader_inputs}
      PARENT_SCOPE)
    set(BONGO_CAT_ASSET_EXTRA_COMMANDS ${BONGO_CAT_ASSET_EXTRA_COMMANDS}
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${BONGO_CAT_CUBISM_VK_SHADER_DIR}"
        "${BONGO_CAT_ASSET_STAGE}/assets/FrameworkShaders" PARENT_SCOPE)
  endif()
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
