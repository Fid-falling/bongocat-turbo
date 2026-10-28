if(BUILD_TESTING)
  add_executable(bongo_cat_input_concurrent_tests tests/core/test_input_concurrent.c)
  target_include_directories(bongo_cat_input_concurrent_tests PRIVATE tests/support)
  target_link_libraries(bongo_cat_input_concurrent_tests PRIVATE
    bongo_cat_core SDL3::SDL3-static bongo_cat_warnings)
  add_test(NAME input-concurrent COMMAND bongo_cat_input_concurrent_tests)
  set_tests_properties(input-concurrent PROPERTIES TIMEOUT 30)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(bongo_cat_linux_evdev_tests tests/platform/test_linux_evdev.c)
    target_include_directories(bongo_cat_linux_evdev_tests PRIVATE
      src/platform/linux tests/support)
    target_link_libraries(bongo_cat_linux_evdev_tests PRIVATE
      bongo_cat_runtime bongo_cat_warnings)
    add_test(NAME linux-evdev COMMAND bongo_cat_linux_evdev_tests)
    set_tests_properties(linux-evdev PROPERTIES TIMEOUT 15)
    add_executable(bongo_cat_linux_window_tests tests/platform/test_linux_window.c)
    target_include_directories(bongo_cat_linux_window_tests PRIVATE
      ${BONGO_CAT_RUNTIME_INTERNAL_INCLUDE_DIRS} tests/support)
    target_link_libraries(bongo_cat_linux_window_tests PRIVATE
      bongo_cat_runtime bongo_cat_warnings)
    add_test(NAME linux-window COMMAND bongo_cat_linux_window_tests
      --ci-smoke --ci-ignore-global-input
      "--storage-root=${CMAKE_CURRENT_BINARY_DIR}/linux-window-data")
    set_tests_properties(linux-window PROPERTIES TIMEOUT 30
      ENVIRONMENT "BONGOCAT_ENABLE_EVDEV=0;BONGO_CAT_DISABLE_NEARBY_MODEL_SCAN=1")
  endif()
  add_executable(bongo_cat_image_filter_tests tests/media/test_image_filter.c)
  target_include_directories(bongo_cat_image_filter_tests PRIVATE
    src/media tests/support)
  target_include_directories(bongo_cat_image_filter_tests SYSTEM PRIVATE
    ${BONGO_CAT_STB_INCLUDE_DIR})
  target_link_libraries(bongo_cat_image_filter_tests PRIVATE
    bongo_cat_runtime bongo_cat_warnings)
  add_test(NAME image-filter COMMAND bongo_cat_image_filter_tests)
  if(APPLE)
    add_test(NAME macos-bundle-signature COMMAND /usr/bin/codesign
      --verify --strict --deep --verbose=2 "$<TARGET_BUNDLE_DIR:bongo_cat>")
  endif()

  add_executable(bongo_cat_hover_fade_tests
    tests/core/test_hover_fade.c src/runtime/input/mouse.c
    src/runtime/shell/modal_frame.c)
  target_include_directories(bongo_cat_hover_fade_tests PRIVATE
    "${BONGO_CAT_GENERATED_INCLUDE_DIR}" include tests/support
    ${BONGO_CAT_RUNTIME_INTERNAL_INCLUDE_DIRS})
  target_link_libraries(bongo_cat_hover_fade_tests PRIVATE
    SDL3::SDL3-static bongo_cat_warnings)
  add_test(NAME hover-fade COMMAND bongo_cat_hover_fade_tests)

  add_executable(bongo_cat_audio_tests tests/media/test_audio.c)
  target_include_directories(bongo_cat_audio_tests PRIVATE src/media/audio tests/support)
  target_link_libraries(bongo_cat_audio_tests PRIVATE bongo_cat_runtime bongo_cat_warnings)
  target_compile_definitions(bongo_cat_audio_tests PRIVATE
    BONGO_CAT_NATIVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME audio COMMAND bongo_cat_audio_tests)

  add_executable(bongo_cat_log_policy_tests tests/core/test_log_policy.c)
  target_link_libraries(bongo_cat_log_policy_tests PRIVATE
    bongo_cat_runtime bongo_cat_warnings)
  add_test(NAME log-policy COMMAND bongo_cat_log_policy_tests)

  # 一键更新的下载阶段: 起一个只在 127.0.0.1 监听的假镜像, 验证多源回退/记住可用源/
  # 哈希不匹配即拒绝。不碰线上资产 (feed 由 BONGO_CAT_UPDATE_FEED_URL 指到本机)。
  add_executable(bongo_cat_update_download_tests
    tests/runtime/test_update_download.c)
  target_include_directories(bongo_cat_update_download_tests PRIVATE
    ${BONGO_CAT_RUNTIME_INTERNAL_INCLUDE_DIRS} tests/support)
  target_link_libraries(bongo_cat_update_download_tests PRIVATE
    bongo_cat_runtime bongo_cat_warnings
    $<$<PLATFORM_ID:Windows>:ws2_32>)
  if(WIN32)
    # CMake 把子项目源文件按 C++ 判断时, tests/runtime 里的 C11 原子需要这个开关
    target_compile_options(bongo_cat_update_download_tests PRIVATE
      $<$<C_COMPILER_ID:MSVC>:/experimental:c11atomics>)
  endif()
  add_test(NAME update-download COMMAND bongo_cat_update_download_tests)
  set_tests_properties(update-download PROPERTIES TIMEOUT 180)

  # 自动替换链路 (--apply-update): 等 PID -> 换文件 -> 用新文件重启, 以及校验不通过/目录
  # 不可写时必须安全失败。全部在临时目录里用构建产物与系统 cmd.exe 扮演新旧版本, 不碰
  # 用户数据 (见脚本头部)。需要 pwsh: 没装就跳过这条用例, 不影响其它测试。
  if(WIN32)
    find_program(BONGO_CAT_PWSH_EXECUTABLE pwsh)
    if(BONGO_CAT_PWSH_EXECUTABLE)
      add_test(NAME windows-update-self-replace COMMAND
        ${BONGO_CAT_PWSH_EXECUTABLE} -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/platform/test_update_self_replace.ps1"
        -Executable "$<TARGET_FILE:bongo_cat>")
      set_tests_properties(windows-update-self-replace PROPERTIES TIMEOUT 300)
    endif()
  endif()

  add_executable(bongo_cat_core_tests
    tests/core/test_main.c
    tests/core/test_config.c
    tests/core/test_config_validation.c
    tests/core/test_language.c
    tests/core/test_input.c
    tests/core/test_models.c
    tests/core/test_mver_pointer.c
    tests/core/test_shortcut.c
    tests/core/test_sound_shortcut.c
    tests/core/test_sound.c
    tests/core/test_update.c
    src/platform/windows/windows_keys.c)
  target_link_libraries(bongo_cat_core_tests PRIVATE
    bongo_cat_core bongo_cat_warnings $<$<PLATFORM_ID:Windows>:user32>)
  target_include_directories(bongo_cat_core_tests PRIVATE
    tests/support
    $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows>)
  target_compile_definitions(bongo_cat_core_tests PRIVATE
    BONGO_CAT_NATIVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME core COMMAND bongo_cat_core_tests)

  add_executable(bongo_cat_i18n_tests tests/i18n/test_i18n.c)
  target_link_libraries(bongo_cat_i18n_tests PRIVATE
    bongo_cat_core bongo_cat_warnings)
  target_compile_definitions(bongo_cat_i18n_tests PRIVATE
    BONGO_CAT_NATIVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME i18n COMMAND bongo_cat_i18n_tests)

  add_executable(bongo_cat_ui_tests
    tests/ui/test_nuklear.c
    src/ui/backend/nuklear_impl.c
    src/ui/rendering/ui_paint_border.c)
  target_include_directories(bongo_cat_ui_tests PRIVATE
    src/ui/backend src/ui/rendering)
  target_include_directories(bongo_cat_ui_tests SYSTEM PRIVATE
    ${BONGO_CAT_NUKLEAR_INCLUDE_DIR})
  target_link_libraries(bongo_cat_ui_tests PRIVATE bongo_cat_warnings)
  add_test(NAME ui COMMAND bongo_cat_ui_tests)

  # 逻辑/像素尺寸换算: 锁死 "像素 = 逻辑 x 布局缩放" (200% 下 564x493 -> 282x247)。
  # 纯算术, 不建窗口也不初始化 SDL, 所以能进默认测试集。
  add_executable(bongo_cat_ui_scale_tests tests/ui/test_ui_scale.c)
  target_include_directories(bongo_cat_ui_scale_tests PRIVATE
    ${BONGO_CAT_RUNTIME_INTERNAL_INCLUDE_DIRS})
  target_include_directories(bongo_cat_ui_scale_tests SYSTEM PRIVATE
    ${BONGO_CAT_NUKLEAR_INCLUDE_DIR})
  target_link_libraries(bongo_cat_ui_scale_tests PRIVATE
    bongo_cat_runtime bongo_cat_warnings)
  add_test(NAME ui-scale COMMAND bongo_cat_ui_scale_tests)

  if(APPLE)
    # The refresh helper runs the real page code with the platform read replaced
    # per target, so the repaint decision is tested without touching TCC.
    add_executable(bongo_cat_input_monitoring_refresh_tests
      tests/ui/test_input_monitoring_refresh.c
      src/ui/preferences/preferences_pages.c)
    target_compile_definitions(bongo_cat_input_monitoring_refresh_tests PRIVATE
      bongo_cat_platform_input_monitoring_authorized=bongo_cat_test_input_monitoring_authorized)
    target_include_directories(bongo_cat_input_monitoring_refresh_tests PRIVATE
      ${BONGO_CAT_RUNTIME_INTERNAL_INCLUDE_DIRS})
    target_include_directories(bongo_cat_input_monitoring_refresh_tests SYSTEM PRIVATE
      ${BONGO_CAT_NUKLEAR_INCLUDE_DIR})
    target_link_libraries(bongo_cat_input_monitoring_refresh_tests PRIVATE
      bongo_cat_runtime bongo_cat_warnings)
    add_test(NAME macos-input-monitoring-refresh
      COMMAND bongo_cat_input_monitoring_refresh_tests)
  endif()

  add_executable(bongo_cat_app_state_tests
    tests/core/test_app_state.c src/core/app_state.c
    src/runtime/model/model_behavior_state.c)
  target_link_libraries(bongo_cat_app_state_tests PRIVATE bongo_cat_warnings)
  target_include_directories(bongo_cat_app_state_tests PRIVATE
    "${BONGO_CAT_GENERATED_INCLUDE_DIR}" include tests/support)
  add_test(NAME app-state COMMAND bongo_cat_app_state_tests)

  set(BONGO_CAT_MVER_IMPORT_TEST_SOURCES
    tests/model_import/test_mver_audio.c
    tests/model_import/test_mver_import.c
    tests/model_import/test_model_import_source.c
    tests/model_import/test_model_import_archive.c
    tests/model_import/test_mver_manifest.c
    tests/model_import/test_tauri_portable.c
    tests/model_import/test_mver_container.c
    tests/model_import/test_mver_missing_motion.c
    tests/model_import/test_mver_pointer_import.c
    tests/ui/test_preferences_text.c
    tests/model_import/test_mver_nearby_identity.c
    tests/model_import/test_mver_policy.c
    tests/model_import/test_model_import_identity.c
    tests/model_import/test_slim_package.c
    tests/model_import/test_mver_support.c)
  add_executable(bongo_cat_mver_import_tests
    ${BONGO_CAT_MVER_IMPORT_TEST_SOURCES})
  target_include_directories(bongo_cat_mver_import_tests PRIVATE
    ${BONGO_CAT_RUNTIME_INTERNAL_INCLUDE_DIRS})
  target_include_directories(bongo_cat_mver_import_tests SYSTEM PRIVATE
    ${BONGO_CAT_NUKLEAR_INCLUDE_DIR} ${BONGO_CAT_STB_INCLUDE_DIR})
  target_link_libraries(bongo_cat_mver_import_tests PRIVATE
    bongo_cat_runtime bongo_cat_archive)
  target_compile_definitions(bongo_cat_mver_import_tests PRIVATE
    BONGO_CAT_NATIVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  if(MSVC)
    set_property(SOURCE ${BONGO_CAT_MVER_IMPORT_TEST_SOURCES}
      APPEND PROPERTY COMPILE_OPTIONS "/experimental:c11atomics")
    set_property(SOURCE tests/platform/test_windows_capture.c APPEND PROPERTY
      COMPILE_OPTIONS "/experimental:c11atomics")
  endif()
  add_test(NAME model-import-unit COMMAND bongo_cat_mver_import_tests)

  add_executable(bongo_cat_preferences_lifecycle_tests
    tests/ui/test_preferences_lifecycle.c)
  target_include_directories(bongo_cat_preferences_lifecycle_tests PRIVATE
    ${BONGO_CAT_RUNTIME_INTERNAL_INCLUDE_DIRS})
  target_include_directories(bongo_cat_preferences_lifecycle_tests SYSTEM PRIVATE
    ${BONGO_CAT_NUKLEAR_INCLUDE_DIR})
  target_link_libraries(bongo_cat_preferences_lifecycle_tests PRIVATE bongo_cat_runtime)
  if(WIN32)
    target_sources(bongo_cat_preferences_lifecycle_tests PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}/windows_resources.rc")
    add_dependencies(bongo_cat_preferences_lifecycle_tests bongo_cat_asset_pack)
  else()
    add_custom_command(TARGET bongo_cat_preferences_lifecycle_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/assets"
        "$<TARGET_FILE_DIR:bongo_cat_preferences_lifecycle_tests>/assets")
  endif()
  bongo_cat_stage_cubism_assets(bongo_cat_preferences_lifecycle_tests)
  if(MSVC)
    target_compile_options(bongo_cat_preferences_lifecycle_tests PRIVATE
      /experimental:c11atomics)
  endif()
  add_test(NAME preferences-lifecycle COMMAND bongo_cat_preferences_lifecycle_tests
    --ci-smoke --ci-ignore-global-input
    "--storage-root=${CMAKE_CURRENT_BINARY_DIR}/preferences-lifecycle-data")
  set_tests_properties(preferences-lifecycle PROPERTIES
    ENVIRONMENT "BONGO_CAT_DISABLE_NEARBY_MODEL_SCAN=1" TIMEOUT 60)
  if(APPLE)
    set_tests_properties(preferences-lifecycle PROPERTIES DISABLED TRUE)
  endif()

  # Core 导入链的坏输入矩阵。**刻意不放进上面的 BONGO_CAT_CUBISM_ENABLED 分支**: 它测的是
  # "用户自备 Core 的导入路径", 与本次构建有没有编 Cubism 无关(诊断构建同样要能安全导入),
  # 而且它需要 miniz 构造恶意 zip, 所以单独链 bongo_cat_archive。
  add_executable(bongo_cat_live2d_core_tests tests/live2d/test_core_import.c)
  target_include_directories(bongo_cat_live2d_core_tests PRIVATE src/live2d)
  target_link_libraries(bongo_cat_live2d_core_tests PRIVATE
    bongo_cat_runtime bongo_cat_archive bongo_cat_warnings)
  add_test(NAME live2d-core-import COMMAND bongo_cat_live2d_core_tests)
  set_tests_properties(live2d-core-import PROPERTIES TIMEOUT 60)

  # 组件 ABI 的坏描述符矩阵: 需要一个"会说谎的组件"作为夹具(只在测试里构建, 不进发行包)。
  # 它导出 bongo_cat_renderer_module_query, 所以是一个**真的**动态库 —— 校验代码走的是与
  # 生产完全相同的路径(而不是测试里另写一套)。
  add_library(bongo_cat_test_component SHARED tests/plugin/test_component.c)
  # 与生产组件一样, 夹具只拿公共头 + 生成头(version.h 是配置期生成的), 不链宿主运行时 ——
  # 这样"组件不能依赖宿主内部符号"这条纪律在测试里也是真的。
  target_include_directories(bongo_cat_test_component PRIVATE include
    "${BONGO_CAT_GENERATED_INCLUDE_DIR}")
  # 但它**必须**能看见 yyjson 的头: renderer_module_abi.h 会把 bongo_cat/json.h 带进来, 而后者
  # `#include <yyjson.h>`, 那是个第三方头, 不在公共 include 里。生产组件链的就是 yyjson
  # (见 RendererModule.cmake), 夹具照抄同一份头可见性 —— 少了它夹具连编译都过不去。
  # (仍然不链宿主运行时: 加的是第三方头库, 不是宿主的内部实现。)
  target_link_libraries(bongo_cat_test_component PRIVATE yyjson bongo_cat_warnings)
  set_target_properties(bongo_cat_test_component PROPERTIES
    PREFIX "" OUTPUT_NAME "bongo_test_component")
  add_executable(bongo_cat_renderer_module_abi_tests
    tests/plugin/test_renderer_module_abi.c)
  target_link_libraries(bongo_cat_renderer_module_abi_tests PRIVATE
    bongo_cat_runtime bongo_cat_warnings)
  if(MSVC)
    # 与其它链 bongo_cat_runtime 的 C 测试一样: 运行时头里有 C11 原子, MSVC 需要显式开
    target_compile_options(bongo_cat_renderer_module_abi_tests PRIVATE
      $<$<C_COMPILER_ID:MSVC>:/experimental:c11atomics>)
  endif()
  # 夹具路径用**命令行参数**传, 不用编译期宏: Windows 原生路径塞进 C 字符串字面量会把
  # \b \t \R 变成转义序列(踩过: warning C4129 -> /WX 直接失败)。
  add_dependencies(bongo_cat_renderer_module_abi_tests bongo_cat_test_component)
  add_test(NAME renderer-module-abi COMMAND bongo_cat_renderer_module_abi_tests
    "$<TARGET_FILE:bongo_cat_test_component>")
  set_tests_properties(renderer-module-abi PROPERTIES TIMEOUT 30)

  if(BONGO_CAT_CUBISM_ENABLED)
    add_executable(bongo_cat_core_profile_tests tests/live2d/test_core_profile.cpp)
    target_include_directories(bongo_cat_core_profile_tests PRIVATE src/live2d)
    target_link_libraries(bongo_cat_core_profile_tests PRIVATE
      bongo_cat_runtime bongo_cat_warnings)
    add_test(NAME live2d-core-profile COMMAND bongo_cat_core_profile_tests)
    set_tests_properties(live2d-core-profile PROPERTIES TIMEOUT 30)

    add_test(NAME model-startup-recovery COMMAND ${CMAKE_COMMAND}
      "-DEXECUTABLE=$<TARGET_FILE:bongo_cat_preferences_lifecycle_tests>"
      "-DASSET_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/resources/assets"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/model-startup-recovery"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckModelStartupRecovery.cmake")
    set_tests_properties(model-startup-recovery PROPERTIES TIMEOUT 60)
    if(APPLE)
      set_tests_properties(model-startup-recovery PROPERTIES DISABLED TRUE)
    endif()
    add_executable(bongo_cat_motion_state_tests
      tests/live2d/test_motion_state.cpp)
    target_include_directories(bongo_cat_motion_state_tests PRIVATE
      src/live2d tests/support)
    # 它测的三条纯规则(motion_toggle_pair / motion_enables_state /
    # motion_run_clears_selection)现在**定义在 cubism_model.hpp 里**(inline): 2c 之后这批实现
    # 只编进渲染组件(隐藏可见性的 .dll), 测试既链接不到它、把实现 .cpp 编进来又会拖进整个
    # NativeModel 实现图。放头里两边共用同一份定义, 于是这个目标照旧只链 bongo_cat_runtime。
    target_link_libraries(bongo_cat_motion_state_tests PRIVATE
      bongo_cat_runtime bongo_cat_warnings)
    add_test(NAME live2d-motion-state COMMAND bongo_cat_motion_state_tests)
  endif()

  if(APPLE)
    add_executable(bongo_cat_macos_click_through_tests
      tests/platform/test_macos_click_through.m)
    target_link_libraries(bongo_cat_macos_click_through_tests PRIVATE
      bongo_cat_runtime bongo_cat_warnings)
    add_test(NAME macos-click-through COMMAND bongo_cat_macos_click_through_tests)
    set_tests_properties(macos-click-through PROPERTIES
      TIMEOUT 60 SKIP_RETURN_CODE 77)
  endif()

  if(WIN32)
    add_executable(bongo_cat_windows_input_tests
      tests/platform/test_windows_input.c
      tests/platform/test_windows_relative.c
      tests/platform/test_windows_mouse_mapping.c
      tests/platform/test_windows_pointer_detection.c
      tests/platform/test_windows_raw_receiver.c)
    if(MSVC)
      # Visual Studio can evaluate target language options as C++ with Cubism.
      set_property(SOURCE
        tests/platform/test_windows_input.c
        tests/platform/test_windows_relative.c
        tests/platform/test_windows_mouse_mapping.c
        tests/platform/test_windows_pointer_detection.c
        tests/platform/test_windows_raw_receiver.c
        APPEND PROPERTY COMPILE_OPTIONS "/experimental:c11atomics")
    endif()
    target_include_directories(bongo_cat_windows_input_tests PRIVATE
      tests/support src/platform/windows src/runtime/input)
    target_link_libraries(bongo_cat_windows_input_tests PRIVATE
      bongo_cat_runtime bongo_cat_warnings user32)
    add_test(NAME windows-input COMMAND bongo_cat_windows_input_tests)
    set_tests_properties(windows-input PROPERTIES TIMEOUT 30)

    add_executable(bongo_cat_windows_capture_tests
      tests/platform/test_windows_capture.c)
    target_include_directories(bongo_cat_windows_capture_tests PRIVATE
      src/platform/windows)
    target_link_libraries(bongo_cat_windows_capture_tests PRIVATE
      bongo_cat_runtime bongo_cat_warnings)
    add_test(NAME windows-capture COMMAND bongo_cat_windows_capture_tests)
  endif()

  if(UNIX AND NOT APPLE)
    target_link_libraries(bongo_cat_ui_tests PRIVATE m)
    target_link_libraries(bongo_cat_app_state_tests PRIVATE m)
  endif()

  if(APPLE)
    add_test(NAME macos-bundle-icon COMMAND ${CMAKE_COMMAND}
      "-DBUNDLE=$<TARGET_BUNDLE_DIR:bongo_cat>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckMacOSBundleIcon.cmake")
  endif()
endif()

# "跑 ctest 之前先把测试目标重建一遍"。
#
# 为什么需要它: ctest **只执行已经存在的**测试二进制, 它不构建任何东西。于是一个"只构建了主目标"
# 的构建目录里可能躺着很久以前的旧测试 exe —— 那时"测试其实编不过"或"断言早已过时"在本地看起来
# 都是**通过**。这不是假设: 阶段 2c 把 Live2D 实现整体搬进渲染组件之后, 有三条测试(两个链接失败 +
# 一个断言过时)就一直被陈旧二进制掩盖着, 直到有人在全新构建目录里跑 ctest 才暴露出来。
#
# 这里用 BUILDSYSTEM_TARGETS 自动收集本目录下所有 bongo_cat_*_tests 目标, 所以将来新增测试不用
# 记得回来改这个列表; CI 的 Windows 测试步骤在 ctest 之前调用它(见 .github/workflows/ci.yml),
# 任何一个测试目标编不过都会在那里直接变红。
get_property(_bongo_cat_buildsystem_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(_bongo_cat_test_targets "")
foreach(_bongo_cat_target IN LISTS _bongo_cat_buildsystem_targets)
  if(_bongo_cat_target MATCHES "^bongo_cat_.*_tests$")
    list(APPEND _bongo_cat_test_targets "${_bongo_cat_target}")
  endif()
endforeach()
if(_bongo_cat_test_targets)
  add_custom_target(build-tests DEPENDS ${_bongo_cat_test_targets})
  list(LENGTH _bongo_cat_test_targets _bongo_cat_test_count)
  message(STATUS "Test rebuild target 'build-tests' covers ${_bongo_cat_test_count} test target(s)")
endif()
