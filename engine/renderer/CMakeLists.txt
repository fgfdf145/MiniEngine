# ----------------------------------------------------------------------------
# engine_render_core: backend-agnostic render data types shared by the editor
# and the render backends (camera, CPU render world, RHI interface headers).
# Keep this target free of ImGui editor code and Vulkan.
# ----------------------------------------------------------------------------
add_library(engine_render_core
    ao_history.cpp
    ao_history.h
    atmosphere.cpp
    atmosphere.h
    camera.cpp
    camera.h
    exposure.cpp
    exposure.h
    material.h
    material_pipeline.cpp
    material_pipeline.h
    motion_history.cpp
    motion_history.h
    render_types.h
    renderer_world.cpp
    renderer_world.h
    scene_lighting.cpp
    scene_lighting.h
    shadow_cascades.cpp
    shadow_cascades.h
    spherical_harmonics.cpp
    spherical_harmonics.h
)
miniengine_group_target_sources(engine_render_core)

target_include_directories(engine_render_core
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(engine_render_core
    PUBLIC
        engine_core
        engine_scene
        engine_asset
        SDL3::SDL3
        glm::glm
        EnTT::EnTT
)

# ----------------------------------------------------------------------------
# engine_renderer: render backend implementations (Vulkan) plus the backend
# factory. Depends on engine_editor because the editor frame loop is currently
# driven from EditorRenderBackendBase, which backends derive from.
# ----------------------------------------------------------------------------
add_library(engine_renderer
    imgui/imgui_impl_sdl3.cpp
    imgui/imgui_impl_sdl3.h
    imgui/imgui_impl_vulkan.cpp
    imgui/imgui_impl_vulkan.h
    rhi/backend.h
    rhi/factory.cpp
    rhi/factory.h
    vulkan/ao_pass.cpp
    vulkan/ao_pass.h
    vulkan/atmosphere.cpp
    vulkan/atmosphere.h
    vulkan/buffer.cpp
    vulkan/buffer.h
    vulkan/command.cpp
    vulkan/command.h
    vulkan/common.h
    vulkan/device.cpp
    vulkan/device.h
    vulkan/exposure_histogram_pass.cpp
    vulkan/exposure_histogram_pass.h
    vulkan/format_support.cpp
    vulkan/format_support.h
    vulkan/forward_pass.cpp
    vulkan/forward_pass.h
    vulkan/geometry_pass.cpp
    vulkan/geometry_pass.h
    vulkan/gbuffer_inputs.cpp
    vulkan/gbuffer_inputs.h
    vulkan/imgui_layer.cpp
    vulkan/imgui_layer.h
    vulkan/instance.cpp
    vulkan/instance.h
    vulkan/lighting_pass.cpp
    vulkan/lighting_pass.h
    vulkan/material_draw.cpp
    vulkan/material_draw.h
    vulkan/pipeline.cpp
    vulkan/pipeline.h
    vulkan/pipeline_set.cpp
    vulkan/pipeline_set.h
    vulkan/render_pass.cpp
    vulkan/render_pass.h
    vulkan/render_target_layout.cpp
    vulkan/render_target_layout.h
    vulkan/renderer.cpp
    vulkan/renderer.h
    vulkan/scene_pass.h
    vulkan/scene_pass_order.cpp
    vulkan/scene_pass_order.h
    vulkan/scene_render_targets.cpp
    vulkan/scene_render_targets.h
    vulkan/shadow_pass.cpp
    vulkan/shadow_pass.h
    vulkan/swapchain.cpp
    vulkan/swapchain.h
    vulkan/texture.cpp
    vulkan/texture.h
    vulkan/tonemap_pass.cpp
    vulkan/tonemap_pass.h
    vulkan/uniform_buffer.cpp
    vulkan/uniform_buffer.h
    vulkan/upload_batch.cpp
    vulkan/upload_batch.h
    vulkan/viewport_capture.cpp
    vulkan/viewport_capture.h
)
miniengine_group_target_sources(engine_renderer)

# MINIENGINE_SHADER_OUTPUT_DIR is set in MiniEngineBuildOptions.cmake so that
# engine_core can bake the same value in as the EnginePaths fallback.
#
# Every shader stage the renderer loads is listed here by file name. The compiled name is the
# source name plus ".spv", which is what VulkanShaderModule is given at load time.
set(MINIENGINE_SHADER_SOURCES
    triangle.vert
    triangle.frag
    gbuffer.frag
    deferred_lighting.frag
    fullscreen.vert
    tonemap.frag
    exposure_histogram.comp
    vbao_trace.comp
    vbao_resolve.comp
    atmosphere_transmittance.comp
    atmosphere_multiscattering.comp
    atmosphere_skyview.comp
    atmosphere_aerial_perspective.comp
    sky.vert
    sky.frag
    shadow.vert
    shadow.frag
)

# Files other shaders pull in with #include. Every shader is rebuilt when any of them changes,
# which is coarser than a per-shader depfile but cannot miss a dependency.
set(MINIENGINE_SHADER_INCLUDES
    exposure_histogram.glsl
    gt7_tonemap.glsl
    scene_common.glsl
    pbr_common.glsl
    gbuffer_common.glsl
    gbuffer_inputs.glsl
    normal_map.glsl
    vbao_common.glsl
    atmosphere_common.glsl
    atmosphere_integrate.glsl
    atmosphere_sampling.glsl
)

function(miniengine_add_shaders shader_target)
    set(spv_outputs "")
    set(shader_includes "")
    foreach(include_name IN LISTS MINIENGINE_SHADER_INCLUDES)
        list(APPEND shader_includes "${PROJECT_SOURCE_DIR}/shaders/vulkan/${include_name}")
    endforeach()
    foreach(shader_name IN LISTS ARGN)
        set(shader_source "${PROJECT_SOURCE_DIR}/shaders/vulkan/${shader_name}")
        set(shader_output "${MINIENGINE_SHADER_OUTPUT_DIR}/${shader_name}.spv")
        add_custom_command(
            OUTPUT "${shader_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIENGINE_SHADER_OUTPUT_DIR}"
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} "${shader_source}" -o "${shader_output}"
            DEPENDS "${shader_source}" ${shader_includes}
            COMMENT "Compiling ${shader_name}"
            VERBATIM
        )
        list(APPEND spv_outputs "${shader_output}")
    endforeach()

    add_custom_target(${shader_target} DEPENDS ${spv_outputs})
endfunction()

miniengine_add_shaders(engine_renderer_shaders ${MINIENGINE_SHADER_SOURCES})

target_link_libraries(engine_renderer
    PUBLIC
        engine_render_core
        engine_editor
        Vulkan::Vulkan
        SDL3::SDL3
        glm::glm
        imgui::imgui
        EnTT::EnTT
    PRIVATE
        engine_logic
)
target_include_directories(engine_renderer PRIVATE "${Stb_INCLUDE_DIR}")

add_dependencies(engine_renderer engine_renderer_shaders)
