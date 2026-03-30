add_library(engine_platform
    file_dialog/file_dialog.cpp
    file_dialog/file_dialog_backend.cpp
    window/window.cpp
    window/window_platform.cpp
)

target_include_directories(engine_platform
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(engine_platform
PUBLIC
    engine_core
    SDL3::SDL3
    Vulkan::Vulkan
)
