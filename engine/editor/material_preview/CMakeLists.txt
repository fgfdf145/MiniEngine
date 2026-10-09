# ----------------------------------------------------------------------------
# engine_material_preview: the Material Editor's preview renderer
# (docs/design/2026-10-09-material-editor-redesign-design.md), a progressive CPU ray tracer that
# shades with the engine's materials and BRDF. It traces every pixel of the preview each pass, which
# unoptimised under MSVC's Debug flags is tens of times slower, so like engine_render_bvh
# (engine/renderer/optimized) it builds in a directory of its own whose Debug flags are optimised.
# The runtime library and iterator debugging stay the configuration's, so it links against the rest
# of a Debug build; the hot loops use raw pointers, not checked iterators.
# ----------------------------------------------------------------------------
if(MSVC)
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REGEX REPLACE "/O[bd][0-9]?" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " /O2 /Ob2")
else()
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " -O2")
endif()

add_library(engine_material_preview STATIC
    material_preview_environment.cpp
    material_preview_environment.h
    material_preview_geometry.cpp
    material_preview_geometry.h
    material_preview_renderer.cpp
    material_preview_renderer.h
    material_preview_shading.cpp
    material_preview_shading.h
    material_preview_shapes.cpp
    material_preview_shapes.h
    material_preview_textures.cpp
    material_preview_textures.h
)
miniengine_group_target_sources(engine_material_preview)

target_include_directories(engine_material_preview
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(engine_material_preview
    PUBLIC
        engine_render_core
        engine_asset
        engine_core
        glm::glm
)
