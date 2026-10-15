# Live2D 渲染组件: cores/bongo_live2d.*(阶段 2b)。
#
# 这里编出来的就是"随包分发的渲染实现": 内含 Cubism **Framework**(现场编译的源码)与我们的
# GL/VK 桥接, 但**不含 Core**(专有库由用户在运行期提供, csm* 符号经生成的 shim 转发)。
#
# 三条必须守住的纪律:
#   1. 组件**不链接 SDL**: SDL 的窗口/GL 上下文/日志回调都属于宿主进程里的那一份。组件若
#      自带一份, 它看到的上下文是空的 —— 渲染会直接失效。所以用 -FI 强制包含
#      component_sdl_shim.h, 把 SDL 调用改道到宿主服务表(见该头文件的说明)。
#   2. 组件**不链接 bongo_cat_core / bongo_cat_runtime**: 需要宿主能力就加服务, 不搬宿主的
#      内部实现(否则两份缓存/两份配置视图会各说各话)。
#   3. stage 到 <exe 目录>/cores/, 与宿主 exe 同级 —— 组件属于程序自身, 版本必须跟 exe 走。

set(BONGO_CAT_COMPONENT_SOURCES
  src/live2d/component/component_entry.cpp
  src/live2d/component/component_host.c
  # 组件自己的 bongo_cat_file_open(UTF-8 -> 宽字符): 语义必须与宿主那份一致, 否则带中文的
  # 用户目录会读不到模型。Windows 上复用平台层的 UTF-8 转换(那个文件不依赖 SDL)。
  src/live2d/component/component_file.c
  # 桥接(Framework 的调用方): 与宿主编译的是**同一批源文件** —— 这样"GL/VK 两条渲染路径"
  # 不会出现两份实现分叉。注意这里**不含** Core 的落点解析与导入(live2d_core_*.c):
  # 组件连 Core 文件都不该打开, 它只用宿主服务表给的 core_symbol。
  ${BONGO_CAT_CUBISM_IMPLEMENTATION_SOURCES})
if(WIN32)
  list(APPEND BONGO_CAT_COMPONENT_SOURCES src/platform/windows/windows_utf8.c)
endif()

add_library(bongo_live2d SHARED ${BONGO_CAT_COMPONENT_SOURCES})
if(WIN32)
  target_include_directories(bongo_live2d PRIVATE src/platform/windows)
endif()
# Framework 只提供编译产物: 它的 csm* 引用由生成的 shim 满足(见 cmake/GenCoreShim.cmake)。
# yyjson 是纯静态库(无全局状态), 组件里的 json/atlas 解析直接用它的 API, 所以链进来 ——
# 但**不**链 bongo_cat_core: 那会把宿主的配置/缓存实现复制一份, 两份状态必然分叉。
target_link_libraries(bongo_live2d PRIVATE Framework yyjson bongo_cat_warnings)
target_include_directories(bongo_live2d PRIVATE include
  "${BONGO_CAT_GENERATED_INCLUDE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/live2d"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/live2d/component")
# SDL 只给**头文件**(桥接需要类型与声明): 链接期不允许出现任何 SDL 符号,
# 这正是下面那个"组件不得依赖 SDL"的构建期断言要保证的事。
target_include_directories(bongo_live2d SYSTEM PRIVATE
  $<TARGET_PROPERTY:SDL3::SDL3-static,INTERFACE_INCLUDE_DIRECTORIES>)

# 强制包含 SDL 改道表: 先让 SDL 头声明完, 再用同名宏把调用改到宿主服务表。
if(MSVC)
  target_compile_options(bongo_live2d PRIVATE
    "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/live2d/component/component_sdl_shim.h")
  # getenv 在 MSVC 下被判为废弃(C4996 -> /WX 直接失败)。桥接只用它读一个只读的调试阈值
  # (BONGO_CAT_MASK_BUDGET_MB), 而环境块是**进程级**的, 两份 CRT 看到的是同一份, 所以
  # 这里关掉这条 deprecation, 而不是为它单开一个服务。
  target_compile_definitions(bongo_live2d PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(bongo_live2d PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/src/live2d/component/component_sdl_shim.h")
endif()

# 只导出查询符号: 其余符号对宿主不可见, 避免组件里的内部实现与宿主同名符号互相抢占。
set_target_properties(bongo_live2d PROPERTIES
  PREFIX "" OUTPUT_NAME "bongo_live2d"
  CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
if(WIN32)
  # 少一个"发布时要记得改"的地方: 组件与宿主用同一份版本号。
  set_target_properties(bongo_live2d PROPERTIES VERSION "${BONGO_CAT_VERSION}")
endif()

# stage: <exe 目录>/cores/。用 POST_BUILD + copy_if_different(而不是 install),
# 因为开发期直接跑 build 目录也要能用。
add_custom_command(TARGET bongo_live2d POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:bongo_live2d>/cores"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bongo_live2d>"
    "$<TARGET_FILE_DIR:bongo_live2d>/cores/$<TARGET_FILE_NAME:bongo_live2d>"
  COMMENT "Staging the Live2D renderer component into cores/"
  VERBATIM)

# 构建期断言: 组件的动态依赖表里不得出现 SDL 或 Core(组件必须是"薄"的:
# 只有 Framework + 我们的桥接 + 生成的 shim)。这条断言把纪律 1/2 变成会失败的检查,
# 而不是靠注释里的一句"请勿链接 SDL"。
if(MSVC AND BONGO_CAT_DUMPBIN)
  add_custom_command(TARGET bongo_live2d POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      "-DEXECUTABLE=$<TARGET_FILE:bongo_live2d>"
      "-DDUMPBIN=${BONGO_CAT_DUMPBIN}"
      -P "${CMAKE_CURRENT_LIST_DIR}/CheckComponentImports.cmake"
    COMMENT "Auditing the renderer component's imports (no SDL, no Cubism Core)"
    VERBATIM)
endif()
