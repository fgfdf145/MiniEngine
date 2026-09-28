add_library(engine_physics
    physics_world.cpp
    physics_world.h
    vehicle_settings.cpp
    vehicle_settings.h
)
miniengine_group_target_sources(engine_physics)

target_include_directories(engine_physics
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
)

# Jolt stays private: its headers and compile definitions (SIMD level, asserts, debug renderer)
# only reach physics_world.cpp, and the public headers speak glm.
target_link_libraries(engine_physics
    PUBLIC
        glm::glm
    PRIVATE
        Jolt::Jolt
)
