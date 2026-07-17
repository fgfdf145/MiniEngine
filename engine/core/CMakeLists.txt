add_library(engine_core
    input/gamepad_backend.cpp
    input/gamepad_backend.h
    input/input.cpp
    input/input.h
    log/log.cpp
    log/log.h
    render_backend_type.h
    uuid/uuid.cpp
    uuid/uuid.h
)
miniengine_group_target_sources(engine_core)

target_include_directories(engine_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(engine_core
    PUBLIC
        SDL3::SDL3
        spdlog::spdlog
)

if(WIN32)
    target_link_libraries(engine_core
        PUBLIC
            Xinput
    )
endif()
