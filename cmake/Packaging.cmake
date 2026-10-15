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
  # 便携版从"单个 exe"改成"含 cores/ 的 zip": Live2D 的渲染实现现在是随包分发的组件
  # (cores/bongo_live2d.dll), 只把 exe 拷走会让 Live2D 失去渲染能力。zip 里保留一层与包名
  # 同名的目录, 解压后不会把文件散在当前目录。
  set(BONGO_CAT_PORTABLE_ARCHIVE
    "${CMAKE_BINARY_DIR}/dist/${BONGO_CAT_PACKAGE_NAME}-portable.zip")
  set(BONGO_CAT_PORTABLE_STAGE
    "${CMAKE_BINARY_DIR}/portable/${BONGO_CAT_PACKAGE_NAME}")
  set(BONGO_CAT_PORTABLE_DEPENDS bongo_cat)
  if(TARGET bongo_live2d)
    list(APPEND BONGO_CAT_PORTABLE_DEPENDS bongo_live2d)
  endif()
  add_custom_command(OUTPUT "${BONGO_CAT_PORTABLE_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/portable"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${BONGO_CAT_PORTABLE_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BONGO_CAT_PORTABLE_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bongo_cat>"
      "${BONGO_CAT_PORTABLE_STAGE}/"
    # 只拷组件本身, **不**整目录拷 cores/: 那个目录里可能有 .staging(导入过程中的临时区,
    # 见 live2d_core.h)之类的瞬态内容, 不该进发行包。
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "${BONGO_CAT_PORTABLE_STAGE}/cores"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bongo_live2d>"
      "${BONGO_CAT_PORTABLE_STAGE}/cores/"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${BONGO_CAT_PORTABLE_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${BONGO_CAT_PORTABLE_ARCHIVE}"
      --format=zip -- "${BONGO_CAT_PACKAGE_NAME}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/portable"
    DEPENDS ${BONGO_CAT_PORTABLE_DEPENDS}
    COMMENT "Building the BongoCat Windows portable archive (exe + cores/)"
    VERBATIM)
  add_custom_target(package-portable
    DEPENDS "${BONGO_CAT_PORTABLE_ARCHIVE}")
endif()

include(CPack)
include(cmake/PackagingAppImage.cmake)
if(WIN32)
  include(cmake/PackagingInno.cmake)
endif()
