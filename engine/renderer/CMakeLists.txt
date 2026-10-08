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
    tyre_deformation.cpp
    tyre_deformation.h
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
    path_tracing.cpp
    path_tracing.h
    reference_path_tracer.cpp
    reference_path_tracer.h
    render_features.cpp
    render_features.h
    render_transform_snapshot.cpp
    render_transform_snapshot.h
    render_types.h
    renderer_world.cpp
    renderer_world.h
    scene_capture_view.h
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
    restir_pairing.cpp
    restir_pairing.h
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
    vulkan/path_trace_lights.cpp
    vulkan/path_trace_lights.h
    vulkan/rt_shadow_pass.cpp
    vulkan/rt_shadow_pass.h
    vulkan/path_trace_layer_pass.cpp
    vulkan/path_trace_layer_pass.h
    vulkan/path_trace_pass.cpp
    vulkan/path_trace_pass.h
    vulkan/restir_pt_pass.cpp
    vulkan/restir_pt_pass.h
    vulkan/ddgi_reference_capture.cpp
    vulkan/common.h
    vulkan/device.cpp
    vulkan/device.h
    vulkan/nvrhi_device.cpp
    vulkan/nvrhi_device.h
    vulkan/nvrhi_native.h
    vulkan/nvrhi_pass.cpp
    vulkan/nvrhi_pass.h
    vulkan/nvrhi_resources.cpp
    vulkan/nvrhi_resources.h
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
    vulkan/retire_queue.h
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
    vulkan/scene_view.h
    vulkan/selection_outline_pass.cpp
    vulkan/selection_outline_pass.h
    vulkan/skinning_pass.cpp
    vulkan/skinning_pass.h
    vulkan/toon_pass.cpp
    vulkan/toon_pass.h
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
# Every shader stage the renderer loads is listed here by name: the source is shaders/vulkan/<name>.slang
# and the compiled module <name>.spv, which is what VulkanShaderModule is given at load time.
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
    skin.comp
    tyre_deform.comp
    toon.vert
    toon.frag
    toon_prepass.frag
    imgui_hdr10.frag
    ssr_trace.comp
    ssr_resolve.comp
    rt_shadow_temporal.comp
    rt_shadow_filter.comp
    path_trace_temporal.comp
    path_trace_filter.comp
    transmission_copy.comp
    ray_material_average.comp
    ddgi_debug.comp
    ddgi_trace.comp
    ddgi_update.comp
)

# One shader build: shaders/vulkan/shader_name.slang -> output_name.spv in MINIENGINE_SHADER_OUTPUT_DIR
# (docs/design/2026-10-08-slang-shader-migration-design.md). RAY_QUERY builds target SPIR-V 1.4
# (Vulkan 1.2), which ray queries need. Extra arguments are NAME=value defines. slangc writes a
# depfile, so a shader is rebuilt when a file it includes changes.
function(_miniengine_add_shader_build out_list shader_name output_name ray_query)
    set(defines ${ARGN})
    set(slang_source "${PROJECT_SOURCE_DIR}/shaders/vulkan/${shader_name}.slang")
    set(shader_output "${MINIENGINE_SHADER_OUTPUT_DIR}/${output_name}.spv")
    set(define_flags "")
    if(ray_query)
        list(APPEND defines "RAY_QUERY")
    endif()
    foreach(define IN LISTS defines)
        list(APPEND define_flags "-D${define}")
    endforeach()
    set(description "${output_name}")
    if(NOT "${shader_name}" STREQUAL "${output_name}" OR defines)
        set(description "${output_name} (${shader_name} ${defines})")
    endif()
    if(ray_query)
        set(profile spirv_1_4)
    else()
        set(profile spirv_1_3)
    endif()
    # Slang cannot mark an output invariant; these get what GLSL's `invariant gl_Position` gave them
    # afterwards (tools/spirv_invariant_position).
    set(post_commands "")
    set(post_depends "")
    if("${shader_name}" IN_LIST MINIENGINE_INVARIANT_POSITION_SHADERS)
        set(post_commands COMMAND miniengine_spirv_invariant_position "${shader_output}")
        set(post_depends miniengine_spirv_invariant_position)
    endif()
    # -matrix-layout-column-major: matrices in memory as GLM writes them, and mul(M, v) is GLSL's
    # M * v. Warning 41012 ("profile implicitly upgraded") lists every capability an intrinsic
    # might need (bindless sampling names sparse residency, min LOD, ...); the module declares
    # only those it uses.
    add_custom_command(
        OUTPUT "${shader_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIENGINE_SHADER_OUTPUT_DIR}"
        COMMAND ${MINIENGINE_SLANGC_EXECUTABLE} "${slang_source}" -target spirv -profile ${profile}
                -entry main -matrix-layout-column-major -warnings-disable 41012 ${define_flags}
                -depfile "${shader_output}.d" -o "${shader_output}"
        ${post_commands}
        DEPENDS "${slang_source}" ${post_depends}
        DEPFILE "${shader_output}.d"
        COMMENT "Compiling ${description}"
        VERBATIM
    )
    set(${out_list} ${${out_list}} "${shader_output}" PARENT_SCOPE)
