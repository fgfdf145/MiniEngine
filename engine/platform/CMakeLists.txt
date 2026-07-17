add_library(engine_platform
    file_dialog/file_dialog.cpp
    file_dialog/file_dialog.h
    file_dialog/file_dialog_backend.cpp
    file_dialog/file_dialog_backend.h
    ui/ui_scale.cpp
    ui/ui_scale.h
    window/window.cpp
    window/window.h
    window/window_platform.cpp
    window/window_platform.h
)
miniengine_group_target_sources(engine_platform)

target_include_directories(engine_platform
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(engine_platform
    PUBLIC
        engine_core
        Vulkan::Vulkan
        SDL3::SDL3
)
