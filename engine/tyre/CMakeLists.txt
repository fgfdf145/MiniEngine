add_library(engine_tyre
    tyre_brush.cpp
    tyre_brush.h
    tyre_magic_formula.cpp
    tyre_magic_formula.h
    tyre_spec.cpp
    tyre_spec.h
    tyre_thermal.cpp
    tyre_thermal.h
    tyre_wear.cpp
    tyre_wear.h
    tyre_tir_file.cpp
    tyre_tir_file.h
)
miniengine_group_target_sources(engine_tyre)

target_include_directories(engine_tyre
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(engine_tyre
    PUBLIC
        glm::glm
)
