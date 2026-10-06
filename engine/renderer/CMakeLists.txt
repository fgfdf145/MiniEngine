# ----------------------------------------------------------------------------
# engine_render_core: backend-agnostic render data types shared by the editor
# and the render backends (camera, CPU render world, RHI interface headers).
# Keep this target free of ImGui editor code and Vulkan.
# ----------------------------------------------------------------------------
add_library(engine_render_core
    atmosphere.cpp
    atmosphere.h
    bloom_chain.cpp
    bloom_chain.h
    block_suballocator.cpp
    block_suballocator.h
    glare.cpp
    glare.h
    height_fog.cpp
    height_fog.h
    volumetric_clouds.cpp
    volumetric_clouds.h
    camera.cpp
    camera.h
    cpu_stage_timer.cpp
    cpu_stage_timer.h
    ddgi_volume.cpp
    ddgi_volume.h
    environment_brdf.cpp
    environment_brdf.h
    exposure.cpp
    exposure.h
    light_clusters.cpp
    light_clusters.h
    local_shadows.cpp
    ltc_table.cpp
    ltc_table.h
    local_shadows.h
    material.h
    material_pipeline.cpp
    material_pipeline.h
    motion_history.cpp
    motion_history.h
    reference_path_tracer.cpp
    reference_path_tracer.h
    render_transform_snapshot.cpp
    render_transform_snapshot.h
    render_types.h
    renderer_world.cpp
    renderer_world.h
    scene_lighting.cpp
    scene_lighting.h
    shadow_cascades.cpp
    shadow_cascades.h
    specular_aa.cpp
    specular_aa.h
    spherical_harmonics.cpp
    spherical_harmonics.h
    taa_jitter.cpp
    taa_jitter.h
    temporal_history.cpp
    temporal_history.h
    view_frustum.cpp
    view_frustum.h
    white_balance.cpp
    white_balance.h
)
miniengine_group_target_sources(engine_render_core)

# The ray scene's hierarchy build, optimised in every configuration (see optimized/CMakeLists.txt).
add_subdirectory(optimized)

