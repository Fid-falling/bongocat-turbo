set(BONGO_CAT_RUNTIME_INTERNAL_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/input"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/lifecycle"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/model"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/model/import"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/model/import/mver"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/model/import/nearby"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/shell"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/update"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/backend"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/preferences"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/rendering"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/theme")

set(BONGO_CAT_MEDIA_SOURCES
  src/media/audio/audio.c
  src/media/audio/audio_playback.c
  src/media/image.c
  src/media/image_decode.c
  src/media/image_info.c
  src/media/image_mipmap.c
  src/media/image_resize.c
  src/media/image_alpha.c
  src/media/stb_image_impl.c)

set(BONGO_CAT_RENDER_SOURCES
  src/render/render_backend.c
  src/render/render_texture.c
  src/render/gl_api.c
  src/render/overlay.c
  src/render/overlay_assets.c
  src/render/overlay_input.c
  src/render/overlay_draw.c
  src/render/mver_pointer_overlay.c
  src/render/mver_pointer_overlay_draw.c
  # Vulkan 后端。文件内部按 BONGO_CAT_HAS_VULKAN 分叉: 未启用时只编出
  # vk_stub.c 里那组返回失败/空指针的桩, 让调用方不必到处写 #ifdef。
  src/render/vk/vk_core.c
  # 上下文生命周期与查询 (vk_core.c 只留 Instance/Device 的创建销毁)
  src/render/vk/vk_context.c
  src/render/vk/vk_stub.c
  src/render/vk/vk_swapchain.c
  src/render/vk/vk_swapchain_rebuild.c
  src/render/vk/vk_frame.c
  # 帧同步对象 (信号量/栅栏/命令池) 的创建、重置与销毁
  src/render/vk/vk_frame_sync.c
  src/render/vk/vk_readback.c
  src/render/vk/vk_texture.c
  src/render/vk/vk_pipeline.c
  src/render/vk/vk_draw.c
  src/render/vk/vk_scene.c)

set(BONGO_CAT_RUNTIME_LIFECYCLE_SOURCES
  src/runtime/lifecycle/app.c
  src/runtime/lifecycle/app_entry.c
  # "改完设置自动重启" 的调度 (渲染后端切换、便携模式开关共用)
  src/runtime/lifecycle/application_restart.c
  src/runtime/lifecycle/app_input.c
  src/runtime/lifecycle/app_loop.c
  src/runtime/lifecycle/assets.c
  src/runtime/lifecycle/config_store.c
  src/runtime/lifecycle/frame_clock.c
  src/runtime/lifecycle/memory_policy.c
  # 便携模式 (BongoCatData): 标记判定 + 设置/模型的搬迁
  src/runtime/lifecycle/portable_mode.c
  # 搬迁用的复制/删除原语 (单独一份, 免得 portable_mode.c 撞上单文件行数上限)
  src/runtime/lifecycle/portable_transfer.c
  # "切换渲染后端"的唯一实现: 设置页分段选择器与托盘菜单共用同一条路径
  src/runtime/lifecycle/renderer_switch.c
  src/runtime/lifecycle/runtime_flow.c
  # --ci-model-switch= 的自测驱动器 (连点模型 / 切到坏模型): 只登记请求, 不做断言
  src/runtime/lifecycle/smoke_model_switch.c
  src/runtime/lifecycle/runtime_state.c
  src/runtime/lifecycle/shutdown.c
  src/runtime/lifecycle/startup.c
  src/runtime/lifecycle/startup_arguments.c
  src/runtime/lifecycle/system_language.c
  src/runtime/lifecycle/storage_paths.c)

