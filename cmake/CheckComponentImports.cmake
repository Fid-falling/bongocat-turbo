# 组件产物的导入表审计: 组件必须是"薄"的 —— 只依赖 Cubism Framework(编进去的)、我们的
# 桥接与生成的 shim, **不得**依赖 SDL(宿主拥有 SDL 状态, 两份会看到不同的窗口/上下文),
# 也**不得**依赖任何 Cubism Core(专有库只能由用户在运行期提供)。
#
# 与 cmake/CheckWindowsImports.cmake 的分工: 那个查宿主 exe 的输入能力(hook/RawInput),
# 这个查组件的依赖边界。两者都做成"构建期就会失败的断言", 而不是靠文档纪律。
#
# 用法(由 cmake/RendererModule.cmake 以 -P 调用):
#   cmake -DEXECUTABLE=<组件路径> -DDUMPBIN=<dumpbin 路径> -P CheckComponentImports.cmake
if(NOT DEFINED EXECUTABLE OR NOT DEFINED DUMPBIN)
  message(FATAL_ERROR "EXECUTABLE and DUMPBIN are required")
endif()
if(NOT EXISTS "${EXECUTABLE}")
  message(FATAL_ERROR "The renderer component is missing: ${EXECUTABLE}")
endif()

execute_process(COMMAND "${DUMPBIN}" /dependents "${EXECUTABLE}"
  OUTPUT_VARIABLE imports ERROR_VARIABLE dumpbin_error RESULT_VARIABLE dumpbin_status)
if(NOT dumpbin_status EQUAL 0)
  message(FATAL_ERROR "dumpbin failed on ${EXECUTABLE}: ${dumpbin_error}")
endif()

set(forbidden "SDL3 SDL2 Live2DCubismCore")
foreach(name IN LISTS forbidden)
  if(imports MATCHES "(^|[\r\n\t ])${name}[^\r\n]*\\.dll")
    message(FATAL_ERROR
      "渲染组件依赖了 ${name}: ${EXECUTABLE}\n"
      "组件不得链接 SDL(窗口/GL 上下文/日志都属于宿主那一份)也不得链接 Cubism Core"
      "(专有库由用户在运行期提供)。需要宿主能力请加进服务表(见 renderer_module_abi.h)。")
  endif()
endforeach()

# 顺带确认它确实导出了我们唯一的入口符号 —— 少了它宿主只会把它当成"不是渲染组件"。
execute_process(COMMAND "${DUMPBIN}" /exports "${EXECUTABLE}"
  OUTPUT_VARIABLE exports ERROR_VARIABLE exports_error RESULT_VARIABLE exports_status)
if(NOT exports_status EQUAL 0)
  message(FATAL_ERROR "dumpbin /exports failed on ${EXECUTABLE}: ${exports_error}")
endif()
if(NOT exports MATCHES "bongo_cat_renderer_module_query")
  message(FATAL_ERROR
    "渲染组件没有导出 bongo_cat_renderer_module_query: ${EXECUTABLE}\n"
    "检查 BONGO_CAT_RENDERER_MODULE_EXPORT 是否加在 query 函数上(Windows 上不加就等于没有)。")
endif()

message(STATUS "Component import audit passed: ${EXECUTABLE}")
