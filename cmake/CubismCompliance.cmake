# Live2D Cubism 合规守卫(法律边界, 不是代码风格检查)。
#
# 三条不可越界的规则(与 LIVE2D-PLUGIN-PLAN.md §9.1 一致):
#   1. 宿主与渲染插件的**链接列表**里不得出现 Live2DCubismCore(静态库或导入库);
#   2. Core 的 csm* ABI 由运行期 shim 持有, 经宿主服务表 core_symbol 解析;
#   3. 发行包 / 安装树 / CI artifact 里不得出现任何 Live2DCubismCore.* 文件。
#
# 为什么要有这个文件: 规则 1 与 3 靠人记是记不住的 —— 一次 "顺手把 SDK 目录加进
# target_link_libraries" 就会让发行物重新包含专有库的目标码, 而构建、单测、界面都不会报错。
# 所以把判定做成可执行断言, 让违规在配置期/打包期直接失败。
#
# 两种用法:
#   * 配置期(target 级): bongo_cat_assert_no_cubism_core_link(<target>)
#   * 脚本期(目录级):     cmake -DBONGO_CAT_AUDIT_ROOT=<dir> [-DBONGO_CAT_AUDIT_EXPECT_COMPONENT=ON]
#                              -P cmake/CubismCompliance.cmake
#
# 名字来源: Core 的库名在各平台分别是 Live2DCubismCore.dll/.lib(Windows)、
# libLive2DCubismCore.a/.so(Linux)、libLive2DCubismCore.dylib/.a(macOS),
# 前缀 lib 与后缀随平台变, 所以统一按 "去掉 lib 前缀与扩展名后等于 Live2DCubismCore" 判定。

set(BONGO_CAT_CUBISM_CORE_STEM "Live2DCubismCore")

# 把文件名归一成可比较的词干: 去目录、去 lib 前缀、去扩展名、去 MSVC 的运行库后缀
# (官方 Windows 静态库叫 Live2DCubismCore_MT.lib, 不归一就会漏判)。
function(bongo_cat_cubism_core_stem_of output name)
  get_filename_component(_name "${name}" NAME)
  string(TOLOWER "${_name}" _lower)
  if(_lower MATCHES "^lib")
    string(SUBSTRING "${_name}" 3 -1 _name)
  endif()
  string(REGEX REPLACE "\\.[^.]*$" "" _name "${_name}")
  string(REGEX REPLACE "_(MT|mt|MD|md|D|d)$" "" _name "${_name}")
  set(${output} "${_name}" PARENT_SCOPE)
endfunction()

# 目录级扫描: 命中任何 Core 二进制即 FATAL_ERROR。
# 只按名字扫 —— 按内容扫(改名夹带)需要流式读二进制, 那是 CI 侧 PowerShell 脚本的活
# (check-unix-package.ps1 / release.yml), 这里保持构建期开销可忽略。
function(bongo_cat_audit_no_cubism_core_files root)
  if(NOT EXISTS "${root}")
    message(FATAL_ERROR "Cubism 合规审计目录不存在: ${root}")
  endif()
  file(GLOB_RECURSE _candidates
    "${root}/*Live2DCubismCore.dll" "${root}/*Live2DCubismCore.so"
    "${root}/*Live2DCubismCore.dylib" "${root}/*Live2DCubismCore.lib"
    "${root}/*Live2DCubismCore.a" "${root}/*Live2DCubismCore_MT.lib")
  if(_candidates)
    list(JOIN _candidates "\n  " _joined)
    message(FATAL_ERROR
      "Live2D Cubism Core 出现在发行产物里(它是专有库, 由用户自行从官网取得并导入):\n  ${_joined}\n"
      "见 LIVE2D-PLUGIN-PLAN.md §9.1 规则 3。")
  endif()
  message(STATUS "Cubism 合规审计通过(无 Core 文件): ${root}")
endfunction()

# 目录级断言: 发行包必须带我们自己的 Live2D 渲染组件 cores/bongo_live2d.*。
# 组件缺失时 Live2D 模型永远渲染不出来, 而这个失败在"能启动"的假象下很难被发现。
# 位置是 exe 同级的 cores/(随包分发): 它内含 Framework 但**不含** Core, 所以它存在
# 与"包内无 Core"两条断言并不冲突, 而是一起把发行物的边界钉死。
function(bongo_cat_audit_live2d_component root)
  set(_found FALSE)
  foreach(_name IN ITEMS bongo_live2d.dll libbongo_live2d.so libbongo_live2d.dylib)
    if(EXISTS "${root}/cores/${_name}")
      set(_found TRUE)
    endif()
  endforeach()
  if(NOT _found)
    message(FATAL_ERROR
      "发行包里没有 Live2D 渲染组件(应有 cores/bongo_live2d.dll|libbongo_live2d.so|.dylib): ${root}")
  endif()
  message(STATUS "Cubism 合规审计通过(Live2D 渲染组件存在): ${root}/cores")
