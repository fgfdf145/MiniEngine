add_library(engine_editor
    command_registry.cpp
    command_registry.h
    editor_backend_base.cpp
    editor_backend_base.h
    editor_commands.cpp
    editor_commands.h
    editor_icons.cpp
    editor_icons.h
    editor_ui.cpp
    editor_ui.h
    engine_settings.cpp
    engine_settings.h
    imgui_frame_snapshot.cpp
    imgui_frame_snapshot.h
    renderer_shared_state.h
    services/capture_state.cpp
    services/capture_state.h
    services/entity_edit_service.cpp
    services/entity_edit_service.h
    services/model_animation_service.cpp
    services/model_animation_service.h
    services/model_import_service.cpp
    services/model_import_service.h
    services/quad_recording.cpp
    services/quad_recording.h
    services/scene_io_service.cpp
    services/scene_io_service.h
    services/scene_renderables.cpp
    services/scene_renderables.h
    services/vehicle_drive_service.cpp
    services/vehicle_rig_service.cpp
    services/vehicle_rig_service.h
    services/vehicle_drive_service.h
    services/vehicle_driver_service.cpp
    services/vehicle_driver_service.h
    services/vehicle_gear_shift.cpp
    services/vehicle_gear_shift.h
    services/vehicle_haptics.cpp
    services/vehicle_haptics.h
    services/vehicle_steering_assist.cpp
    services/vehicle_steering_assist.h
    services/world_streaming_service.cpp
    services/world_streaming_service.h
    ui/editor_dockspace.cpp
    ui/editor_gt7_hud.cpp
    ui/editor_gt7_hud.h
    ui/editor_material_graph.cpp
    ui/editor_material_graph.h
    ui/editor_menu_toolbar.cpp
    ui/editor_menu_toolbar.h
    ui/editor_model_preview.cpp
    ui/editor_model_preview.h
    ui/editor_ui_internal.h
    ui/editor_ui_shared.cpp
    ui/editor_vehicle_overlay.cpp
    ui/editor_vehicle_overlay.h
    ui/framework/editor_context.h
    ui/framework/editor_modal.cpp
    ui/framework/editor_modal.h
    ui/framework/editor_panel.h
    ui/framework/editor_style.cpp
    ui/framework/editor_style.h
    ui/framework/editor_window.cpp
    ui/framework/editor_window.h
    ui/framework/editor_window_manager.cpp
    ui/framework/editor_window_manager.h
    ui/modals/about_modal.cpp
    ui/modals/about_modal.h
    ui/modals/import_conflict_modal.cpp
    ui/modals/import_conflict_modal.h
    ui/modals/kn5_import_modal.cpp
    ui/modals/kn5_import_modal.h
    ui/modals/scene_reset_modal.cpp
    ui/modals/scene_reset_modal.h
    ui/panels/asset_browser_panel.cpp
    ui/panels/asset_browser_panel.h
    ui/panels/camera_panel.cpp
    ui/panels/camera_panel.h
    ui/panels/graphics_debug_panel.cpp
    ui/panels/graphics_debug_panel.h
    ui/panels/input_monitor_panel.cpp
    ui/panels/input_monitor_panel.h
    ui/panels/quad_recording_panel.cpp
    ui/panels/quad_recording_panel.h
    ui/panels/scene_panel.cpp
    ui/panels/scene_panel.h
    ui/panels/suspension_rigs_panel.cpp
    ui/panels/suspension_rigs_panel.h
    ui/panels/theme_panel.cpp
    ui/panels/theme_panel.h
    ui/panels/vehicle_panel.cpp
    ui/panels/vehicle_panel.h
    ui/panels/viewport_panel.cpp
    ui/panels/viewport_panel.h
    ui/windows/keyboard_shortcuts_window.cpp
    ui/windows/keyboard_shortcuts_window.h
    ui/windows/model_processor_window.cpp
    ui/windows/model_processor_window.h
    ui/windows/preferences_window.cpp
    ui/windows/preferences_window.h
    ui_colors.h
    view_settings_fields.h
)
miniengine_group_target_sources(engine_editor)

# The Phosphor icon font, built into the editor so it needs no file at run time.
set(_editor_icon_font "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor.ttf")
set(_editor_icon_font_source "${CMAKE_CURRENT_BINARY_DIR}/generated/phosphor_regular_ttf.cpp")
add_custom_command(
    OUTPUT "${_editor_icon_font_source}"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${_editor_icon_font}"
        "-DOUTPUT=${_editor_icon_font_source}"
        -DSYMBOL=kPhosphorRegularTtf
        -P "${PROJECT_SOURCE_DIR}/cmake/MiniEngineEmbedFile.cmake"
    DEPENDS "${_editor_icon_font}" "${PROJECT_SOURCE_DIR}/cmake/MiniEngineEmbedFile.cmake"
    COMMENT "Embedding Phosphor.ttf"
    VERBATIM
)
target_sources(engine_editor PRIVATE "${_editor_icon_font_source}")
source_group("Generated" FILES "${_editor_icon_font_source}")

# The GT7 driving HUD's fonts and icons (SVG, written by tools/gt7_hud/make_gt7_hud_svgs.py), built in
# the same way.
set(_gt7_hud_svgs
    fonts/gt7_meter fonts/gt7_sans fonts/gt7_sans_bold
    icons/abs icons/arrow_left icons/arrow_right icons/brake_assist icons/car_lines icons/car_markers icons/car_top
    icons/countersteer icons/fuel icons/handbrake icons/headlight icons/steering icons/tcs icons/throttle
    icons/turbo icons/warning icons/water)
foreach(_svg IN LISTS _gt7_hud_svgs)
    get_filename_component(_svg_name "${_svg}" NAME)
    set(_svg_input "${CMAKE_CURRENT_SOURCE_DIR}/ui/gt7_hud/${_svg}.svg")
    set(_svg_source "${CMAKE_CURRENT_BINARY_DIR}/generated/gt7_hud_${_svg_name}_svg.cpp")
    add_custom_command(
        OUTPUT "${_svg_source}"
        COMMAND ${CMAKE_COMMAND}
            "-DINPUT=${_svg_input}"
            "-DOUTPUT=${_svg_source}"
            "-DSYMBOL=kGt7HudSvg_${_svg_name}"
            -P "${PROJECT_SOURCE_DIR}/cmake/MiniEngineEmbedFile.cmake"
        DEPENDS "${_svg_input}" "${PROJECT_SOURCE_DIR}/cmake/MiniEngineEmbedFile.cmake"
        COMMENT "Embedding gt7_hud/${_svg}.svg"
        VERBATIM
    )
    target_sources(engine_editor PRIVATE "${_svg_source}")
    source_group("Generated" FILES "${_svg_source}")
endforeach()

target_include_directories(engine_editor
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/phosphor"
)

target_link_libraries(engine_editor
    PUBLIC
        engine_render_core
        engine_audio
        engine_logic
        engine_physics
        engine_asset
        engine_platform
        engine_scene
        engine_core
        SDL3::SDL3
        glm::glm
        imgui::imgui
        imguizmo::imguizmo
        implot::implot
        EnTT::EnTT
    PRIVATE
        yaml-cpp::yaml-cpp
)
