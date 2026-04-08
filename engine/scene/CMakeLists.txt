add_library(engine_scene
    material_graph.h
    scene_components.h
    scene_module.cpp
    scene_world.h
    world_units.h
)

target_include_directories(engine_scene
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(engine_scene
PUBLIC
    glm::glm
    EnTT::EnTT
)
