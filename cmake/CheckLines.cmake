cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED ROOT)
  message(FATAL_ERROR "ROOT is required")
endif()

file(GLOB_RECURSE FILES
  "${ROOT}/src/*.c" "${ROOT}/src/*.cc" "${ROOT}/src/*.cpp"
  "${ROOT}/src/*.cxx" "${ROOT}/src/*.m" "${ROOT}/src/*.mm"
  "${ROOT}/src/*.h" "${ROOT}/src/*.hh" "${ROOT}/src/*.hpp" "${ROOT}/src/*.hxx"
  "${ROOT}/include/*.h" "${ROOT}/include/*.hh"
  "${ROOT}/include/*.hpp" "${ROOT}/include/*.hxx"
  "${ROOT}/tests/*.c" "${ROOT}/tests/*.cc" "${ROOT}/tests/*.cpp"
  "${ROOT}/tests/*.cxx" "${ROOT}/tests/*.m" "${ROOT}/tests/*.mm"
  "${ROOT}/tests/*.h" "${ROOT}/tests/*.hh"
  "${ROOT}/tests/*.hpp" "${ROOT}/tests/*.hxx"
  "${ROOT}/cmake/*.cmake")
list(APPEND FILES "${ROOT}/CMakeLists.txt")

set(FAILED "")
foreach(FILE IN LISTS FILES)
  # 不能用 file(STRINGS) + list(LENGTH) 数行: 它的结果是 CMake 列表, 行内的分号会被当成
  # 项分隔符, 一行里每个分号都算成额外一行 (471 行的文件会被算成 709)。这里直接数物理换行符,
  # 口径与 wc -l 一致。
  file(READ "${FILE}" CONTENTS)
  string(REGEX MATCHALL "\n" NEWLINES "${CONTENTS}")
  list(LENGTH NEWLINES COUNT)
  file(RELATIVE_PATH RELATIVE_FILE "${ROOT}" "${FILE}")
  string(REPLACE "\\" "/" RELATIVE_FILE "${RELATIVE_FILE}")

  set(LIMIT 500)

  if(COUNT GREATER LIMIT)
    list(APPEND FAILED "${RELATIVE_FILE}: ${COUNT} (limit ${LIMIT})")
  endif()
endforeach()

if(FAILED)
  list(JOIN FAILED "\n" MESSAGE_TEXT)
  message(FATAL_ERROR "Files over their line limit:\n${MESSAGE_TEXT}")
endif()

message(STATUS "Line policy passed")
