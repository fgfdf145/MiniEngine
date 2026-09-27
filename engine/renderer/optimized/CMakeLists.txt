# ----------------------------------------------------------------------------
# engine_render_bvh: the ray scene's hierarchy build (ray_tracing_bvh.cpp), optimised in every
# configuration. It walks every triangle of new content on a worker (New Sponza with its curtains and
# ivy has 10.8 million), which unoptimised under MSVC's Debug flags took 94 s against 3.4 s in
# Release, and DDGI waits for it. MSVC refuses /O2 together with Debug's /RTC1, and a source file
# cannot drop a flag its directory set, so the file builds here, in a directory of its own whose
# Debug flags are optimised. The runtime library and iterator debugging stay the configuration's, so
# it links against the rest of a Debug build.
# ----------------------------------------------------------------------------
if(MSVC)
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REGEX REPLACE "/O[bd][0-9]?" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " /O2 /Ob2")
else()
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " -O2")
endif()

set(_engine_render_bvh_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/../ray_tracing_bvh.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ray_tracing_bvh.h"
)
add_library(engine_render_bvh STATIC ${_engine_render_bvh_sources})
# The sources live one level up, beside the rest of the renderer.
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/.." FILES ${_engine_render_bvh_sources})

target_include_directories(engine_render_bvh
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(engine_render_bvh
    PUBLIC
        glm::glm
)