# Live2D 渲染组件(<exe>/cores/bongo_live2d.*)的目录解析与启动扫描
# (见 include/bongo_cat/renderer_module.h): 只做发现与"能不能用/为什么不能用"的判定,
# 不实例化 —— 实例化要等组件 ABI 与图形上下文就绪, 那是 app.c 里装载时机的事。
set(BONGO_CAT_RUNTIME_RENDERER_MODULE_SOURCES
  src/runtime/cores/renderer_module.c
  # 组件 ABI 校验与装载(纯函数校验 + dlopen/回滚), 见 renderer_module_abi.h
  src/runtime/cores/renderer_module_loader.c
  # 宿主服务表(组件唯一入口): 每条都是一份跨界契约, 集中一处便于审计
  src/runtime/cores/renderer_module_services.c)

set(BONGO_CAT_RUNTIME_UPDATE_SOURCES
  src/runtime/update/update_channel.c
  src/runtime/update/update_http.c
  src/runtime/update/update_http_unix.c
  # 一键更新的"下载 + 校验"阶段 (多源回退, 独立于状态机)
  src/runtime/update/update_download.c
  # 替换/重启的调度与启动收尾 (.old 备份、失败回执)
  src/runtime/update/update_install.c
  # 下载源列表的配置覆盖 (update-sources.txt) 与"记住上次成功的源"
  src/runtime/update/update_source_store.c
  src/runtime/update/update_service.c)

set(BONGO_CAT_RUNTIME_DIAGNOSTIC_SOURCES
  src/runtime/diagnostics/frame_audit.c
  # "主循环有没有在出帧"的量化探针 (帧间隔最大值/分位数): 与 frame_audit 的帧内容口径互补,
  # 也是透明背景走代理分层呈现时唯一能用的帧节奏口径 (那条路径下后台缓冲不逐帧提交)。
  src/runtime/diagnostics/frame_interval_probe.c
  src/runtime/diagnostics/frame_presentation_audit.c
  src/runtime/diagnostics/live2d_audit.c
  src/runtime/diagnostics/live2d_mouse_mapping_audit.c
  src/runtime/diagnostics/live2d_pointer_audit.c
  src/runtime/diagnostics/live2d_audit_scenario.c
  src/runtime/diagnostics/live2d_viewer_audit.c
  src/runtime/diagnostics/live2d_viewer_timing.c
  src/runtime/diagnostics/live2d_visual_audit.c)

set(BONGO_CAT_RUNTIME_INPUT_SOURCES
  src/runtime/input/gamepad.c
  src/runtime/input/mouse.c
  src/runtime/input/mouse_bounds.c
  src/runtime/input/mouse_mapping.c
  src/runtime/input/mouse_pipeline.c
  src/runtime/input/shortcuts.c
  src/runtime/input/sound_shortcuts.c)

set(BONGO_CAT_RUNTIME_MODEL_SOURCES
  src/runtime/model/model_behavior.c
  src/runtime/model/model_behavior_cache.c
  src/runtime/model/model_behavior_state.c
  src/runtime/model/model_catalog_runtime.c
  src/runtime/model/model_catalog_builtins.c
  src/runtime/model/model_catalog_commit.c
  src/runtime/model/model_catalog_remove.c
  src/runtime/model/model_catalog_refresh.c
  src/runtime/model/model_catalog_selection.c
  src/runtime/model/model_cover.c
  src/runtime/model/model_cover_queue.c
  src/runtime/model/model_files.c
  src/runtime/model/model_geometry.c
  # 换桌宠的异步队列: 影子准备 + 逐帧预算推进 + 失败回退 (见 model_switch.h 的说明)。
  # 拆成两份是为了让 model_files.c 不撞上单文件行数上限, 也为了让"准备/提交"与"调度"分开。
  src/runtime/model/model_switch.c
  src/runtime/model/model_switch_shadow.c
  src/runtime/model/multi_pet.c
  src/runtime/model/multi_pet_selection.c
  src/runtime/model/multi_pet_process.c
  src/runtime/model/multi_pet_settings.c
  src/runtime/model/model_storage.c
  src/runtime/model/model_update.c)

