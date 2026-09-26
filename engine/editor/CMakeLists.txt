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
    renderer_shared_state.h
    services/entity_edit_service.cpp
    services/entity_edit_service.h
    services/model_import_service.cpp
    services/model_import_service.h
    services/scene_io_service.cpp
    services/scene_io_service.h
    services/scene_renderables.cpp
    services/scene_renderables.h
    ui/editor_asset_browser_panel.cpp
    ui/editor_dockspace.cpp
    ui/editor_material_graph.cpp
    ui/editor_material_graph.h
    ui/editor_menu_toolbar.cpp
    ui/editor_menu_toolbar.h
    ui/editor_misc_panels.cpp
    ui/editor_model_preview.cpp
    ui/editor_model_preview.h
    ui/editor_model_processor_panel.cpp
    ui/editor_scene_panel.cpp
    ui/editor_theme_panel.cpp
    ui/editor_ui_internal.h
    ui/editor_ui_shared.cpp
    ui/editor_viewport_panel.cpp
)
miniengine_group_target_sources(engine_editor)

# The Font Awesome icon font, built into the editor so it needs no file at run time.
set(_editor_icon_font "${PROJECT_SOURCE_DIR}/third_party/fontawesome/fa-solid-900.ttf")
set(_editor_icon_font_source "${CMAKE_CURRENT_BINARY_DIR}/generated/fa_solid_900_ttf.cpp")
add_custom_command(
    OUTPUT "${_editor_icon_font_source}"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${_editor_icon_font}"
        "-DOUTPUT=${_editor_icon_font_source}"
        -DSYMBOL=kFontAwesomeSolidTtf
        -P "${PROJECT_SOURCE_DIR}/cmake/MiniEngineEmbedFile.cmake"
    DEPENDS "${_editor_icon_font}" "${PROJECT_SOURCE_DIR}/cmake/MiniEngineEmbedFile.cmake"
    COMMENT "Embedding fa-solid-900.ttf"
    VERBATIM
)
target_sources(engine_editor PRIVATE "${_editor_icon_font_source}")
source_group("Generated" FILES "${_editor_icon_font_source}")

target_include_directories(engine_editor
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/IconFontCppHeaders"
)

target_link_libraries(engine_editor
    PUBLIC
        engine_render_core
        engine_logic
        engine_asset
        engine_platform
        engine_scene
        engine_core
        SDL3::SDL3
        glm::glm
        imgui::imgui
        imguizmo::imguizmo
        EnTT::EnTT
    PRIVATE
        yaml-cpp::yaml-cpp
)
