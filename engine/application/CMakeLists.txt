add_library(engine_application
    application.h
    editor_application.cpp
    editor_application.h
)
miniengine_group_target_sources(engine_application)

target_include_directories(engine_application
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
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
