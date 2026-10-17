cmake_minimum_required(VERSION 3.24)
set(BONGO_CAT_VERSION "1.17.1")
project(BongoCat VERSION ${BONGO_CAT_VERSION} DESCRIPTION "BongoCat Turbo" HOMEPAGE_URL "https://github.com/Fid-falling/bongocat-turbo" LANGUAGES C CXX)
include(cmake/Version.cmake)
include(FetchContent)
include(CTest)
option(BONGO_CAT_FETCH_DEPS "Download pinned open-source dependencies" ON)
option(BONGO_CAT_REQUIRE_CUBISM "Fail configuration when Cubism SDK is absent" OFF)
option(BONGO_CAT_WARNINGS_AS_ERRORS "Treat native source warnings as errors" OFF)
option(BONGO_CAT_ENABLE_VULKAN "Build the optional Vulkan rendering backend" OFF)
# 法律边界开关(见 LIVE2D-PLUGIN-PLAN.md §9.1): Live2D Cubism Core 是 Live2D Inc. 的专有库,
# 我们不随包分发。Core 只能由用户自行从官网取得动态库, 运行期经宿主服务表 core_symbol 解析。
# 这个 option 存在的意义是把"有人想回到静态链接"变成一个显式的、会失败的配置 ——
# 关掉它没有"降级"语义, 只有"违反许可"语义, 所以直接 FATAL_ERROR(照 BongoCat-X 的做法)。
option(BONGO_CAT_RUNTIME_CORE
  "由用户自备 Cubism Core 动态库并在运行期加载(发行物不得含 Core)" ON)
if(NOT BONGO_CAT_RUNTIME_CORE)
  message(FATAL_ERROR
    "BONGO_CAT_RUNTIME_CORE=OFF 不受支持: 宿主与渲染插件都不得内嵌或静态链接 "
    "Live2D Cubism Core(专有库)。请在运行期加载用户自备的 Core。")
endif()
# 发行物是否必须能提供我们自己的 Live2D 渲染组件。两种形态都算"有":
#   * 组件文件随包分发(cores/bongo_live2d.*, Linux/macOS 发行包);
#   * 组件内嵌在 exe 的资源包里, 首次启动按需释放到 <存储根>/cores/(Windows 单文件形态)。
# 有 Cubism SDK 的构建里它被打开(见下面的 SDK 分支); 无 SDK 的诊断构建里组件本就不存在,
# 所以那里的审计不会要求它 —— 否则诊断构建会被自己的审计挡住。
option(BONGO_CAT_REQUIRE_LIVE2D_COMPONENT
  "发行产物必须能提供 Live2D 渲染组件(包内文件或内嵌)" OFF)
include(cmake/CubismCompliance.cmake)
set(BONGO_CAT_CUBISM_SDK "${CMAKE_CURRENT_SOURCE_DIR}/vendor/CubismSdkForNative"
  CACHE PATH "Path to Cubism SDK for Native")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(cmake/ReleaseOptimize.cmake)
