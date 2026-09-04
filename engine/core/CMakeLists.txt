add_library(engine_core
    input/gamepad_backend.cpp
    input/gamepad_backend.h
    input/input.cpp
    input/input.h
    log/log.cpp
    log/log.h
    paths/engine_paths.cpp
    paths/engine_paths.h
    render_backend_type.h
    uuid/uuid.cpp
    uuid/uuid.h
)
miniengine_group_target_sources(engine_core)

target_include_directories(engine_core
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
)

# Fallbacks for EnginePaths, used only when neither a command-line override nor
# an environment variable supplies the root. Keep these the only path
# definitions in the build.
target_compile_definitions(engine_core
    PRIVATE
        MINIENGINE_DEFAULT_PROJECT_DIR="${PROJECT_SOURCE_DIR}"
        MINIENGINE_DEFAULT_CACHE_DIR="${MINIENGINE_DEFAULT_CACHE_DIR}"
        MINIENGINE_DEFAULT_SHADER_DIR="${MINIENGINE_SHADER_OUTPUT_DIR}"
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
