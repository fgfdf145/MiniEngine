add_library(engine_application
    control_server.cpp
    control_server.h
    control_session.cpp
    control_session.h
    control_ui.cpp
    control_ui.h
    editor_application.cpp
    editor_application.h
)
miniengine_group_target_sources(engine_application)

target_include_directories(engine_application
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(engine_application
    PUBLIC
        engine_core
        engine_platform
        engine_renderer
    PRIVATE
        engine_audio
        engine_logic
        imguizmo::imguizmo
        imgui::imgui
        yaml-cpp::yaml-cpp
        EnTT::EnTT
)

if(WIN32)
    target_link_libraries(engine_application PRIVATE ws2_32)
endif()
