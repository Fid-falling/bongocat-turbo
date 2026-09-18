if(NOT DEFINED ROOT)
  message(FATAL_ERROR "ROOT is required")
endif()

file(GLOB_RECURSE SOURCES
  "${ROOT}/src/*.c" "${ROOT}/src/*.cc" "${ROOT}/src/*.cpp"
  "${ROOT}/src/*.cxx" "${ROOT}/src/*.m" "${ROOT}/src/*.mm"
  "${ROOT}/src/*.h" "${ROOT}/src/*.hh" "${ROOT}/src/*.hpp"
  "${ROOT}/src/*.hxx" "${ROOT}/include/*.h" "${ROOT}/include/*.hh"
  "${ROOT}/include/*.hpp" "${ROOT}/include/*.hxx")
set(KEYS "")
foreach(SOURCE IN LISTS SOURCES)
  file(READ "${SOURCE}" CONTENT)
  string(REGEX MATCHALL
    "\"(native|pages|components|composables)\\.[A-Za-z0-9_.]+\""
    FOUND_KEYS "${CONTENT}")
  foreach(FOUND IN LISTS FOUND_KEYS)
    string(LENGTH "${FOUND}" LENGTH)
    math(EXPR LAST "${LENGTH} - 2")
    string(SUBSTRING "${FOUND}" 1 ${LAST} KEY)
    list(APPEND KEYS "${KEY}")
  endforeach()
endforeach()
list(REMOVE_DUPLICATES KEYS)

file(GLOB LOCALES "${ROOT}/resources/assets/locales/*.json")
if(NOT LOCALES)
  message(FATAL_ERROR "No locale files found")
endif()
foreach(LOCALE IN LISTS LOCALES)
  file(READ "${LOCALE}" JSON)
  foreach(KEY IN LISTS KEYS)
    string(REPLACE "." ";" PARTS "${KEY}")
    set(VALUE "${JSON}")
    foreach(PART IN LISTS PARTS)
      string(JSON VALUE ERROR_VARIABLE ERROR GET "${VALUE}" "${PART}")
      if(ERROR)
        message(FATAL_ERROR
          "Missing localization key '${KEY}' in ${LOCALE}")
      endif()
    endforeach()
  endforeach()
endforeach()
list(LENGTH KEYS KEY_COUNT)
message(STATUS "Localization key policy passed (${KEY_COUNT} keys)")
