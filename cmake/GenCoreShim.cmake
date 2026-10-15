# Cubism Core 的运行期 shim(法律边界的技术落地, 见 cmake/GenCoreShim.ps1 的说明)。
#
# 生成的 shim 是**唯一**提供 csm* 符号的东西: 宿主与渲染组件都不链接 Core(专有库),
# 只链接这个 shim; shim 在运行期把调用转给用户自备的 Core 动态库。
#
# 为什么用 PowerShell 生成而不是 Python: 本仓库不依赖 Python(只有个别工作流用), 而 pwsh
# 已经是既有依赖(cmake/Tests.cmake 里同样 find_program pwsh)。少一个工具链依赖就少一类
# "在干净机器上配不出来"的问题。

# 输入只有官方头文件: 它是接口描述, 不含实现。缺它就等于 SDK 不完整。
set(BONGO_CAT_CORE_SHIM_HEADER "${CUBISM_CORE_PATH}/include/Live2DCubismCore.h")
if(NOT EXISTS "${BONGO_CAT_CORE_SHIM_HEADER}")
  message(FATAL_ERROR "Cubism Core header is missing: ${BONGO_CAT_CORE_SHIM_HEADER}")
endif()

find_program(BONGO_CAT_CORE_SHIM_PWSH NAMES pwsh powershell REQUIRED
  DOC "PowerShell used to generate the Cubism Core runtime shim")
set(BONGO_CAT_CORE_SHIM_SOURCE "${CMAKE_BINARY_DIR}/generated/cubism_core_shim.c")

add_custom_command(
  OUTPUT "${BONGO_CAT_CORE_SHIM_SOURCE}"
  COMMAND "${BONGO_CAT_CORE_SHIM_PWSH}" -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_LIST_DIR}/GenCoreShim.ps1"
    -Header "${BONGO_CAT_CORE_SHIM_HEADER}"
    -Output "${BONGO_CAT_CORE_SHIM_SOURCE}"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/GenCoreShim.ps1" "${BONGO_CAT_CORE_SHIM_HEADER}"
  COMMENT "Generating the Cubism Core runtime shim (no Core binary is linked)"
  VERBATIM)

add_library(bongo_cat_core_shim STATIC "${BONGO_CAT_CORE_SHIM_SOURCE}")
target_include_directories(bongo_cat_core_shim SYSTEM PUBLIC
  "${CUBISM_CORE_PATH}/include")
target_link_libraries(bongo_cat_core_shim PRIVATE bongo_cat_warnings)
# 解析器由**链接进同一个二进制**的一方提供: 宿主提供 bongo_cat_live2d_core_symbol 的包装,
# 渲染组件提供走宿主服务表 core_symbol 的包装。这样 shim 本身与"谁加载 Core"解耦。
target_compile_definitions(bongo_cat_core_shim PUBLIC BONGO_CAT_CUBISM_CORE_SHIM=1)
