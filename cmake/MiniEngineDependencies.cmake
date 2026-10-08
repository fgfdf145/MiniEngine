include_guard(GLOBAL)

function(_miniengine_set_cached_path cache_var default_value docstring)
    if(DEFINED ${cache_var} AND NOT "${${cache_var}}" STREQUAL "")
        set(_miniengine_cached_value "${${cache_var}}")
        set(_miniengine_force_mode)
    else()
        set(_miniengine_cached_value "${default_value}")
        set(_miniengine_force_mode FORCE)
    endif()

    set(${cache_var}
        "${_miniengine_cached_value}"
        CACHE PATH
        "${docstring}"
        ${_miniengine_force_mode}
    )
endfunction()

function(_miniengine_find_latest_sdk_dir out_var root_dir header_relative_path library_relative_path)
    set(_miniengine_sdk_dir "")

    if(EXISTS "${root_dir}/${header_relative_path}" AND EXISTS "${root_dir}/${library_relative_path}")
        set(_miniengine_sdk_dir "${root_dir}")
    elseif(EXISTS "${root_dir}")
        file(GLOB _miniengine_sdk_candidates LIST_DIRECTORIES true "${root_dir}/*")
        set(_miniengine_matching_sdk_dirs "")

        foreach(_miniengine_sdk_candidate IN LISTS _miniengine_sdk_candidates)
            if(IS_DIRECTORY "${_miniengine_sdk_candidate}" AND
               EXISTS "${_miniengine_sdk_candidate}/${header_relative_path}" AND
               EXISTS "${_miniengine_sdk_candidate}/${library_relative_path}")
                list(APPEND _miniengine_matching_sdk_dirs "${_miniengine_sdk_candidate}")
            endif()
        endforeach()

        if(_miniengine_matching_sdk_dirs)
            list(SORT _miniengine_matching_sdk_dirs COMPARE NATURAL ORDER DESCENDING)
            list(GET _miniengine_matching_sdk_dirs 0 _miniengine_sdk_dir)
        endif()
    endif()

    set(${out_var} "${_miniengine_sdk_dir}" PARENT_SCOPE)
endfunction()

if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_miniengine_vulkan_library_relative_path "Lib32/vulkan-1.lib")
    else()
        set(_miniengine_vulkan_library_relative_path "Lib/vulkan-1.lib")
    endif()

    set(_miniengine_default_vulkan_sdk_root "")
    if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
        set(_miniengine_default_vulkan_sdk_root "$ENV{VULKAN_SDK}")
    else()
        _miniengine_find_latest_sdk_dir(
            _miniengine_default_vulkan_sdk_root
            "C:/VulkanSDK"
            "Include/vulkan/vulkan.h"
            "${_miniengine_vulkan_library_relative_path}"
        )
    endif()

    _miniengine_set_cached_path(
        MINIENGINE_VULKAN_SDK_ROOT
        "${_miniengine_default_vulkan_sdk_root}"
        "Path to the Vulkan SDK installation root"
    )

    if(NOT MINIENGINE_VULKAN_SDK_ROOT STREQUAL "")
        set(Vulkan_ROOT "${MINIENGINE_VULKAN_SDK_ROOT}")
        set(_miniengine_vulkan_include_dir "${MINIENGINE_VULKAN_SDK_ROOT}/Include")
        set(_miniengine_vulkan_library
            "${MINIENGINE_VULKAN_SDK_ROOT}/${_miniengine_vulkan_library_relative_path}"
        )

        if(EXISTS "${_miniengine_vulkan_include_dir}/vulkan/vulkan.h" AND
           EXISTS "${_miniengine_vulkan_library}")
            if(NOT TARGET Vulkan::Vulkan)
                add_library(Vulkan::Vulkan UNKNOWN IMPORTED GLOBAL)
                set_target_properties(Vulkan::Vulkan PROPERTIES
                    IMPORTED_LOCATION "${_miniengine_vulkan_library}"
                    INTERFACE_INCLUDE_DIRECTORIES "${_miniengine_vulkan_include_dir}"
                )
            endif()

            set(Vulkan_INCLUDE_DIR
                "${_miniengine_vulkan_include_dir}"
                CACHE PATH
                "Path to the Vulkan headers"
                FORCE
            )
            set(Vulkan_LIBRARY
                "${_miniengine_vulkan_library}"
                CACHE FILEPATH
                "Path to the Vulkan loader import library"
                FORCE
            )
        endif()
    endif()

    # A 64-bit-only SDK cannot satisfy a Win32 generator. Clear any stale
    # cached SDK result so FindVulkan can resolve the vcpkg x86 package.
    if(NOT TARGET Vulkan::Vulkan)
        unset(Vulkan_INCLUDE_DIR CACHE)
        unset(Vulkan_LIBRARY CACHE)
    endif()
