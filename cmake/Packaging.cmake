install(TARGETS bongo_cat
  RUNTIME DESTINATION . COMPONENT Runtime
  BUNDLE DESTINATION . COMPONENT Runtime)
if(WIN32)
  install(FILES LICENSE DESTINATION . COMPONENT Runtime)
  install(FILES resources/assets/models/LICENSE DESTINATION assets/models COMPONENT Runtime)
endif()
set(BONGO_CAT_PACKAGE_PRODUCT "BongoCat")
# 安装器/卸载列表里给人看的名字, 刻意与 BONGO_CAT_PACKAGE_PRODUCT 分开: 后者是发布包
# 文件名的一段 (BongoCat-1.16.0-windows-x64-...), 属于 CI 打包脚本与客户端
# update_release.c 的 read_assets() 之间的跨仓库契约, 改了老客户端就找不到更新资源。
set(BONGO_CAT_DISPLAY_NAME "BongoCat Turbo")
if(NOT BONGO_CAT_CUBISM_ENABLED)
  set(BONGO_CAT_PACKAGE_PRODUCT "BongoCat-Diagnostic")
  install(FILES cmake/DiagnosticBuildNotice.txt DESTINATION .
    COMPONENT Runtime)
endif()
if(UNIX AND NOT APPLE)
  install(DIRECTORY resources/assets DESTINATION . COMPONENT Runtime
    PATTERN "LICENSE" EXCLUDE)
endif()

include(cmake/PackagingPlatform.cmake)

set(BONGO_CAT_PACKAGE_NAME
  "${BONGO_CAT_PACKAGE_PRODUCT}-${PROJECT_VERSION}-${BONGO_CAT_PACKAGE_PLATFORM}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/bongocat-package-name.txt"
  CONTENT "${BONGO_CAT_PACKAGE_NAME}\n")

set(CPACK_PACKAGE_NAME "${BONGO_CAT_PACKAGE_PRODUCT}")
set(CPACK_PACKAGE_VENDOR "vladelaina")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "${BONGO_CAT_PACKAGE_NAME}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/dist")
set(CPACK_PACKAGE_CHECKSUM "SHA256")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_INSTALL_CMAKE_PROJECTS
  "${CMAKE_BINARY_DIR};${PROJECT_NAME};Runtime;/")

if(WIN32)
  # 便携版就是**那一个 exe**: 渲染组件已经内嵌进 exe 的资源包里, 首次启动按需释放到
  # <存储根>/cores/ (见 src/platform/windows/windows_assets.c 的 release_component 与
  # cmake/CubismAssets.cmake 的 bongo_cat_configure_embedded_renderer_component)。
  # 所以 2f 那个"含 cores/ 的 zip"被撤掉了 —— 单文件才是用户要的发布形态, 也少一层
  # "解压到哪、目录里还有别的什么"的解释成本。
  #
  # 保留 package-portable 这个目标名: CI/发布脚本与 update.h 里的自动更新载荷都按名字引用它,
  # 改名的收益只是好看一点, 代价是动到跨脚本契约。
  # 只拷 exe 本身, **不**拷 <构建目录>/cores/(那是开发期 stage 出来的副本): 目录里可能有
  # .staging(导入过程中的临时区)、旧版本组件之类不该进发行物的东西, 而且它会盖掉内嵌释放
  # 这条路径 —— 便携版里出现一个磁盘上的 cores/ 会让"组件到底从哪来"变得不可判定。
  set(BONGO_CAT_PORTABLE_BINARY
    "${CMAKE_BINARY_DIR}/dist/${BONGO_CAT_PACKAGE_NAME}-portable.exe")
  add_custom_command(OUTPUT "${BONGO_CAT_PORTABLE_BINARY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bongo_cat>"
      "${BONGO_CAT_PORTABLE_BINARY}"
    DEPENDS bongo_cat
    COMMENT "Building the BongoCat Windows portable executable (single file)"
    VERBATIM)
  add_custom_target(package-portable
    DEPENDS "${BONGO_CAT_PORTABLE_BINARY}")
endif()

include(CPack)
include(cmake/PackagingAppImage.cmake)
if(WIN32)
  include(cmake/PackagingInno.cmake)
endif()
