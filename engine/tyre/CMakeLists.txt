add_library(engine_tyre
    flex_ring/flex_ring_banded.cpp
    flex_ring/flex_ring_banded.h
    flex_ring/flex_ring_data.cpp
    flex_ring/flex_ring_data.h
    flex_ring/flex_ring_modal.cpp
    flex_ring/flex_ring_modal.h
    flex_ring/flex_ring_parameters.h
    flex_ring/flex_ring_preprocess.cpp
    flex_ring/flex_ring_preprocess.h
    flex_ring/flex_ring_report.cpp
    flex_ring/flex_ring_report.h
    flex_ring/flex_ring_rig.cpp
    flex_ring/flex_ring_rig.h
    flex_ring/flex_ring_road.cpp
    flex_ring/flex_ring_road.h
    flex_ring/flex_ring_tyre.cpp
    flex_ring/flex_ring_tyre.h
    tyre_brush.cpp
    tyre_brush.h
    tyre_spec.cpp
    tyre_spec.h
    tyre_thermal.cpp
    tyre_thermal.h
    tyre_wear.cpp
    tyre_wear.h
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

# The flexible ring tyre's C interface as a shared library, for external programs (Python ctypes,
# MATLAB, ...): engine/tyre/flex_ring/flex_ring_c_api.h.
add_library(miniengine_flex_ring_c SHARED
    flex_ring/flex_ring_c_api.cpp
    flex_ring/flex_ring_c_api.h
)
miniengine_group_target_sources(miniengine_flex_ring_c)
target_compile_definitions(miniengine_flex_ring_c PRIVATE MINIENGINE_FLEX_RING_C_EXPORTS)
target_link_libraries(miniengine_flex_ring_c PRIVATE engine_tyre)
set_target_properties(miniengine_flex_ring_c PROPERTIES FOLDER "Engine")
