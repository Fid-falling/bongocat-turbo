if(NOT DEFINED ROOT)
  message(FATAL_ERROR "ROOT is required")
endif()

# 匹配精度: 下面所有"禁止的生产 API / token / 敏感能力归属"判定都只看**代码**。
# 先把注释与字符串字面量剥掉, 再匹配 —— 否则一句"RegisterRawInputDevices 是进程级的"
# 说明性注释就会被当成真实调用(这正是这个门禁长期变红的原因: 注释里提到 API 名很常见,
# 而按子串匹配分不清注释与代码)。
#
# 为什么不用几条正则一把梭: 字符串里的 "//" / "/*" 会骗过按行的注释剥离, 注释里的
# 引号又会骗过字符串剥离 —— 两者互相干扰。这里用一个按行的小状态机, 只做三件事:
#   * 块注释 /* ... */ 可以跨行;
#   * 行注释 // 到行尾;
#   * 字符串/字符字面量整体替换成空字面量("" / ''), 其中的反斜杠转义不结束字面量。
# 它不解析预处理、不解析 raw string —— 那是 C++ 解析器的工作; 本门禁只需要"代码文本",
# 不是完整编译前端。
function(bongo_cat_strip_comments_and_strings INPUT OUTVAR)
  string(REPLACE "\r\n" "\n" text "${INPUT}")
  string(REPLACE "\r" "\n" text "${text}")
  # 保护分号: 下面要按换行切 CMake 列表, 而源码里的 ";" 会被当成分隔符。
  string(REPLACE ";" "\\;" text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  set(result "")
  set(in_block FALSE)
  foreach(line IN LISTS lines)
    set(rest "${line}")
    set(code "")
    while(TRUE)
      if("${rest}" STREQUAL "")
        break()
      endif()
      if(in_block)
        string(FIND "${rest}" "*/" close)
        if(close EQUAL -1)
          set(rest "")
        else()
          math(EXPR after "${close} + 2")
          string(SUBSTRING "${rest}" ${after} -1 rest)
          set(in_block FALSE)
        endif()
        continue()
      endif()
      string(FIND "${rest}" "/*" block_open)
      string(FIND "${rest}" "//" line_comment)
      string(FIND "${rest}" "\"" double_quote)
      string(FIND "${rest}" "'" single_quote)
      set(kind "")
      set(position -1)
      foreach(candidate block_open line_comment double_quote single_quote)
        if(NOT ${candidate} EQUAL -1 AND
            (position EQUAL -1 OR ${candidate} LESS position))
          set(position ${${candidate}})
        endif()
      endforeach()
      if(position EQUAL -1)
        string(APPEND code "${rest}")
        set(rest "")
      elseif(position EQUAL block_open)
        string(SUBSTRING "${rest}" 0 ${position} prefix)
        string(APPEND code "${prefix}")
        math(EXPR after "${position} + 2")
        string(SUBSTRING "${rest}" ${after} -1 rest)
        set(in_block TRUE)
      elseif(position EQUAL line_comment)
        string(SUBSTRING "${rest}" 0 ${position} prefix)
        string(APPEND code "${prefix}")
        set(rest "")
      elseif(position EQUAL double_quote)
        string(SUBSTRING "${rest}" 0 ${position} prefix)
        string(APPEND code "${prefix}")
        # 从引号本身开始匹配字符串字面量; 用 tail 而不是 rest, 因为 position 可能不在 0。
        string(SUBSTRING "${rest}" ${position} -1 tail)
        string(REGEX MATCH "^\"([^\"\\\\]|\\\\.)*\"" literal "${tail}")
        if(literal STREQUAL "")
          # 行内没有闭合引号: 多行字符串在 C 里只能靠反斜杠续行, 这里直接丢到行尾,
          # 保证不会把下一行当成"字符串里"而漏掉真实调用。
          set(rest "")
        else()
          string(APPEND code "\"\"")
          string(LENGTH "${literal}" length)
          math(EXPR after "${position} + ${length}")
          string(SUBSTRING "${rest}" ${after} -1 rest)
        endif()
      else()
        string(SUBSTRING "${rest}" 0 ${position} prefix)
        string(APPEND code "${prefix}")
        string(SUBSTRING "${rest}" ${position} -1 tail)
        string(REGEX MATCH "^'([^'\\\\]|\\\\.)*'" literal "${tail}")
        if(literal STREQUAL "")
          set(rest "")
        else()
          string(APPEND code "''")
          string(LENGTH "${literal}" length)
          math(EXPR after "${position} + ${length}")
          string(SUBSTRING "${rest}" ${after} -1 rest)
        endif()
      endif()
    endwhile()
    string(APPEND result "${code}\n")
  endforeach()
  set(${OUTVAR} "${result}" PARENT_SCOPE)
endfunction()

# 与运行时判定同一个正则: 前面不能是标识符字符(避免 MySetCursorPos 命中), 后面允许空白
# 再跟 "("(避免把声明/宏名当成调用之外的东西 —— 这里仍然按"调用"计, 见规则说明)。
function(bongo_cat_api_calls TEXT API OUTVAR)
  string(REGEX MATCHALL
    "(^|[^A-Za-z0-9_])${API}[ \t\r\n]*\\(" MATCHES "${TEXT}")
  list(LENGTH MATCHES count)
  if(count EQUAL 0)
    set(${OUTVAR} FALSE PARENT_SCOPE)
  else()
    set(${OUTVAR} TRUE PARENT_SCOPE)
  endif()
endfunction()

function(bongo_cat_expect VALUE EXPECTED LABEL)
  if(NOT "${VALUE}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR
      "platform-safety matcher self-test failed: ${LABEL} (got ${VALUE}, expected ${EXPECTED})")
  endif()
endfunction()

# 自测: 每次跑门禁都先证明"剥离器没有把代码当注释、也没有把注释当代码"。
# 这是本门禁可信度的前提 —— 修误报时最危险的做法是顺手把规则放宽到什么都抓不到。
function(bongo_cat_self_test)
  # 1) 只有注释/字符串提到 API 与 token: 必须一条都不命中。
  set(comment_only
    "/* SetCursorPos(1, 2); RegisterRawInputDevices(3, 4) */\n"
    "// OpenProcess(5, 6)\n"
    "const char *text = \"SE_DEBUG_NAME\";\n"
    "const char *path = \"C:\\\\dev\\\\uinput\";\n")
  bongo_cat_strip_comments_and_strings("${comment_only}" stripped)
  bongo_cat_api_calls("${stripped}" SetCursorPos calls)
  bongo_cat_expect("${calls}" FALSE "comment-only API")
  bongo_cat_api_calls("${stripped}" OpenProcess calls)
  bongo_cat_expect("${calls}" FALSE "line-comment API")
  string(FIND "${stripped}" "SE_DEBUG_NAME" position)
  if(NOT position EQUAL -1)
    message(FATAL_ERROR
      "platform-safety matcher self-test failed: string-literal token matched")
  endif()

  # 2) 块注释跨行 + 注释之后紧跟真实调用: 必须命中, 且只命中真实调用。
  set(multiline
    "/*\n"
    " * SetCursorPos(1, 2);\n"
    " */\n"
    "SetCursorPos(7, 8);\n")
  bongo_cat_strip_comments_and_strings("${multiline}" stripped)
  bongo_cat_api_calls("${stripped}" SetCursorPos calls)
  bongo_cat_expect("${calls}" TRUE "call after block comment")

  # 3) 字符串里出现注释起始符, 后面同一行还有真实调用: 不能被"行注释"吞掉。
  set(mixed
    "SDL_Log(\"http://example.invalid/* not a comment */\"); "
    "SetCursorPos(9, 9);\n")
  bongo_cat_strip_comments_and_strings("${mixed}" stripped)
  bongo_cat_api_calls("${stripped}" SetCursorPos calls)
  bongo_cat_expect("${calls}" TRUE "comment marker inside string")

  # 4) 真实调用后面跟注释: 调用必须留下。
  set(trailing
    "SetCursorPos(1, 2); // SetCursorPos(3, 4)\n")
  bongo_cat_strip_comments_and_strings("${trailing}" stripped)
  bongo_cat_api_calls("${stripped}" SetCursorPos calls)
  bongo_cat_expect("${calls}" TRUE "call before trailing comment")

  # 5) token 在代码里也必须被抓到。
  set(token_code "DWORD access = SE_DEBUG_NAME;\n")
  bongo_cat_strip_comments_and_strings("${token_code}" stripped)
  string(FIND "${stripped}" "SE_DEBUG_NAME" position)
  if(position EQUAL -1)
    message(FATAL_ERROR
      "platform-safety matcher self-test failed: code token was stripped")
  endif()
endfunction()

bongo_cat_self_test()

file(GLOB_RECURSE PRODUCTION_FILES
  "${ROOT}/src/*.c" "${ROOT}/src/*.cc" "${ROOT}/src/*.cpp"
  "${ROOT}/src/*.cxx" "${ROOT}/src/*.m" "${ROOT}/src/*.mm"
  "${ROOT}/src/*.h" "${ROOT}/src/*.hh" "${ROOT}/src/*.hpp"
  "${ROOT}/src/*.hxx" "${ROOT}/include/*.h" "${ROOT}/include/*.hh"
  "${ROOT}/include/*.hpp" "${ROOT}/include/*.hxx")
list(FILTER PRODUCTION_FILES EXCLUDE REGEX "[/\\\\]src[/\\\\]tools[/\\\\]")

set(FORBIDDEN_APIS
  OpenProcess ReadProcessMemory WriteProcessMemory VirtualAllocEx
  VirtualProtectEx CreateRemoteThread CreateRemoteThreadEx NtOpenProcess
  NtReadVirtualMemory NtWriteVirtualMemory NtCreateThreadEx
  RtlCreateUserThread QueueUserAPC SetThreadContext DebugActiveProcess
  DebugBreakProcess MiniDumpWriteDump CreateToolhelp32Snapshot Process32First
  Process32Next EnumProcesses EnumProcessModules AdjustTokenPrivileges
  LookupPrivilegeValue OpenSCManager CreateService StartService ControlService
  DeviceIoControl NtLoadDriver AttachThreadInput SendInput keybd_event
  mouse_event SetWindowsHookEx SetWindowsHookExA SetWindowsHookExW DirectInput8Create
  SetCursorPos SetPhysicalCursorPos ClipCursor BlockInput
  SDL_SetWindowRelativeMouseMode
  SDL_SetWindowKeyboardGrab
  CGEventPost CGEventPostToPid CGEventCreateKeyboardEvent
  CGEventCreateMouseEvent CGWarpMouseCursorPosition
  CGAssociateMouseAndMouseCursorPosition IOHIDManagerCreate
  IOHIDEventSystemClientCreate IOServiceOpen task_for_pid mach_vm_read
  mach_vm_read_overwrite mach_vm_write mach_vm_allocate mach_vm_protect
  AuthorizationCreate AuthorizationExecuteWithPrivileges SMJobBless
  XTestFakeKeyEvent XTestFakeButtonEvent XTestFakeMotionEvent XWarpPointer
  XGrabKey XGrabButton process_vm_readv process_vm_writev ptrace ioctl
  setuid seteuid setgid setegid capset)
set(FORBIDDEN_TOKENS
  SE_DEBUG_NAME SeDebugPrivilege PROCESS_VM_READ PROCESS_VM_WRITE
  PROCESS_VM_OPERATION PROCESS_ALL_ACCESS THREAD_SET_CONTEXT
  kCGHIDEventTap kCGHeadInsertEventTap IOConnectCall
  /dev/uinput CAP_SYS_PTRACE CAP_SYS_ADMIN
  WH_KEYBOARD_LL WH_MOUSE_LL RIDEV_NOLEGACY RIDEV_CAPTUREMOUSE
  SDL_WINDOW_KEYBOARD_GRABBED SDL_HINT_FORCE_RAISEWINDOW
  RIDEV_NOHOTKEYS RIDEV_APPKEYS RIDEV_EXCLUDE RIDEV_EXINPUTSINK)

# 只有"原始文本里出现候选名"的文件才值得做剥离与精确匹配 —— 剥离器本身不便宜,
# 但候选文件通常只有十来个。原始文本是精确匹配的超集, 所以跳过不会漏报。
set(SCAN_MARKERS ${FORBIDDEN_APIS} ${FORBIDDEN_TOKENS})
set(SENSITIVE_RULES
  "GetAsyncKeyState|src/platform/windows/windows_keys.c"
  "RegisterRawInputDevices|src/platform/windows/windows_input_registration.c"
  "GetRegisteredRawInputDevices|src/platform/windows/windows_input_registration.c"
  "GetRawInputData|src/platform/windows/windows_input_receiver.c"
  "SDL_HINT_WINDOWS_RAW_KEYBOARD|src/runtime/shell/window.c|src/platform/windows/windows_input.c"
  "BitBlt|src/platform/windows/windows_capture_probe.c"
  "PrintWindow|src/platform/windows/windows_capture_probe.c"
  # UpdateLayeredWindow 由分层窗口模块独占。1.17.x 把整帧提交拆到
  # windows_layered_frame.c(见该文件头), 它仍是同一个受审模块的一部分, 所以归它一份。
  "UpdateLayeredWindow|src/platform/windows/windows_layered.c|src/platform/windows/windows_layered_frame.c"
  "CGEventTapCreate|src/platform/macos/macos_input.m"
  "CGEventTapEnable|src/platform/macos/macos_input.m"
  "CGPreflightListenEventAccess|src/platform/macos/macos_input.m"
  "CGRequestListenEventAccess|src/platform/macos/macos_input.m"
  "/dev/input|src/platform/linux/linux_evdev_devices.c"
  "XISelectEvents|src/platform/linux/linux_x11.c"
  "XFixesSetWindowShapeRegion|src/platform/linux/linux_x11.c"
  "XSendEvent|src/platform/linux/linux.c|src/platform/linux/linux_x11.c"
  "XGrabPointer|src/platform/linux/linux_menu.c"
  "XGrabKeyboard|src/platform/linux/linux_menu.c"
  "SDL_INIT_GAMEPAD|src/runtime/input/gamepad.c"
  "SDL_OpenGamepad|src/runtime/input/gamepad.c"
  "SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS|src/runtime/input/gamepad.c"
  "SDL_CreateProcessWithProperties|src/runtime/model/multi_pet_process.c"
  "SDL_KillProcess|src/runtime/model/multi_pet_process.c")
foreach(RULE IN LISTS SENSITIVE_RULES)
  string(REPLACE "|" ";" PARTS "${RULE}")
  list(POP_FRONT PARTS CAPABILITY)
  list(APPEND SCAN_MARKERS "${CAPABILITY}")
endforeach()

set(FAILURES "")
foreach(FILE IN LISTS PRODUCTION_FILES)
  file(READ "${FILE}" SOURCE)
  set(candidate FALSE)
  foreach(MARKER IN LISTS SCAN_MARKERS)
    string(FIND "${SOURCE}" "${MARKER}" POSITION)
    if(NOT POSITION EQUAL -1)
      set(candidate TRUE)
      break()
    endif()
  endforeach()
  if(NOT candidate)
    continue()
  endif()
  bongo_cat_strip_comments_and_strings("${SOURCE}" CODE)
  file(RELATIVE_PATH RELATIVE_FILE "${ROOT}" "${FILE}")
  string(REPLACE "\\" "/" RELATIVE_FILE "${RELATIVE_FILE}")

  foreach(API IN LISTS FORBIDDEN_APIS)
    bongo_cat_api_calls("${CODE}" "${API}" MATCHED)
    if(MATCHED)
      list(APPEND FAILURES
        "${RELATIVE_FILE}: forbidden production API ${API}")
    endif()
  endforeach()
  foreach(TOKEN IN LISTS FORBIDDEN_TOKENS)
    string(FIND "${CODE}" "${TOKEN}" POSITION)
    if(NOT POSITION EQUAL -1)
      list(APPEND FAILURES
        "${RELATIVE_FILE}: forbidden production token ${TOKEN}")
    endif()
  endforeach()

  # Intentional sensitive capabilities remain confined to reviewed modules.
  foreach(RULE IN LISTS SENSITIVE_RULES)
    string(REPLACE "|" ";" PARTS "${RULE}")
    list(POP_FRONT PARTS CAPABILITY)
    string(FIND "${CODE}" "${CAPABILITY}" POSITION)
    if(POSITION EQUAL -1)
      continue()
    endif()
    list(FIND PARTS "${RELATIVE_FILE}" OWNER_POSITION)
    if(OWNER_POSITION EQUAL -1)
      list(JOIN PARTS ", " OWNERS)
      list(APPEND FAILURES
        "${RELATIVE_FILE}: ${CAPABILITY} is owned by ${OWNERS}")
    endif()
  endforeach()
endforeach()

# The reviewed evdev exception permits read-only observation, not device writes
# or permission changes.
file(READ "${ROOT}/src/platform/linux/linux_evdev_devices.c" LINUX_EVDEV_RAW)
bongo_cat_strip_comments_and_strings("${LINUX_EVDEV_RAW}" LINUX_EVDEV)
foreach(TOKEN O_WRONLY O_RDWR O_CREAT O_TRUNC)
  string(FIND "${LINUX_EVDEV}" "${TOKEN}" POSITION)
  if(NOT POSITION EQUAL -1)
    list(APPEND FAILURES "linux_evdev_devices.c: forbidden open flag ${TOKEN}")
  endif()
endforeach()
foreach(TOKEN O_RDONLY O_NONBLOCK O_CLOEXEC O_NOFOLLOW)
  string(FIND "${LINUX_EVDEV}" "${TOKEN}" POSITION)
  if(POSITION EQUAL -1)
    list(APPEND FAILURES "linux_evdev_devices.c: missing open safeguard ${TOKEN}")
  endif()
endforeach()
foreach(API chmod fchmod chown fchown system popen)
  bongo_cat_api_calls("${LINUX_EVDEV}" "${API}" MATCHED)
  if(MATCHED)
    list(APPEND FAILURES "linux_evdev_devices.c: forbidden API ${API}")
  endif()
endforeach()

file(READ "${ROOT}/cmake/windows.manifest.in" WINDOWS_MANIFEST)
string(FIND "${WINDOWS_MANIFEST}"
  "<requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/>"
  MANIFEST_POSITION)
if(MANIFEST_POSITION EQUAL -1)
  list(APPEND FAILURES
    "cmake/windows.manifest.in: expected asInvoker with uiAccess=false")
endif()

file(READ "${ROOT}/packaging/windows/BongoCat.iss.in" WINDOWS_PACKAGING)
file(READ "${ROOT}/packaging/windows/install-lifecycle.iss.in" WINDOWS_INSTALL_LIFECYCLE)
string(APPEND WINDOWS_PACKAGING "\n${WINDOWS_INSTALL_LIFECYCLE}")
string(FIND "${WINDOWS_PACKAGING}" "PrivilegesRequired=lowest"
  INSTALLER_LEVEL_POSITION)
string(FIND "${WINDOWS_PACKAGING}"
  "DefaultDirName={code:GetInstallDir}"
  INSTALLER_ROOT_POSITION)
string(FIND "${WINDOWS_PACKAGING}"
  "{localappdata}\\Programs\\BongoCat" INSTALLER_DEFAULT_POSITION)
if(INSTALLER_LEVEL_POSITION EQUAL -1 OR INSTALLER_ROOT_POSITION EQUAL -1
    OR INSTALLER_DEFAULT_POSITION EQUAL -1)
  list(APPEND FAILURES
    "packaging/windows/BongoCat.iss.in: expected current-user Inno installation")
endif()

file(READ "${ROOT}/cmake/Info.plist.in" MACOS_INFO)
string(FIND "${MACOS_INFO}" "NSInputMonitoringUsageDescription"
  INPUT_MONITORING_POSITION)
string(FIND "${MACOS_INFO}" "NSAccessibilityUsageDescription"
  ACCESSIBILITY_POSITION)
if(INPUT_MONITORING_POSITION EQUAL -1 OR NOT ACCESSIBILITY_POSITION EQUAL -1)
  list(APPEND FAILURES
    "cmake/Info.plist.in: expected Input Monitoring without Accessibility")
endif()

file(READ "${ROOT}/src/platform/macos/macos_input.m" MACOS_INPUT)
foreach(TOKEN kCGSessionEventTap kCGTailAppendEventTap
    kCGEventTapOptionListenOnly)
  string(FIND "${MACOS_INPUT}" "${TOKEN}" POSITION)
  if(POSITION EQUAL -1)
    list(APPEND FAILURES
      "src/platform/macos/macos_input.m: missing safe event tap token ${TOKEN}")
  endif()
endforeach()

file(READ "${ROOT}/cmake/PlatformRuntime.cmake" PLATFORM_RUNTIME)
string(FIND "${PLATFORM_RUNTIME}" "X11::Xtst" XTEST_POSITION)
if(NOT XTEST_POSITION EQUAL -1)
  list(APPEND FAILURES
    "cmake/PlatformRuntime.cmake: XTest must not enter the Linux runtime")
endif()

file(READ "${ROOT}/cmake/Dependencies.cmake" DEPENDENCIES)
foreach(TOKEN 402fc52af4e731184ad6a704068b5ccd27d8f1b8
    e413151af71c23d316b6076a96a999342142afa792394eaf8a542a03503fc491)
  string(FIND "${DEPENDENCIES}" "${TOKEN}" POSITION)
  if(POSITION EQUAL -1)
    list(APPEND FAILURES
      "cmake/Dependencies.cmake: SDL dependency changed without safety review")
  endif()
endforeach()

file(READ "${ROOT}/cmake/ValidationTools.cmake" VALIDATION_TOOLS)
foreach(TOOL cubism_viewer_desktop_capture cubism_viewer_drag_capture)
  string(REGEX MATCH
    "add_executable\\(${TOOL}[ \t\r\n]+EXCLUDE_FROM_ALL" TOOL_ISOLATED
    "${VALIDATION_TOOLS}")
  if(NOT TOOL_ISOLATED)
    list(APPEND FAILURES
      "cmake/ValidationTools.cmake: ${TOOL} must remain EXCLUDE_FROM_ALL")
  endif()
endforeach()

if(FAILURES)
  list(REMOVE_DUPLICATES FAILURES)
  list(JOIN FAILURES "\n" MESSAGE_TEXT)
  message(FATAL_ERROR "Platform runtime safety policy failed:\n${MESSAGE_TEXT}")
endif()

message(STATUS "Platform runtime safety policy passed")
