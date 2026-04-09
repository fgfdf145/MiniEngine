set(_miniengine_renderer_sources
    camera.cpp
    editor_backend_base.cpp
    editor_ui.cpp
    engine_settings.cpp
    gltf_model_loader.cpp
    imgui/imgui_impl_sdl3.cpp
    mesh.cpp
    material_graph_runtime.cpp
    model_loader.cpp
    model_post_process.cpp
    renderer_world.cpp
    rhi/factory.cpp
    texture_loader.cpp
)

if(TARGET Vulkan::Vulkan)
    list(APPEND _miniengine_renderer_sources
        imgui/imgui_impl_vulkan.cpp
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
    )
endif()

if(APPLE)
    list(APPEND _miniengine_renderer_sources
        imgui/imgui_impl_metal.mm
        metal/renderer.mm
    )
endif()

add_library(engine_renderer
    ${_miniengine_renderer_sources}
)

if(TARGET Vulkan::Vulkan)
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
endif()

target_include_directories(engine_renderer
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
PRIVATE
    ${Stb_INCLUDE_DIR}
    ${MINIENGINE_TINYGLTF_INCLUDE_DIR}
)

target_compile_definitions(engine_renderer
PRIVATE
    MINIENGINE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
    MINIENGINE_PROJECT_DIR="${CMAKE_SOURCE_DIR}"
)

if(TARGET Vulkan::Vulkan)
    target_compile_definitions(engine_renderer
    PRIVATE
        MINIENGINE_HAS_VULKAN_BACKEND=1
        MINIENGINE_SHADER_DIR="${MINIENGINE_SHADER_OUTPUT_DIR}"
    )
endif()

target_link_libraries(engine_renderer
PUBLIC
    engine_core
    engine_platform
    engine_scene
    SDL3::SDL3
    glm::glm
    imgui::imgui
    EnTT::EnTT
PRIVATE
    engine_logic
    imguizmo::imguizmo
    yaml-cpp::yaml-cpp
)

if(TARGET Vulkan::Vulkan)
    target_link_libraries(engine_renderer
    PUBLIC
        Vulkan::Vulkan
    )
endif()

if(APPLE)
    set_source_files_properties(
        imgui/imgui_impl_metal.mm
        metal/renderer.mm
        PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc"
    )
endif()

if(TARGET engine_renderer_shaders)
    add_dependencies(engine_renderer engine_renderer_shaders)
endif()
