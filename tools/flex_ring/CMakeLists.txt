# The flexible ring tyre's command line rigs (engine/tyre/flex_ring): pre-processing, modes, statics,
# steady-state sweeps, cleat runs and the benchmark, written as CSV.
add_executable(miniengine_flex_ring
    flex_ring_cli.cpp
)
miniengine_group_target_sources(miniengine_flex_ring)

target_link_libraries(miniengine_flex_ring
    PRIVATE
        engine_tyre
)

set_target_properties(miniengine_flex_ring PROPERTIES FOLDER "Tools")