target_include_directories(engine_render_core
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(engine_render_core
    PUBLIC
        engine_core
        engine_scene
        engine_asset
        engine_render_bvh
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
    rhi/backend.h
    rhi/factory.cpp
    rhi/factory.h
    vulkan/ao_pass.cpp
    vulkan/ao_pass.h
    vulkan/gi_pass.cpp
    vulkan/gi_pass.h
    vulkan/gpu_timer.cpp
    vulkan/gpu_timer.h
    vulkan/bloom_pass.cpp
    vulkan/bloom_pass.h
    vulkan/atmosphere.cpp
    vulkan/atmosphere.h
    vulkan/buffer.cpp
    vulkan/buffer.h
    vulkan/descriptor_pool_list.cpp
    vulkan/descriptor_pool_list.h
    vulkan/material_set_cache.cpp
    vulkan/material_set_cache.h
    vulkan/memory_pool.cpp
    vulkan/memory_pool.h
    vulkan/command.cpp
    vulkan/command.h
    vulkan/compute_pass_util.cpp
    vulkan/compute_pass_util.h
    vulkan/ddgi.cpp
    vulkan/ddgi.h
    vulkan/ddgi_debug_pass.cpp
    vulkan/ddgi_debug_pass.h
    vulkan/rt_shadow_pass.cpp
    vulkan/rt_shadow_pass.h
    vulkan/ddgi_reference_capture.cpp
    vulkan/common.h
    vulkan/device.cpp
    vulkan/device.h
    vulkan/dlss.cpp
    vulkan/dlss.h
    vulkan/environment_probe.cpp
    vulkan/environment_probe.h
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
    vulkan/parallel_recorder.cpp
    vulkan/parallel_recorder.h
    vulkan/pipeline.cpp
    vulkan/pipeline.h
    vulkan/pipeline_set.cpp
    vulkan/pipeline_set.h
    vulkan/reverse_depth.h
    vulkan/ray_acceleration.cpp
    vulkan/ray_acceleration.h
    vulkan/ray_scene.cpp
    vulkan/ray_scene.h
    vulkan/render_pass.cpp
    vulkan/render_pass.h
    vulkan/render_target_layout.cpp
    vulkan/render_target_layout.h
    vulkan/renderer.cpp
    vulkan/renderer.h
    vulkan/scene_pass.h
    vulkan/scatter_pass.cpp
    vulkan/scatter_pass.h
    vulkan/scene_pass_order.cpp
    vulkan/scene_pass_order.h
    vulkan/sampler_cache.cpp
    vulkan/sampler_settings.cpp
    vulkan/sampler_settings.h
    vulkan/scene_render_targets.cpp
    vulkan/scene_render_targets.h
    vulkan/selection_outline_pass.cpp
    vulkan/selection_outline_pass.h
    vulkan/local_shadow_pass.cpp
    vulkan/local_shadow_pass.h
    vulkan/shadow_pass.cpp
    vulkan/shadow_pass.h
    vulkan/swapchain.cpp
    vulkan/swapchain.h
    vulkan/ssr_pass.cpp
    vulkan/ssr_pass.h
    vulkan/taa_pass.cpp
    vulkan/taa_pass.h
    vulkan/texture.cpp
    vulkan/texture.h
    vulkan/tonemap_pass.cpp
    vulkan/transmission_copy.cpp
    vulkan/transmission_copy.h
    vulkan/tonemap_pass.h
    vulkan/uniform_buffer.cpp
    vulkan/uniform_buffer.h
    vulkan/upload_batch.cpp
    vulkan/upload_batch.h
    vulkan/video_readback.cpp
    vulkan/video_readback.h
    vulkan/viewport_capture.cpp
    vulkan/viewport_capture.h
)
miniengine_group_target_sources(engine_renderer)

# The ImGui SDL3/Vulkan backends are a vendored, locally patched copy (third_party/imgui_backends).
# They live outside this directory, so they are added after the source tree grouping above.
set(_imgui_backend_sources
    "${PROJECT_SOURCE_DIR}/third_party/imgui_backends/imgui_impl_sdl3.cpp"
    "${PROJECT_SOURCE_DIR}/third_party/imgui_backends/imgui_impl_sdl3.h"
    "${PROJECT_SOURCE_DIR}/third_party/imgui_backends/imgui_impl_vulkan.cpp"
    "${PROJECT_SOURCE_DIR}/third_party/imgui_backends/imgui_impl_vulkan.h"
)
target_sources(engine_renderer PRIVATE ${_imgui_backend_sources})
source_group(TREE "${PROJECT_SOURCE_DIR}/third_party" PREFIX "third_party" FILES ${_imgui_backend_sources})

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
    gi_trace.comp
    gi_resolve.comp
    gi_composite.frag
    taa_resolve.comp
    dlss_motion_vectors.comp
    dlss_rr_guides.comp
    bloom.comp
    atmosphere_transmittance.comp
    atmosphere_multiscattering.comp
    atmosphere_skyview.comp
    atmosphere_aerial_perspective.comp
    atmosphere_irradiance.comp
    cloud_march.comp
    cloud_noise.comp
    cloud_resolve.comp
    cloud_shadow.comp
    cloud_weather.comp
    environment_capture.comp
    environment_prefilter.comp
    sky.vert
    sky.frag
    ground.frag
    shadow.vert
    shadow_depth.vert
    shadow.frag
    selection_outline.frag
    imgui_hdr10.frag
    ssr_trace.comp
    ssr_resolve.comp
    rt_shadow_temporal.comp
    rt_shadow_filter.comp
    transmission_copy.comp
    ray_material_average.comp
    ddgi_debug.comp
    ddgi_trace.comp
    ddgi_update.comp
)