endfunction()

# target 级断言: 目标的链接列表(含 INTERFACE 传递)里不得出现 Core。
# 同时覆盖"直接链核心库"与"链了一个 PUBLIC 传播 Core 的中间目标"两种情况 ——
# 后者正是我们当前 cmake/Cubism.cmake 把 Core 挂在 Framework 上的形态。
function(bongo_cat_collect_core_links target out visited)
  if("${target}" IN_LIST ${visited})
    return()
  endif()
  set(${visited} "${${visited}};${target}" PARENT_SCOPE)
  # 目标**自身**的名字必须先查: Live2DCubismCore 是个 IMPORTED STATIC 目标, 它自己没有
  # 链接列表 —— 只看它的 LINK_LIBRARIES 会漏掉"宿主直接链了 Core"这个最危险的形态。
  bongo_cat_cubism_core_stem_of(_normalized "${target}")
  if(_normalized STREQUAL BONGO_CAT_CUBISM_CORE_STEM)
    set(${out} "${${out}};${target}" PARENT_SCOPE)
    return()
  endif()
  if(NOT TARGET "${target}")
    return()
  endif()
  foreach(_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_links "${target}" ${_property})
    if(NOT _links)
      continue()
    endif()
    foreach(_link IN LISTS _links)
      # 生成器表达式($<...>)不是目标名, 跳过; 其中的目标会在别处单独检查。
      if(_link MATCHES "^\\$<")
        continue()
      endif()
      bongo_cat_collect_core_links("${_link}" ${out} ${visited})
    endforeach()
  endforeach()
  # 收集结果必须逐层往上传: 子调用用 PARENT_SCOPE 只写到**本层**作用域, 不在收尾处再往
  # 上写一次的话, 深层发现的东西会在中间层断掉(表现为"明明链了 Core 却查不出来")。
  set(${out} "${${out}}" PARENT_SCOPE)
  set(${visited} "${${visited}}" PARENT_SCOPE)
endfunction()

# 阶段 2e 起这里是**硬失败**: 宿主改用生成的 shim(cmake/GenCoreShim.ps1)之后, 链接列表里
# 任何 Core 引用都是违规, 不再有"迁移期暂时允许"的例外。那条过渡 option 已删除 —— 留这行
# 注释是为了让后人知道它曾经存在、以及为什么现在没有例外。
function(bongo_cat_assert_no_cubism_core_link target)
  set(_visited "")
  set(_found "")
  bongo_cat_collect_core_links("${target}" _found _visited)
  if(NOT _found)
    return()
  endif()
  list(REMOVE_DUPLICATES _found)
  list(REMOVE_ITEM _found "")
  list(JOIN _found ", " _joined)
  message(FATAL_ERROR
    "目标 ${target} 链接了 Live2D Cubism Core(${_joined})。"
    "宿主与渲染组件都不得链接或内嵌 Core: Core 只能由用户自行从 Live2D 官网取得, "
    "运行期经宿主服务表 core_symbol 解析(csm* 符号由生成的 shim 提供)。"
    "见 LIVE2D-PLUGIN-PLAN.md §9.1 规则 1。")
endfunction()

# 脚本期入口: 供打包后/CI 复用同一套判定, 避免"构建期查一套、CI 查另一套"。
# BONGO_CAT_AUDIT_ROOT 可以是列表(分号分隔): 不存在的目录跳过(例如还没跑过打包的 dist),
# 但一个都不存在就是配置错误 —— 那样脚本会"通过"一次什么都没查的审计。
if(BONGO_CAT_AUDIT_ROOT)
  set(_audited 0)
  foreach(_root IN LISTS BONGO_CAT_AUDIT_ROOT)
    if(NOT EXISTS "${_root}")
      message(STATUS "Cubism 合规审计跳过(目录不存在): ${_root}")
      continue()
    endif()
    bongo_cat_audit_no_cubism_core_files("${_root}")
    if(BONGO_CAT_AUDIT_EXPECT_COMPONENT)
      bongo_cat_audit_live2d_component("${_root}")
    endif()
    math(EXPR _audited "${_audited} + 1")
  endforeach()
  if(_audited EQUAL 0)
    message(FATAL_ERROR "Cubism 合规审计没有任何可审计目录: ${BONGO_CAT_AUDIT_ROOT}")
  endif()
endif()
