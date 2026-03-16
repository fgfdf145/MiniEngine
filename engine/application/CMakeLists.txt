add_library(engine_application
    editor_application.cpp
)

target_include_directories(engine_application
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(engine_application
PUBLIC
    engine_core
    engine_platform
    engine_renderer
PRIVATE
    engine_logic
    imguizmo::imguizmo
    yaml-cpp::yaml-cpp
    EnTT::EnTT
)
