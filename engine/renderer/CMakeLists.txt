# ----------------------------------------------------------------------------
# engine_render_core: backend-agnostic render data types shared by the editor
# and the render backends (camera, CPU render world, RHI interface headers).
# Keep this target free of ImGui editor code and Vulkan.
# ----------------------------------------------------------------------------
add_library(engine_render_core
    camera.cpp
    camera.h
    material.h
    material_pipeline.cpp
    material_pipeline.h
    render_types.h
    renderer_world.cpp
    renderer_world.h
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
    vulkan/buffer.cpp
    vulkan/buffer.h
    vulkan/command.cpp
    vulkan/command.h
    vulkan/common.h
    vulkan/device.cpp
    vulkan/device.h
    vulkan/format_support.cpp
    vulkan/format_support.h
    vulkan/forward_pass.cpp
    vulkan/forward_pass.h
    vulkan/imgui_layer.cpp
    vulkan/imgui_layer.h
    vulkan/instance.cpp
    vulkan/instance.h
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
    vulkan/scene_render_targets.cpp
    vulkan/scene_render_targets.h
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
    fullscreen.vert
    tonemap.frag
)

function(miniengine_add_shaders shader_target)
    set(spv_outputs "")
    foreach(shader_name IN LISTS ARGN)
        set(shader_source "${PROJECT_SOURCE_DIR}/shaders/vulkan/${shader_name}")
        set(shader_output "${MINIENGINE_SHADER_OUTPUT_DIR}/${shader_name}.spv")
        add_custom_command(
            OUTPUT "${shader_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIENGINE_SHADER_OUTPUT_DIR}"
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} "${shader_source}" -o "${shader_output}"
            DEPENDS "${shader_source}"
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

add_dependencies(engine_renderer engine_renderer_shaders)
