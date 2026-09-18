set(BONGO_CAT_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(cmake/version.h.in
  "${BONGO_CAT_GENERATED_INCLUDE_DIR}/bongo_cat/version.h" @ONLY)
