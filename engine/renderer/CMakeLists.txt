add_library(engine_renderer
    vulkan/buffer.cpp
    vulkan/command.cpp
    vulkan/device.cpp
    vulkan/instance.cpp
    vulkan/pipeline.cpp
    vulkan/render_pass.cpp
    vulkan/renderer.cpp
    vulkan/swapchain.cpp
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

target_include_directories(engine_renderer
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(engine_renderer
PRIVATE
    MINIENGINE_SHADER_DIR="${MINIENGINE_SHADER_OUTPUT_DIR}"
)

target_link_libraries(engine_renderer
PUBLIC
    engine_core
    engine_platform
    SDL3::SDL3
    Vulkan::Vulkan
)

add_dependencies(engine_renderer engine_renderer_shaders)
