add_library(engine_scene INTERFACE)

target_include_directories(engine_scene
INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(engine_scene
INTERFACE
    glm::glm
    EnTT::EnTT
)