set(BONGO_CAT_RUNTIME_IMPORT_SOURCES
  # Shared import workflow.
  src/runtime/model/import/model_import_app.c
  src/runtime/model/import/model_import_archive.c
  src/runtime/model/import/model_import_adapter.c
  src/runtime/model/import/model_import_digest.c
  src/runtime/model/import/model_import_digest_api.c
  src/runtime/model/import/model_import_digest_cache.c
  src/runtime/model/import/model_import_digest_path.c
  src/runtime/model/import/model_import_discover.c
  src/runtime/model/import/model_import_manifest.c
  src/runtime/model/import/model_import_identity.c
  src/runtime/model/import/model_import_installed.c
  src/runtime/model/import/model_import_install.c
  src/runtime/model/import/model_import_lock.c
  src/runtime/model/import/model_import_metadata.c
  src/runtime/model/import/model_import_package.c
  src/runtime/model/import/model_import_package_index.c
  src/runtime/model/import/model_import_path.c
  src/runtime/model/import/model_import_probe.c
  src/runtime/model/import/model_import_progressive.c
  src/runtime/model/import/model_import_report.c
  src/runtime/model/import/model_import_scan.c
  src/runtime/model/import/model_import_session.c
  src/runtime/model/import/model_import_source.c

  # Mver is the canonical package and runtime-adapter format.
  src/runtime/model/import/mver/model_import_mver_assets.c
  src/runtime/model/import/mver/model_import_mver_copy.c
  src/runtime/model/import/mver/model_import_mver_discovery.c
  src/runtime/model/import/mver/model_import_mver_manifest.c
  src/runtime/model/import/mver/model_import_mver_effect.c
  src/runtime/model/import/mver/model_import_mver_image.c
  src/runtime/model/import/mver/model_import_mver_labels.c
  src/runtime/model/import/mver/model_import_mver_metadata.c
  src/runtime/model/import/mver/model_import_mver_audio.c
  src/runtime/model/import/mver/model_import_mver_motion.c
  src/runtime/model/import/mver/model_import_mver_patch.c
  src/runtime/model/import/mver/model_import_mver_policy.c
  src/runtime/model/import/mver/model_import_mver_shortcut.c

  # Tauri packages are converted to canonical Mver packages before loading.
  src/runtime/model/import/tauri/model_import_tauri_candidate.c
  src/runtime/model/import/tauri/model_import_tauri_config.c
  src/runtime/model/import/tauri/model_import_tauri_convert.c
  src/runtime/model/import/tauri/model_import_tauri_discovery.c
  src/runtime/model/import/tauri/model_import_tauri_images.c
  src/runtime/model/import/tauri/model_import_tauri_input.c
  src/runtime/model/import/tauri/model_import_tauri_keys.c
  src/runtime/model/import/tauri/model_import_tauri_legacy.c
  src/runtime/model/import/tauri/model_import_tauri_metadata.c
  src/runtime/model/import/tauri/model_import_tauri_resources.c
  src/runtime/model/import/tauri/model_import_tauri_tree.c

  # Nearby discovery builds adapters without installing source packages.
  src/runtime/model/import/nearby/model_import_nearby.c
  src/runtime/model/import/nearby/model_import_nearby_cache.c
  src/runtime/model/import/nearby/model_import_nearby_signature.c)

set(BONGO_CAT_RUNTIME_SHELL_SOURCES
  src/runtime/shell/modal_frame.c
  # 托盘拆成三份: 本体 (建/销毁/同步)、渲染后端逃生项、--ci-menu 自测。
  src/runtime/shell/tray.c
  src/runtime/shell/tray_renderer.c
  src/runtime/shell/tray_self_test.c
  src/runtime/shell/window.c
  src/runtime/shell/window_background.c
  src/runtime/shell/window_display.c
  # 显示器几何的纯计算 + 自检 (window_display.c 只留"什么时候该动窗口")。
  src/runtime/shell/window_display_geometry.c
  # 松手后的惯性滑行 (含"关闭动量衰减"的永不停止模式): 逐帧状态机, 与窗口几何分开。
  src/runtime/shell/window_inertia.c
  src/runtime/shell/window_drag.c
  src/runtime/shell/window_geometry.c
  src/runtime/shell/window_hit.c
  src/runtime/shell/window_menu_actions.c
  src/runtime/shell/window_menu_behavior.c
  src/runtime/shell/window_menu_behavior_test.c
  src/runtime/shell/window_menu_preview.c
  src/runtime/shell/window_scale.c
  src/runtime/shell/window_snapshot.c
  src/runtime/shell/window_wheel.c
  src/runtime/shell/window_wheel_test.c)

