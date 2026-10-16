if(APPLE)
  set(BONGO_CAT_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/icon.icns")
  set_target_properties(bongo_cat PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "BongoCat Turbo"
    MACOSX_BUNDLE_GUI_IDENTIFIER com.bongocat.desktop
    MACOSX_BUNDLE_ICON_FILE "icon.icns"
    MACOSX_BUNDLE_INFO_PLIST
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in")
  # Stage the icon with other resources before any later POST_BUILD signing.
  # Relink when it changes so incremental builds also run the resource copy.
  set_property(TARGET bongo_cat APPEND PROPERTY LINK_DEPENDS
    "${BONGO_CAT_APP_ICON}")
  add_custom_command(TARGET bongo_cat POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
      "$<TARGET_BUNDLE_CONTENT_DIR:bongo_cat>/MacOS/assets"
      "$<TARGET_BUNDLE_CONTENT_DIR:bongo_cat>/MacOS/FrameworkShaders"
      "$<TARGET_BUNDLE_CONTENT_DIR:bongo_cat>/Resources/assets"
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_BUNDLE_CONTENT_DIR:bongo_cat>/Resources/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${BONGO_CAT_APP_ICON}"
      "$<TARGET_BUNDLE_CONTENT_DIR:bongo_cat>/Resources/icon.icns"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/assets"
      "$<TARGET_BUNDLE_CONTENT_DIR:bongo_cat>/Resources/assets"
    COMMAND ${CMAKE_COMMAND} -E rm -f
      "$<TARGET_BUNDLE_CONTENT_DIR:bongo_cat>/Resources/assets/models/LICENSE"
    VERBATIM)
endif()

if(WIN32)
  target_sources(bongo_cat_runtime PRIVATE
    src/platform/windows/windows_keys.c)
  add_executable(bongo_cat_asset_packer src/tools/asset_packer.c)
  target_compile_definitions(bongo_cat_asset_packer PRIVATE
    _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(bongo_cat_asset_packer PRIVATE bongo_cat_warnings
    bongo_cat_archive)
  set(BONGO_CAT_ASSET_STAGE "${CMAKE_CURRENT_BINARY_DIR}/embedded-assets")
  set(BONGO_CAT_ASSET_PACK "${CMAKE_CURRENT_BINARY_DIR}/bongocat-assets.pak")
  file(GLOB_RECURSE BONGO_CAT_ASSET_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/assets/*")
  bongo_cat_configure_embedded_cubism_assets()
  # 渲染组件也进内嵌资源包(cores/bongo_live2d.dll): 发行形态因此回到"只发单个 exe"。
  bongo_cat_configure_embedded_renderer_component()
  add_custom_command(OUTPUT "${BONGO_CAT_ASSET_PACK}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${BONGO_CAT_ASSET_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "${BONGO_CAT_ASSET_STAGE}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/assets"
      "${BONGO_CAT_ASSET_STAGE}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy
      "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
      "${BONGO_CAT_ASSET_STAGE}/assets/LICENSE"
    ${BONGO_CAT_ASSET_EXTRA_COMMANDS}
    # 打包器吃的根目录是**暂存根**, 不再是 <暂存根>/assets: ROOT 下的结构就是包内结构
    # (assets/ 解到资源缓存, cores/ 按需释放到 <存储根>/cores)。
    COMMAND "$<TARGET_FILE:bongo_cat_asset_packer>"
      "${BONGO_CAT_ASSET_STAGE}" "${BONGO_CAT_ASSET_PACK}"
    DEPENDS ${BONGO_CAT_ASSET_INPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
      bongo_cat_asset_packer ${BONGO_CAT_ASSET_EXTRA_DEPENDS}
    VERBATIM)
  add_custom_target(bongo_cat_asset_pack DEPENDS "${BONGO_CAT_ASSET_PACK}")
  # 内嵌资源里也放一份编译好的 Cubism Vulkan 着色器, 所以先让 FrameworkShaders
  # 目标把 .spv 生成出来, 否则打包时会拷到一个还不存在的目录。
  if(TARGET FrameworkShaders)
    add_dependencies(bongo_cat_asset_pack FrameworkShaders)
  endif()
  if(TARGET bongo_live2d)
    add_dependencies(bongo_cat_asset_pack bongo_live2d)
  endif()
  file(TO_CMAKE_PATH "${BONGO_CAT_ASSET_PACK}" BONGO_CAT_ASSET_PACK_RC)
  file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/icon.ico"
    BONGO_CAT_ICON_RC)
  set(BONGO_CAT_WINDOWS_MANIFEST
    "${CMAKE_CURRENT_BINARY_DIR}/windows.manifest")
  configure_file(cmake/windows.manifest.in
    "${BONGO_CAT_WINDOWS_MANIFEST}" @ONLY)
  if(MSVC)
    set(BONGO_CAT_MANIFEST_RESOURCE "")
    target_sources(bongo_cat PRIVATE "${BONGO_CAT_WINDOWS_MANIFEST}")
  else()
    file(TO_CMAKE_PATH "${BONGO_CAT_WINDOWS_MANIFEST}"
      BONGO_CAT_MANIFEST_RC)
    set(BONGO_CAT_MANIFEST_RESOURCE
      "1 RT_MANIFEST \"${BONGO_CAT_MANIFEST_RC}\"")
  endif()
  configure_file(cmake/windows_resources.rc.in
    "${CMAKE_CURRENT_BINARY_DIR}/windows_resources.rc" @ONLY)
  set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/windows_resources.rc"
    APPEND PROPERTY OBJECT_DEPENDS "${BONGO_CAT_ASSET_PACK}")
  target_sources(bongo_cat PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/windows_resources.rc")
  add_dependencies(bongo_cat bongo_cat_asset_pack)
  set_target_properties(bongo_cat PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
