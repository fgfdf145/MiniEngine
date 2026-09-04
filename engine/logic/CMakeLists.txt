add_library(engine_logic
    editor_scene.cpp
    editor_scene.h
    editor_world.h
    gizmo_settings.cpp
    gizmo_settings.h
    logic_layer.h
)
miniengine_group_target_sources(engine_logic)

target_include_directories(engine_logic
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(engine_logic
    PUBLIC
        engine_core
        engine_scene
        glm::glm
        EnTT::EnTT
    PRIVATE
        imguizmo::imguizmo
        yaml-cpp::yaml-cpp
)