set(BONGO_CAT_UI_BACKEND_SOURCES
  src/ui/backend/nuklear_impl.c
  src/ui/backend/ui_backend.c
  src/ui/backend/ui_backend_fonts.c
  src/ui/backend/ui_cursor.c
  src/ui/backend/ui_input.c
  src/ui/backend/ui_resize_cache.c
  src/ui/backend/ui_scale.c
  src/ui/backend/ui_tooltip.c)

set(BONGO_CAT_UI_RENDERING_SOURCES
  src/ui/dial/dial_window.c
  # 模态循环 / 失败回退 (dial_window.c 只管建窗)
  src/ui/dial/dial_loop.c
  src/ui/dial/dial_input.c
  src/ui/dial/dial_items.c
  src/ui/dial/dial_geometry.c
  src/ui/dial/dial_paint.c
  src/ui/dial/dial_draw.c
  src/ui/dial/dial_scene.c
  src/ui/dial/dial_fonts.c
  src/ui/dial/dial_covers.c
  src/ui/dial/dial_icons.c
  src/ui/rendering/ui_animation.c
  src/ui/rendering/ui_font.c
  src/ui/rendering/ui_font_atlas.c
  src/ui/rendering/ui_font_atlas_ranges.c
  src/ui/rendering/ui_font_atlas_sources.c
  src/ui/rendering/ui_font_atlas_upload.c
  src/ui/rendering/ui_font_reload.c
  src/ui/rendering/ui_native_theme.c
  src/ui/rendering/ui_paint.c
  src/ui/rendering/ui_paint_border.c
  src/ui/rendering/ui_paint_cache.c
  src/ui/rendering/ui_paint_lifecycle.c
  src/ui/rendering/ui_paint_shell.c)

set(BONGO_CAT_UI_THEME_SOURCES
  src/ui/theme/ui_catime.c
  src/ui/theme/ui_catime_icons.c
  src/ui/theme/ui_catime_tabs.c
  src/ui/theme/ui_catime_theme.c)

