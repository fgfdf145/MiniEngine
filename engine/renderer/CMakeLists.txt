# ----------------------------------------------------------------------------
# engine_render_core: backend-agnostic render data types shared by the editor
# and the render backends (camera, CPU render world, RHI interface headers).
# Keep this target free of ImGui editor code and Vulkan.
# ----------------------------------------------------------------------------
add_library(engine_render_core
    camera.cpp
    renderer_world.cpp
)

target_include_directories(engine_render_core
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
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
    imgui/imgui_impl_vulkan.cpp
    rhi/factory.cpp
    vulkan/buffer.cpp
    vulkan/command.cpp
    vulkan/device.cpp
    vulkan/imgui_layer.cpp
    vulkan/instance.cpp
    vulkan/pipeline.cpp
    vulkan/render_pass.cpp
    vulkan/renderer.cpp
    vulkan/scene_viewport.cpp
    vulkan/swapchain.cpp
    vulkan/texture.cpp
    vulkan/uniform_buffer.cpp
    vulkan/upload_batch.cpp
)

set(MINIENGINE_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
set(MINIENGINE_VERTEX_SHADER "${CMAKE_SOURCE_DIR}/shaders/vulkan/triangle.vert")
set(MINIENGINE_FRAGMENT_SHADER "${CMAKE_SOURCE_DIR}/shaders/vulkan/triangle.frag")
set(MINIENGINE_VERTEX_SPV "${MINIENGINE_SHADER_OUTPUT_DIR}/triangle.vert.spv")
set(MINIENGINE_FRAGMENT_SPV "${MINIENGINE_SHADER_OUTPUT_DIR}/triangle.frag.spv")

add_custom_command(
    OUTPUT ${MINIENGINE_VERTEX_SPV}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIENGINE_SHADER_OUTPUT_DIR}"
    COMMAND ${Vulkan_GLSLC_EXECUTABLE} "${MINIENGINE_VERTEX_SHADER}" -o "${MINIENGINE_VERTEX_SPV}"
    DEPENDS "${MINIENGINE_VERTEX_SHADER}"
    VERBATIM
)

add_custom_command(
    OUTPUT ${MINIENGINE_FRAGMENT_SPV}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIENGINE_SHADER_OUTPUT_DIR}"
    COMMAND ${Vulkan_GLSLC_EXECUTABLE} "${MINIENGINE_FRAGMENT_SHADER}" -o "${MINIENGINE_FRAGMENT_SPV}"
    DEPENDS "${MINIENGINE_FRAGMENT_SHADER}"
    VERBATIM
)

add_custom_target(engine_renderer_shaders
    DEPENDS
        ${MINIENGINE_VERTEX_SPV}
        ${MINIENGINE_FRAGMENT_SPV}
)

target_compile_definitions(engine_renderer
PRIVATE
    MINIENGINE_PROJECT_DIR="${CMAKE_SOURCE_DIR}"
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

if(TARGET engine_renderer_shaders)
    add_dependencies(engine_renderer engine_renderer_shaders)
endif()
