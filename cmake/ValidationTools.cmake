if(WIN32)
  add_library(bongo_cat_tool_support STATIC EXCLUDE_FROM_ALL
    src/tools/windows_tool.c)
  target_include_directories(bongo_cat_tool_support PUBLIC src/tools)
  target_link_libraries(bongo_cat_tool_support PUBLIC bongo_cat_warnings)

  add_executable(cubism_viewer_desktop_capture EXCLUDE_FROM_ALL
    src/tools/cubism_viewer_desktop_capture.c
    src/tools/cubism_viewer_desktop_capture_dxgi.c)
  target_compile_definitions(cubism_viewer_desktop_capture PRIVATE
    _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(cubism_viewer_desktop_capture PRIVATE
    bongo_cat_tool_support bongo_cat_warnings d3d11 dxgi dxguid user32)

  add_library(bongo_cat_validation_image STATIC EXCLUDE_FROM_ALL
    src/tools/validation_image.c src/tools/validation_gdiplus.c)
  target_include_directories(bongo_cat_validation_image PUBLIC src/tools)
  target_link_libraries(bongo_cat_validation_image PUBLIC
    bongo_cat_warnings windowscodecs gdiplus ole32)

  add_executable(cubism_viewer_drag_capture EXCLUDE_FROM_ALL
    src/tools/cubism_viewer_drag_capture.c)
  target_compile_definitions(cubism_viewer_drag_capture PRIVATE
    _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(cubism_viewer_drag_capture PRIVATE
    bongo_cat_validation_image bongo_cat_tool_support bongo_cat_warnings
    user32 gdi32 ole32)

  add_executable(cubism_viewer_blind_test EXCLUDE_FROM_ALL
    src/tools/cubism_viewer_blind_test.c
    src/tools/cubism_viewer_blind_frames.c)
  target_compile_definitions(cubism_viewer_blind_test PRIVATE
    _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(cubism_viewer_blind_test PRIVATE
    bongo_cat_validation_image bongo_cat_tool_support bongo_cat_warnings ole32)

  add_executable(mver_blind_metrics EXCLUDE_FROM_ALL
    src/tools/mver_blind_metrics.c)
  target_compile_definitions(mver_blind_metrics PRIVATE
    _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(mver_blind_metrics PRIVATE
    bongo_cat_validation_image bongo_cat_warnings ole32)

  add_executable(mver_phase_metrics EXCLUDE_FROM_ALL
    src/tools/mver_phase_metrics.c
    src/tools/mver_phase_frames.c)
  target_compile_definitions(mver_phase_metrics PRIVATE
    _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(mver_phase_metrics PRIVATE
    bongo_cat_validation_image bongo_cat_warnings ole32)

  add_custom_target(bongo_cat_validation_tools
    DEPENDS cubism_viewer_desktop_capture cubism_viewer_drag_capture
      cubism_viewer_blind_test mver_blind_metrics mver_phase_metrics)
endif()