add_library(bongo_cat_warnings INTERFACE)
if(MSVC)
  target_compile_options(bongo_cat_warnings INTERFACE /W4 /permissive- /utf-8)
  target_compile_options(bongo_cat_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
  if(BONGO_CAT_WARNINGS_AS_ERRORS)
    target_compile_options(bongo_cat_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(bongo_cat_warnings INTERFACE -Wall -Wextra)
  if(BONGO_CAT_WARNINGS_AS_ERRORS)
    target_compile_options(bongo_cat_warnings INTERFACE -Werror)
  endif()
endif()
include(cmake/Dependencies.cmake)
include(cmake/AudioDecoder.cmake)
find_package(OpenGL REQUIRED)
if(BONGO_CAT_ENABLE_VULKAN)
  find_package(Vulkan REQUIRED)
  message(STATUS "Vulkan backend enabled (headers: ${Vulkan_INCLUDE_DIRS})")
  include(cmake/Vulkan.cmake)
  bongo_cat_embed_vulkan_shaders(
    "${BONGO_CAT_GENERATED_INCLUDE_DIR}/bongo_cat_vk_shaders.h")
endif()
# 用户自备 Core 的落点解析与安全导入: 与是否编译 Cubism 无关 —— 缺 SDK 的诊断构建
# 同样要能接收用户导入的 Core, 而且"包内无 Core"的守卫要靠它。单独成一个变量: 下面的
# Cubism 分支会**替换** BONGO_CAT_LIVE2D_SOURCES(诊断实现与真实实现是同一组符号, 两个
# 一起编会撞定义), 而这两个文件在两种构建里都必须存在。
set(BONGO_CAT_LIVE2D_CORE_SOURCES
  src/live2d/live2d_core_paths.c src/live2d/live2d_core_import.c
  # 压缩包侧的安全规则单独成文件(不可信输入), 见 live2d_core_internal.h
  src/live2d/live2d_core_archive.c)
set(BONGO_CAT_LIVE2D_SOURCES src/live2d/live2d_stub.c
  ${BONGO_CAT_LIVE2D_CORE_SOURCES})
set(BONGO_CAT_CUBISM_ENABLED OFF)
set(CUBISM_CORE_HEADER "${BONGO_CAT_CUBISM_SDK}/Core/include/Live2DCubismCore.h")
if(EXISTS "${CUBISM_CORE_HEADER}")
  if(WIN32 AND NOT MSVC)
    if(BONGO_CAT_REQUIRE_CUBISM)
      message(FATAL_ERROR "Cubism Core for Windows requires Visual Studio 2022")
    endif()
    message(WARNING "Cubism SDK found but MinGW is unsupported; using diagnostic backend")
  else()
    include(cmake/Cubism.cmake)
    # "Cubism 实现"这一组源文件有两个消费者: 宿主的诊断/过渡实现, 以及渲染组件
    # (cores/bongo_live2d.*)。**Core 的落点解析与导入不在这一组里** —— 那是宿主独占的职责
    # (组件连 Core 文件都不该打开), 所以两个列表必须分开, 不能一股脑都塞给组件。
    set(BONGO_CAT_CUBISM_IMPLEMENTATION_SOURCES
      src/live2d/cubism_bridge.cpp src/live2d/cubism_bridge_load.cpp
      src/live2d/cubism_bridge_load_session.cpp
      src/live2d/cubism_bridge_visual.cpp
      src/live2d/cubism_expression.cpp
      src/live2d/cubism_model_load.cpp
      src/live2d/cubism_model_renderer.cpp
      src/live2d/cubism_renderer_factory.cpp
      src/live2d/cubism_vulkan_bridge.cpp
      src/live2d/cubism_vulkan_texture.cpp
      src/live2d/cubism_vulkan_transition.cpp
      src/live2d/cubism_model_idle.cpp
      src/live2d/cubism_model_update.cpp
      src/live2d/cubism_parameter_overrides.cpp
      src/live2d/cubism_motion_playback.cpp
      src/live2d/cubism_motion_preview.cpp
      src/live2d/cubism_motion_selection.cpp
      src/live2d/cubism_motion_state.cpp src/live2d/cubism_viewer_look.cpp
      src/live2d/cubism_model_bounds.cpp
      src/live2d/cubism_model_viewport.cpp
      src/live2d/cubism_model_validate.cpp)
    # 宿主这一侧**不再编译这套实现**(阶段 2c): 真正渲染的是组件(cores/bongo_live2d.*),
    # 宿主只保留"分发层 + 诊断实现"。BONGO_CAT_CUBISM_IMPLEMENTATION_SOURCES 仍要保留, 因为
    # 组件那一侧编的就是它(见 cmake/RendererModule.cmake) —— 两条路径共用同一批源文件。
    set(BONGO_CAT_LIVE2D_SOURCES ${BONGO_CAT_LIVE2D_CORE_SOURCES}
      src/live2d/live2d_dispatch.c src/live2d/live2d_dispatch_vulkan.c
      src/live2d/live2d_stub.c)
    # 诊断实现与分发层提供的是**同一组符号**, 同时编就会撞定义。照上游的做法把诊断实现这一份
    # 改名(逐个函数加前缀), 于是分发层独占 `bongo_cat_live2d_*`, 回落到 `stub_live2d_*`。
    # 只在"有分发层"的构建里改名: 没有 SDK 的诊断构建里 stub 就是唯一实现, 保持原名。
    set(BONGO_CAT_LIVE2D_STUB_METHODS
      create destroy load ready canvas_size frame viewport overlay_viewport resize
      reshape update draw set_mirror set_vertical_flip set_render_options
      set_dragging set_centered_dragging set_parameter parameter start_motion
      restore_motion_state preview_motion restore_motion_preview
      commit_motion_preview motion_selected motion_persistent motion_visible
      motion_same_toggle set_expression expression visual_state
      prepare_cover_capture prepare_viewer_audit load_session_begin
      load_session_activate load_session_step load_session_end load_session_abort
      vulkan_attach vulkan_detach vulkan_ready vulkan_shaders_available
      vulkan_set_shader_root vulkan_set_supersample vulkan_begin_frame
      vulkan_end_frame vulkan_target_info vulkan_release_target
      vulkan_target_generation
      vulkan_create_texture vulkan_create_texture_attached
      vulkan_destroy_texture vulkan_bind_texture)
    set(BONGO_CAT_LIVE2D_STUB_DEFINITIONS "")
    foreach(method IN LISTS BONGO_CAT_LIVE2D_STUB_METHODS)
      if(method MATCHES "^vulkan_")
        list(APPEND BONGO_CAT_LIVE2D_STUB_DEFINITIONS
          "bongo_cat_live2d_${method}=stub_live2d_${method}")
      else()
        list(APPEND BONGO_CAT_LIVE2D_STUB_DEFINITIONS
          "bongo_cat_live2d_${method}=stub_live2d_${method}")
      endif()
    endforeach()
    set_source_files_properties(src/live2d/live2d_stub.c PROPERTIES
      COMPILE_DEFINITIONS "${BONGO_CAT_LIVE2D_STUB_DEFINITIONS}")
    # 有 Cubism SDK 的构建就必须有渲染组件: 它是 Live2D 的唯一实现, 缺了它包发出去用户也渲染不了。
    # 诊断构建(无 SDK)里组件本就不存在, 所以这个开关只在这里打开。
    set(BONGO_CAT_REQUIRE_LIVE2D_COMPONENT ON)
    set(BONGO_CAT_CUBISM_ENABLED ON)
  endif()
elseif(BONGO_CAT_REQUIRE_CUBISM)
  message(FATAL_ERROR "Cubism SDK missing at ${BONGO_CAT_CUBISM_SDK}")
else()
  message(WARNING "Cubism SDK missing; building the diagnostic backend")
endif()
add_library(bongo_cat_core STATIC
  src/core/config_defaults.c src/core/config_model_state.c src/core/config_session_defaults.c src/core/config_names.c src/core/config_fields.c src/core/config_io.c src/core/config_settings_save.c
  src/core/config_file.c src/core/config_session_io.c
  src/core/error.c
  src/core/file.c src/core/i18n.c
  src/core/input_state.c src/core/mver_pointer.c
  src/core/mouse_tracking.c src/core/sha256.c src/core/utf8.c
  src/core/model_catalog.c
  src/core/model_behavior.c src/core/model_json.c
  src/core/path.c src/core/path_io.c src/core/json.c src/core/shortcut.c
  src/core/sound_shortcut.c
  src/core/update_release.c src/core/update_version.c
  # 一键更新的纯逻辑: 哈希解析/校验 (#7) 与下载源列表 (镜像前缀/顺序/记住可用源)
  src/core/update_digest.c src/core/update_source.c
  src/core/update_error.c)
target_include_directories(bongo_cat_core PUBLIC "${BONGO_CAT_GENERATED_INCLUDE_DIR}" include)
target_link_libraries(bongo_cat_core PUBLIC yyjson PRIVATE bongo_cat_warnings)
include(cmake/CubismAssets.cmake)
if(WIN32)
  target_sources(bongo_cat_core PRIVATE
    src/platform/windows/windows_utf8.c)
  target_include_directories(bongo_cat_core PRIVATE
    src/platform/windows)
endif()

include(cmake/RuntimeSources.cmake)
add_library(bongo_cat_runtime STATIC
  ${BONGO_CAT_RUNTIME_SOURCES} ${BONGO_CAT_LIVE2D_SOURCES})
target_include_directories(bongo_cat_runtime PUBLIC "${BONGO_CAT_GENERATED_INCLUDE_DIR}" include)
target_include_directories(bongo_cat_runtime PRIVATE
  ${BONGO_CAT_RUNTIME_INTERNAL_INCLUDE_DIRS})
target_include_directories(bongo_cat_runtime SYSTEM PRIVATE
  ${BONGO_CAT_STB_INCLUDE_DIR} ${BONGO_CAT_NUKLEAR_INCLUDE_DIR})
target_link_libraries(bongo_cat_runtime PRIVATE bongo_cat_warnings bongo_cat_archive)
target_link_libraries(bongo_cat_runtime PUBLIC bongo_cat_core SDL3::SDL3-static OpenGL::GL ${BONGO_CAT_MINIAUDIO_TARGET}
  $<$<BOOL:${BONGO_CAT_ENABLE_VULKAN}>:Vulkan::Vulkan>)
if(MSVC)
  set_source_files_properties(src/media/stb_image_impl.c src/media/image_resize.c src/ui/backend/nuklear_impl.c
    PROPERTIES COMPILE_OPTIONS "/W0")
else()
  set_source_files_properties(src/media/stb_image_impl.c src/media/image_resize.c src/ui/backend/nuklear_impl.c
    PROPERTIES COMPILE_OPTIONS "-w")
endif()
if(BONGO_CAT_CUBISM_ENABLED)
  target_link_libraries(bongo_cat_runtime PUBLIC Framework)
  target_compile_definitions(bongo_cat_runtime PUBLIC BONGO_CAT_HAS_CUBISM=1)
endif()
if(BONGO_CAT_ENABLE_VULKAN)
  target_compile_definitions(bongo_cat_runtime PUBLIC BONGO_CAT_HAS_VULKAN=1)
endif()
target_compile_definitions(bongo_cat_runtime PRIVATE
  BONGO_CAT_DEV_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/assets")

# 渲染组件 cores/bongo_live2d.*: 只在真有 Cubism Framework 可编时才建 —— 缺 SDK 的诊断
# 构建里不存在这个组件, 宿主照常启动并走 live2d_stub.c 的兜底实现(这是降级路径的一部分)。
if(BONGO_CAT_CUBISM_ENABLED)
  include(cmake/RendererModule.cmake)
endif()

include(cmake/PlatformRuntime.cmake)
if(NOT WIN32)
  # dlopen/dlsym 版本的 Core 加载器(与 windows_live2d_core.c 语义一一对应)
  target_sources(bongo_cat_runtime PRIVATE src/live2d/live2d_core_posix.c)
  target_link_libraries(bongo_cat_runtime PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(bongo_cat src/main.c)
target_link_libraries(bongo_cat PRIVATE bongo_cat_runtime bongo_cat_warnings)
# 规则 1 的配置期检查(宿主/插件不得链接或内嵌 Core): 违规在迁移期只告警, 阶段 2 起硬失败。
bongo_cat_assert_no_cubism_core_link(bongo_cat)
bongo_cat_assert_no_cubism_core_link(bongo_cat_runtime)
set_target_properties(bongo_cat PROPERTIES OUTPUT_NAME "BongoCatTurbo")
bongo_cat_enable_release_ipo(Framework bongo_cat_core bongo_cat_runtime bongo_cat)

include(cmake/ValidationTools.cmake)
include(cmake/ApplicationResources.cmake)
if(MSVC AND BONGO_CAT_SDL_WINDOWS_TRIMMED)
  get_filename_component(BONGO_CAT_MSVC_TOOL_DIR "${CMAKE_LINKER}" DIRECTORY)
  find_program(BONGO_CAT_DUMPBIN dumpbin PATHS "${BONGO_CAT_MSVC_TOOL_DIR}" REQUIRED)
  add_custom_command(TARGET bongo_cat POST_BUILD
    COMMAND ${CMAKE_COMMAND} "-DEXECUTABLE=$<TARGET_FILE:bongo_cat>"
      "-DDUMPBIN=${BONGO_CAT_DUMPBIN}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckWindowsImports.cmake"
    COMMENT "Checking the final Windows executable's input capabilities"
    VERBATIM)
endif()
if(MSVC)
  set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_OPTIONS "/experimental:c11atomics")
  get_target_property(BONGO_CAT_RUNTIME_SOURCES bongo_cat_runtime SOURCES)
  foreach(BONGO_CAT_SOURCE IN LISTS BONGO_CAT_RUNTIME_SOURCES)
    if(BONGO_CAT_SOURCE MATCHES "\\.c$")
      set_property(SOURCE "${BONGO_CAT_SOURCE}" APPEND PROPERTY
        COMPILE_OPTIONS "/experimental:c11atomics")
    endif()
  endforeach()
endif()

bongo_cat_stage_cubism_assets(bongo_cat)

if(APPLE)
  # The linker signs the executable on its own before the bundle's resources are
  # copied in, which leaves strict verification failing with "code has no resources
  # but signature indicates they must be present". Re-sign once every resource and
  # executable change has landed, so this must stay the last POST_BUILD step.
  add_custom_command(TARGET bongo_cat POST_BUILD
    COMMAND /usr/bin/codesign --force --sign - "$<TARGET_BUNDLE_DIR:bongo_cat>"
    COMMENT "Ad-hoc signing the BongoCat bundle"
    VERBATIM)
endif()

add_custom_target(check-lines ALL
  COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckLines.cmake
  VERBATIM)
add_custom_target(check-localization ALL
  COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckLocalization.cmake
  VERBATIM)
add_custom_target(check-platform-runtime-safety ALL
  COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckPlatformRuntimeSafety.cmake
  VERBATIM)
add_custom_target(check-cubism-user-model-safety ALL
  COMMAND ${CMAKE_COMMAND}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckCubismUserModelSafety.cmake
  VERBATIM)
add_custom_target(check-cubism-compliance ALL
  COMMAND ${CMAKE_COMMAND}
    "-DBONGO_CAT_AUDIT_ROOT=$<TARGET_FILE_DIR:bongo_cat>"
    "-DBONGO_CAT_AUDIT_EXPECT_COMPONENT=${BONGO_CAT_REQUIRE_LIVE2D_COMPONENT}"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CubismCompliance.cmake
  COMMENT "Auditing the build output for Live2D Cubism Core leaks"
  VERBATIM)
add_dependencies(check-cubism-compliance bongo_cat)
include(cmake/Tests.cmake)
include(cmake/Packaging.cmake)