set(BONGO_CAT_UI_PREFERENCES_SOURCES
  src/ui/preferences/preferences.c
  # Live2D Cubism Core 的设置入口(状态行 + 选择压缩包 + 拖放路由 + 一次性提示)。
  # 单独成文件: 它是"法律合规流程的用户可见部分", 与渲染/模型页的改动无关。
  src/ui/preferences/preferences_live2d_setup.c
  src/ui/preferences/preferences_about.c
  src/ui/preferences/preferences_about_community.c
  src/ui/preferences/preferences_about_footer.c
  src/ui/preferences/preferences_about_page.c
  src/ui/preferences/preferences_assets.c
  src/ui/preferences/preferences_cache_path.c
  src/ui/preferences/preferences_behavior_dialog.c
  src/ui/preferences/preferences_behavior_rename.c
  src/ui/preferences/preferences_behavior_row.c
  src/ui/preferences/preferences_combo.c
  src/ui/preferences/preferences_controls.c
  src/ui/preferences/preferences_number_edit.c
  src/ui/preferences/preferences_dialog.c
  src/ui/preferences/preferences_form_label.c
  src/ui/preferences/preferences_fonts.c
  src/ui/preferences/preferences_gl.c
  src/ui/preferences/preferences_import.c
  src/ui/preferences/preferences_import_dialog.c
  src/ui/preferences/preferences_import_progress.c
  src/ui/preferences/preferences_import_worker.c
  src/ui/preferences/preferences_icons.c
  src/ui/preferences/preferences_language.c
  src/ui/preferences/preferences_lifecycle.c
  src/ui/preferences/preferences_live_resize.c
  src/ui/preferences/preferences_model.c
  src/ui/preferences/preferences_model_card.c
  src/ui/preferences/preferences_model_card_paint.c
  src/ui/preferences/preferences_model_cover.c
  src/ui/preferences/preferences_model_section.c
  src/ui/preferences/preferences_model_watch.c
  src/ui/preferences/preferences_model_glyphs.c
  src/ui/preferences/preferences_model_import.c
  src/ui/preferences/preferences_model_rename.c
  src/ui/preferences/preferences_notice.c
  src/ui/preferences/preferences_overlay.c
  src/ui/preferences/preferences_page_dispatch.c
  # [应用] 区与 macOS 输入监控区 (从 preferences_pages.c 拆出, 见行数策略)
  src/ui/preferences/preferences_general_page.c
  src/ui/preferences/preferences_pages.c
  # 模型页的"随机"标签页: 三个随机分区 + 顺序轮换 (从 preferences_pages.c 拆出, 见行数策略)
  src/ui/preferences/preferences_random_page.c
  src/ui/preferences/preferences_portable.c
  src/ui/preferences/preferences_present.c
  src/ui/preferences/preferences_render.c
  src/ui/preferences/preferences_row_icons.c
  src/ui/preferences/preferences_scale.c
  src/ui/preferences/preferences_scrollbar.c
  src/ui/preferences/preferences_section_icons.c
  # 分段选择器的叶子实现: 手绘图标 + 按控件去重的点击判定 (从 preferences_theme.c 拆出)
  src/ui/preferences/preferences_segment.c
  src/ui/preferences/preferences_shortcut_clear.c
  src/ui/preferences/preferences_renderer_row.c
  src/ui/preferences/preferences_shortcuts.c
  src/ui/preferences/preferences_smoke.c
  src/ui/preferences/preferences_text_edit.c
  src/ui/preferences/preferences_text_session.c
  src/ui/preferences/preferences_theme.c
  src/ui/preferences/preferences_toggle.c
  # 一键更新的状态条 (下载进度/当前源/失败原因), 与一次性提示条分开
  src/ui/preferences/preferences_update_status.c
  src/ui/preferences/preferences_widget_shortcut.c
  src/ui/preferences/preferences_widgets.c
  src/ui/preferences/preferences_widgets_compound.c
  src/ui/preferences/preferences_window_transparency.c)

set(BONGO_CAT_RUNTIME_SOURCES
  src/core/app_state.c
  src/platform/common/application_restart.c
  src/platform/common/memory.c
  src/platform/common/update_shutdown.c
  ${BONGO_CAT_MEDIA_SOURCES}
  ${BONGO_CAT_RENDER_SOURCES}
  ${BONGO_CAT_RUNTIME_LIFECYCLE_SOURCES}
  ${BONGO_CAT_RUNTIME_UPDATE_SOURCES}
  ${BONGO_CAT_RUNTIME_DIAGNOSTIC_SOURCES}
  ${BONGO_CAT_RUNTIME_INPUT_SOURCES}
  ${BONGO_CAT_RUNTIME_MODEL_SOURCES}
  ${BONGO_CAT_RUNTIME_RENDERER_MODULE_SOURCES}
  ${BONGO_CAT_RUNTIME_IMPORT_SOURCES}
  ${BONGO_CAT_RUNTIME_SHELL_SOURCES}
  ${BONGO_CAT_UI_BACKEND_SOURCES}
  ${BONGO_CAT_UI_RENDERING_SOURCES}
  ${BONGO_CAT_UI_THEME_SOURCES}
  ${BONGO_CAT_UI_PREFERENCES_SOURCES})


