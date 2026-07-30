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
        "${CMAKE_CURRENT_SOURCE_DIR}"
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
    vulkan/imgui_layer.cpp
    vulkan/imgui_layer.h
    vulkan/instance.cpp
    vulkan/instance.h
    vulkan/pipeline.cpp
    vulkan/pipeline.h
    vulkan/render_pass.cpp
    vulkan/render_pass.h
    vulkan/renderer.cpp
    vulkan/renderer.h
    vulkan/scene_viewport.cpp
    vulkan/scene_viewport.h
    vulkan/swapchain.cpp
    vulkan/swapchain.h
    vulkan/texture.cpp
    vulkan/texture.h
    vulkan/uniform_buffer.cpp
    vulkan/uniform_buffer.h
    vulkan/upload_batch.cpp
    vulkan/upload_batch.h
)
miniengine_group_target_sources(engine_renderer)

set(MINIENGINE_SHADER_OUTPUT_DIR "${PROJECT_BINARY_DIR}/shaders")
set(MINIENGINE_VERTEX_SHADER "${PROJECT_SOURCE_DIR}/shaders/vulkan/triangle.vert")
set(MINIENGINE_FRAGMENT_SHADER "${PROJECT_SOURCE_DIR}/shaders/vulkan/triangle.frag")
set(MINIENGINE_VERTEX_SPV "${MINIENGINE_SHADER_OUTPUT_DIR}/triangle.vert.spv")
set(MINIENGINE_FRAGMENT_SPV "${MINIENGINE_SHADER_OUTPUT_DIR}/triangle.frag.spv")

function(_miniengine_compile_shader output_file source_file)
    add_custom_command(
        OUTPUT "${output_file}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIENGINE_SHADER_OUTPUT_DIR}"
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} "${source_file}" -o "${output_file}"
        DEPENDS "${source_file}"
        VERBATIM
    )
endfunction()

_miniengine_compile_shader("${MINIENGINE_VERTEX_SPV}" "${MINIENGINE_VERTEX_SHADER}")
_miniengine_compile_shader("${MINIENGINE_FRAGMENT_SPV}" "${MINIENGINE_FRAGMENT_SHADER}")

add_custom_target(engine_renderer_shaders
    DEPENDS
        ${MINIENGINE_VERTEX_SPV}
        ${MINIENGINE_FRAGMENT_SPV}
)

target_compile_definitions(engine_renderer
    PRIVATE
        MINIENGINE_PROJECT_DIR="${PROJECT_SOURCE_DIR}"
        MINIENGINE_SHADER_DIR="${MINIENGINE_SHADER_OUTPUT_DIR}"
)

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
