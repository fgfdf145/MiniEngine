add_library(engine_platform
    file_dialog/file_dialog.cpp
    file_dialog/file_dialog_backend.cpp
    ui/ui_scale.cpp
    window/window.cpp
    window/window_platform.cpp
)

target_include_directories(engine_platform
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine/renderer
)

target_link_libraries(engine_platform
PUBLIC
    engine_core
    SDL3::SDL3
)

if(TARGET Vulkan::Vulkan)
    target_link_libraries(engine_platform
    PUBLIC
        Vulkan::Vulkan
    )
endif()

if(APPLE)
    if(MINIENGINE_APPLE_FRAMEWORKS)
        target_link_libraries(engine_platform
        PUBLIC
            ${MINIENGINE_APPLE_FRAMEWORKS}
        )
    endif()

    if(DEFINED MINIENGINE_VULKAN_LOADER_PATH AND NOT "${MINIENGINE_VULKAN_LOADER_PATH}" STREQUAL "")
        target_compile_definitions(engine_platform
        PRIVATE
            MINIENGINE_VULKAN_LOADER_PATH="${MINIENGINE_VULKAN_LOADER_PATH}"
        )
    endif()
endif()
