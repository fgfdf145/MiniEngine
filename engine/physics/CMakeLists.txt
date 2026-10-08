add_library(engine_physics
    collision_filter.cpp
    collision_filter.h
    physics_world.cpp
    physics_world.h
    task_job_system.cpp
    task_job_system.h
    vehicle_settings.cpp
    vehicle_settings.h
    vehicle_suspension.cpp
    vehicle_suspension.h
    vehicle_wheel_motion.cpp
    vehicle_wheel_motion.h
    water_surface.cpp
    water_surface.h
)
miniengine_group_target_sources(engine_physics)

target_include_directories(engine_physics
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
)

# Jolt stays private: its headers and compile definitions (SIMD level, asserts, debug renderer)
# only reach physics_world.cpp and task_job_system.cpp, and the public headers speak glm.
target_link_libraries(engine_physics
    PUBLIC
        glm::glm
        engine_suspension
        engine_tyre
    PRIVATE
        engine_core
        Jolt::Jolt
)
# Jolt is built without RTTI, its default. GCC and Clang make a derived class's type_info name its
# base's (TaskJobSystem derives from Jolt's job system), which such a Jolt does not define; MSVC
# emits what it needs where it is used. Nothing here uses RTTI.
if(NOT MSVC)
    target_compile_options(engine_physics PRIVATE -fno-rtti)
endif()
