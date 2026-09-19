set(CUBISM_CORE_PATH "${BONGO_CAT_CUBISM_SDK}/Core")
set(CUBISM_FRAMEWORK_PATH "${BONGO_CAT_CUBISM_SDK}/Framework")
set(CUBISM_GLEW_PATH "${BONGO_CAT_CUBISM_SDK}/Samples/OpenGL/thirdParty/glew")

foreach(PATH IN ITEMS CUBISM_FRAMEWORK_PATH CUBISM_GLEW_PATH)
  if(NOT EXISTS "${${PATH}}")
    message(FATAL_ERROR "Incomplete Cubism SDK: ${${PATH}} is missing")
  endif()
endforeach()

add_library(Live2DCubismCore STATIC IMPORTED GLOBAL)
if(WIN32)
  # 官方只提供 v143 的 Windows Core 库, 上游这里要求 1930 <= MSVC_VERSION < 1950。
  # MSVC 14.x (v143 之后) 保持二进制兼容, 所以这里允许更新的工具集 (例如 VS 18 的
  # 14.51/v145) 直接链 143 的库, 只提示风险; 低于 v143 仍然拒绝。
  if(MSVC_VERSION LESS 1930)
    message(FATAL_ERROR "Cubism Windows build requires Visual Studio 2022 (v143) or newer")
  endif()
  if(MSVC_VERSION GREATER_EQUAL 1950)
    message(WARNING
      "Cubism Core 库是 v143 构建的, 当前工具集 MSVC_VERSION=${MSVC_VERSION}; "
      "MSVC 14.x 二进制兼容, 若链接或运行异常请换 -T v143。")
  endif()
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CUBISM_WINDOWS_ARCH "x86_64")
  else()
    set(CUBISM_WINDOWS_ARCH "x86")
  endif()
  set(CUBISM_CORE_LIBRARY
    "${CUBISM_CORE_PATH}/lib/windows/${CUBISM_WINDOWS_ARCH}/143/Live2DCubismCore_MT.lib")
elseif(APPLE)
  if(CMAKE_OSX_ARCHITECTURES)
    list(GET CMAKE_OSX_ARCHITECTURES 0 CUBISM_ARCH)
  else()
    set(CUBISM_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  set(CUBISM_CORE_LIBRARY
    "${CUBISM_CORE_PATH}/lib/macos/${CUBISM_ARCH}/libLive2DCubismCore.a")
else()
  set(CUBISM_CORE_LIBRARY
    "${CUBISM_CORE_PATH}/lib/linux/x86_64/libLive2DCubismCore.a")
endif()
if(NOT EXISTS "${CUBISM_CORE_LIBRARY}")
  message(FATAL_ERROR "Cubism Core library missing: ${CUBISM_CORE_LIBRARY}")
endif()
set_target_properties(Live2DCubismCore PROPERTIES
  IMPORTED_LOCATION "${CUBISM_CORE_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${CUBISM_CORE_PATH}/include")

add_library(glew_s STATIC "${CUBISM_GLEW_PATH}/src/glew.c")
target_include_directories(glew_s SYSTEM PUBLIC "${CUBISM_GLEW_PATH}/include")
target_compile_definitions(glew_s PUBLIC GLEW_STATIC GLEW_NO_GLU)
target_link_libraries(glew_s PUBLIC OpenGL::GL)
if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  target_link_libraries(glew_s PRIVATE X11::X11 ${CMAKE_DL_LIBS})
endif()
set(FRAMEWORK_SOURCE OpenGL)
add_subdirectory("${CUBISM_FRAMEWORK_PATH}" "${CMAKE_BINARY_DIR}/cubism-framework")
include(cmake/CubismUserModelSafety.cmake)
bongo_cat_harden_cubism_user_model(Framework)
include(cmake/CubismCoreProfile.cmake)
include(cmake/CubismShaderOptimize.cmake)
bongo_cat_optimize_cubism_shaders(Framework)
if(APPLE)
  bongo_cat_core_profile_prepare_shaders()
  bongo_cat_core_profile_patch_renderer(Framework)
endif()

if(WIN32)
  target_compile_definitions(Framework PUBLIC CSM_TARGET_WIN_GL)
elseif(APPLE)
  target_compile_definitions(Framework PUBLIC CSM_TARGET_MAC_GL)
else()
  target_compile_definitions(Framework PUBLIC CSM_TARGET_LINUX_GL)
endif()
target_include_directories(Framework SYSTEM PUBLIC
  "${CUBISM_FRAMEWORK_PATH}/src"
  "${CUBISM_CORE_PATH}/include")
target_link_libraries(Framework PUBLIC Live2DCubismCore glew_s)