# Files other shaders pull in with #include. Every shader is rebuilt when any of them changes,
# which is coarser than a per-shader depfile but cannot miss a dependency.
set(MINIENGINE_SHADER_INCLUDES
    exposure_histogram.glsl
    gt7_tonemap.glsl
    hdr_output.glsl
    ssr_common.glsl
    ssr_half_res.glsl
    local_shadow_common.glsl
    anisotropy_common.glsl
    brdf_common.glsl
    ltc_common.glsl
    iridescence_common.glsl
    transmission_common.glsl
    volume_scatter_common.glsl
    pbr_neutral.glsl
    material_layers.glsl
    detail_layers.glsl
    material_uv.glsl
    pre_exposure.glsl
    reverse_depth.glsl
    scene_common.glsl
    pbr_common.glsl
    material_common.glsl
    specular_aa.glsl
    gbuffer_common.glsl
    gbuffer_inputs.glsl
    normal_map.glsl
    vbao_common.glsl
    atmosphere_common.glsl
    atmosphere_integrate.glsl
    atmosphere_sampling.glsl
    height_fog.glsl
    volumetric_clouds.glsl
    cloud_reconstruction.glsl
    cloud_shadow.glsl
    spherical_harmonics.glsl
    cubemap_common.glsl
    ray_tracing_common.glsl
    ray_hit_common.glsl
    rt_shadow_common.glsl
    ddgi_common.glsl
    ground_plane.glsl
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
    # The hardware ray tracing variants (ray_tracing_common.glsl): name.comp -> name_ray_query.comp.spv,
    # ray queries need SPIR-V 1.4, so Vulkan 1.2.
    foreach(shader_name IN LISTS MINIENGINE_RAY_QUERY_SHADERS)
        set(shader_source "${PROJECT_SOURCE_DIR}/shaders/vulkan/${shader_name}")
        get_filename_component(shader_stem "${shader_name}" NAME_WE)
        get_filename_component(shader_extension "${shader_name}" LAST_EXT)
        set(shader_output "${MINIENGINE_SHADER_OUTPUT_DIR}/${shader_stem}_ray_query${shader_extension}.spv")
        add_custom_command(
            OUTPUT "${shader_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIENGINE_SHADER_OUTPUT_DIR}"
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.2 -DRAY_QUERY "${shader_source}" -o "${shader_output}"
            DEPENDS "${shader_source}" ${shader_includes}
            COMMENT "Compiling ${shader_name} (ray query)"
            VERBATIM
        )
        list(APPEND spv_outputs "${shader_output}")
    endforeach()

    # Shaders only hardware ray tracing runs (the ray traced effects): one ray query build, named as
    # the source.
    foreach(shader_name IN LISTS MINIENGINE_HARDWARE_RAY_SHADERS)
        set(shader_source "${PROJECT_SOURCE_DIR}/shaders/vulkan/${shader_name}")
        set(shader_output "${MINIENGINE_SHADER_OUTPUT_DIR}/${shader_name}.spv")
        add_custom_command(
            OUTPUT "${shader_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIENGINE_SHADER_OUTPUT_DIR}"
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.2 -DRAY_QUERY "${shader_source}" -o "${shader_output}"
            DEPENDS "${shader_source}" ${shader_includes}
            COMMENT "Compiling ${shader_name} (hardware ray tracing)"
            VERBATIM
        )
        list(APPEND spv_outputs "${shader_output}")
    endforeach()

    add_custom_target(${shader_target} DEPENDS ${spv_outputs})
endfunction()

# Shaders that trace the ray scene, compiled a second time for hardware ray tracing.
set(MINIENGINE_RAY_QUERY_SHADERS
    ddgi_trace.comp
    ddgi_debug.comp
    deferred_lighting.frag
)

# The ray traced effects, which exist only with hardware ray tracing.
set(MINIENGINE_HARDWARE_RAY_SHADERS
    rt_occlusion.comp
    rt_shadow_trace.comp
    rt_reflection_trace.comp
)

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
if(MINIENGINE_DLSS_FOUND)
    target_link_libraries(engine_renderer PRIVATE miniengine_ngx)
endif()
target_include_directories(engine_renderer PRIVATE "${Stb_INCLUDE_DIR}")

add_dependencies(engine_renderer engine_renderer_shaders)
