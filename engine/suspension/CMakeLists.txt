add_library(engine_suspension
    suspension_corner.cpp
    suspension_corner.h
    suspension_curves.cpp
    suspension_curves.h
    suspension_friction.cpp
    suspension_friction.h
    suspension_kinematics.cpp
    suspension_kinematics.h
    suspension_math.cpp
    suspension_math.h
    suspension_model.cpp
    suspension_model.h
    suspension_statics.cpp
    suspension_statics.h
    suspension_strut.cpp
    suspension_strut.h
)
miniengine_group_target_sources(engine_suspension)

target_include_directories(engine_suspension
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
)

# Pure math in double precision: no physics engine behind it.
target_link_libraries(engine_suspension
    PUBLIC
        glm::glm
)
