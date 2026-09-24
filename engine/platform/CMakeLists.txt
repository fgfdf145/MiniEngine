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
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(engine_platform
    PUBLIC
        engine_core
        Vulkan::Vulkan
        SDL3::SDL3
)

# macOS runs Vulkan through MoltenVK, which vcpkg does not provide. Bake in the driver manifest
# of a Homebrew or Vulkan SDK install so the app starts without a hand-set VK_DRIVER_FILES.
if(APPLE)
    set(_miniengine_moltenvk_icd_hints
        /opt/homebrew/etc/vulkan/icd.d
        /opt/homebrew/share/vulkan/icd.d
        /usr/local/etc/vulkan/icd.d
        /usr/local/share/vulkan/icd.d
    )
    if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
        list(PREPEND _miniengine_moltenvk_icd_hints "$ENV{VULKAN_SDK}/share/vulkan/icd.d")
    endif()

    find_file(MINIENGINE_MOLTENVK_ICD
        NAMES MoltenVK_icd.json
        HINTS ${_miniengine_moltenvk_icd_hints}
        NO_DEFAULT_PATH
    )

    if(MINIENGINE_MOLTENVK_ICD)
        set_source_files_properties(window/window_platform.cpp
            PROPERTIES
                COMPILE_DEFINITIONS "MINIENGINE_MOLTENVK_ICD_PATH=\"${MINIENGINE_MOLTENVK_ICD}\""
        )
    else()
        message(WARNING
            "MoltenVK was not found; the app will need VK_DRIVER_FILES at runtime. "
            "Install it with 'brew install molten-vk'."
        )
    endif()
endif()
