add_library(engine_logic
    editor_scene.cpp
)

target_include_directories(engine_logic
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
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