endfunction()

function(miniengine_add_shaders shader_target)
    set(spv_outputs "")
    foreach(shader_name IN LISTS ARGN)
        _miniengine_add_shader_build(spv_outputs "${shader_name}" "${shader_name}" FALSE)
    endforeach()

    # The hardware ray tracing variants (ray_tracing_common.slang): name.comp -> name_ray_query.comp.spv.
    foreach(shader_name IN LISTS MINIENGINE_RAY_QUERY_SHADERS)
        get_filename_component(shader_stem "${shader_name}" NAME_WE)
        get_filename_component(shader_extension "${shader_name}" LAST_EXT)
        _miniengine_add_shader_build(spv_outputs "${shader_name}" "${shader_stem}_ray_query${shader_extension}" TRUE)
    endforeach()

    # Shaders only hardware ray tracing runs (the ray traced effects): one ray query build, named as
    # the source.
    foreach(shader_name IN LISTS MINIENGINE_HARDWARE_RAY_SHADERS)
        _miniengine_add_shader_build(spv_outputs "${shader_name}" "${shader_name}" TRUE)
    endforeach()

    # Shaders compiled again under another name with a define: source|output|NAME=value.
    foreach(variant IN LISTS MINIENGINE_SHADER_DEFINE_VARIANTS)
        string(REPLACE "|" ";" variant_fields "${variant}")
        list(GET variant_fields 0 variant_source)
        list(GET variant_fields 1 variant_name)
        list(GET variant_fields 2 variant_define)
        _miniengine_add_shader_build(spv_outputs "${variant_source}" "${variant_name}" FALSE ${variant_define})
    endforeach()

    # Hardware ray tracing shaders compiled again under another name with defines:
    # source|output|NAME=value,NAME=value.
    foreach(variant IN LISTS MINIENGINE_HARDWARE_RAY_DEFINE_VARIANTS)
        string(REPLACE "|" ";" variant_fields "${variant}")
        list(GET variant_fields 0 variant_source)
        list(GET variant_fields 1 variant_name)
        list(GET variant_fields 2 variant_defines)
        string(REPLACE "," ";" variant_define_list "${variant_defines}")
        _miniengine_add_shader_build(spv_outputs "${variant_source}" "${variant_name}" TRUE ${variant_define_list})
    endforeach()

    add_custom_target(${shader_target} DEPENDS ${spv_outputs})
endfunction()

# Vertex shaders whose position must be invariant: the toon pass draws the characters' opaque
# surfaces again on the depth the geometry pass wrote for them.
set(MINIENGINE_INVARIANT_POSITION_SHADERS
    triangle.vert
    toon.vert
)

# The path traced layer's two passes (path_trace_layer_pass.h): gbuffer.frag writing the nearest
# forward-shaded surface's depth, then its G-buffer.
set(MINIENGINE_SHADER_DEFINE_VARIANTS
    "gbuffer.frag|path_trace_layer_depth.frag|PATH_TRACE_LAYER_PASS=1"
    "gbuffer.frag|path_trace_layer_surface.frag|PATH_TRACE_LAYER_PASS=2"
)

# The path tracer's trace without what a frame does not need (VulkanPathTracePass picks one).
set(MINIENGINE_HARDWARE_RAY_DEFINE_VARIANTS
    "path_trace.comp|path_trace_no_transmission.comp|PT_TRANSMISSION=0"
    "path_trace.comp|path_trace_no_layers.comp|PT_LAYERED=0"
    "path_trace.comp|path_trace_base.comp|PT_TRANSMISSION=0,PT_LAYERED=0"
)

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
    path_trace.comp
    emissive_lights.comp
    light_grid.comp
    restir_pt_initial.comp
    restir_pt_temporal.comp
    restir_pt_spatial_shift.comp
    restir_pt_spatial.comp
    restir_pt_duplication.comp
)

miniengine_add_shaders(engine_renderer_shaders ${MINIENGINE_SHADER_SOURCES})

target_link_libraries(engine_renderer
    PUBLIC
        engine_render_core
        engine_editor
        Vulkan::Vulkan
        ${MINIENGINE_NVRHI_TARGETS}
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
# A static NVRHI leaves vulkan.hpp's dispatcher to the program (nvrhi_device.cpp); a shared one owns it.
if(MINIENGINE_NVRHI_STATIC)
    set_source_files_properties(vulkan/nvrhi_device.cpp
        PROPERTIES
            COMPILE_DEFINITIONS "MINIENGINE_NVRHI_STATIC=1"
            SKIP_PRECOMPILE_HEADERS ON
    )
endif()

add_dependencies(engine_renderer engine_renderer_shaders)