elseif(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
    set(Vulkan_ROOT "$ENV{VULKAN_SDK}")
endif()

find_package(spdlog CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
if(NOT TARGET Vulkan::Vulkan)
    find_package(Vulkan REQUIRED)
endif()
find_package(glm CONFIG REQUIRED)
# The render backend's Vulkan layer (docs/design/2026-10-08-nvrhi-backend-design.md): a shared nvrhi
# with the Vulkan backend inside, or with a static triplet nvrhi plus nvrhi_vk.
find_package(nvrhi CONFIG REQUIRED)
# Its export asks every consumer for VK_USE_PLATFORM_WIN32_KHR, which makes vulkan.h include
# <windows.h> (min and max macros) in every file that sees the engine's Vulkan headers. Nothing
# outside NVRHI needs the Win32 surface types.
set_property(TARGET nvrhi PROPERTY INTERFACE_COMPILE_DEFINITIONS "")
set(MINIENGINE_NVRHI_TARGETS nvrhi)
if(TARGET nvrhi_vk)
    list(APPEND MINIENGINE_NVRHI_TARGETS nvrhi_vk)
endif()
find_package(imgui CONFIG REQUIRED)
find_package(imguizmo CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
# Vehicle physics (engine_physics): the static Jolt::Jolt, whose exported compile definitions keep
# its headers configured as the library was built.
find_package(Jolt CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(unofficial-bc7enc-rdo CONFIG REQUIRED)
find_package(tinyexr CONFIG REQUIRED)
# glTF compressed geometry: EXT/KHR_meshopt_compression and KHR_draco_mesh_compression.
find_package(meshoptimizer CONFIG REQUIRED)
find_package(draco CONFIG REQUIRED)
# KHR_texture_basisu. vcpkg's ktx port does not build for x86 Windows, where vcpkg.json leaves it
# out: that build loads no KTX2 textures and says so when a model asks for one.
find_package(Ktx CONFIG)
find_path(MINIENGINE_TINYGLTF_INCLUDE_DIR NAMES tiny_gltf.h REQUIRED)
# engine_audio: miniaudio is a single header, its implementation compiled in engine/audio.
find_path(MINIENGINE_MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h REQUIRED)

# The Slang compiler for the shaders (engine/renderer/CMakeLists.txt): the Vulkan SDK's, else the
# vcpkg shader-slang host package's.
set(_miniengine_slangc_hints "")
if(DEFINED MINIENGINE_VULKAN_SDK_ROOT AND NOT "${MINIENGINE_VULKAN_SDK_ROOT}" STREQUAL "")
    list(APPEND _miniengine_slangc_hints "${MINIENGINE_VULKAN_SDK_ROOT}/Bin")
elseif(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
    list(APPEND _miniengine_slangc_hints "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
endif()
if(DEFINED VCPKG_INSTALLED_DIR AND NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "")
    foreach(_miniengine_triplet IN ITEMS "${VCPKG_HOST_TRIPLET}" "${VCPKG_TARGET_TRIPLET}")
        if(NOT "${_miniengine_triplet}" STREQUAL "")
            list(APPEND _miniengine_slangc_hints
                "${VCPKG_INSTALLED_DIR}/${_miniengine_triplet}/tools/shader-slang"
                "${VCPKG_INSTALLED_DIR}/${_miniengine_triplet}/tools/shader-slang/bin"
            )
        endif()
    endforeach()
endif()
find_program(MINIENGINE_SLANGC_EXECUTABLE
    NAMES slangc
    HINTS ${_miniengine_slangc_hints}
)
if(NOT MINIENGINE_SLANGC_EXECUTABLE)
    message(FATAL_ERROR
        "slangc was not found. Install the Vulkan SDK or restore the vcpkg shader-slang host dependency."
    )
endif()
