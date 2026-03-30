add_library(engine_core
    input/gamepad_backend.cpp
    input/input.cpp
    log/log.cpp
)

target_include_directories(engine_core
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(engine_core
PUBLIC
    spdlog::spdlog
    SDL3::SDL3
)

if(WIN32)
    target_link_libraries(engine_core
    PUBLIC
        Xinput
    )
endif()
