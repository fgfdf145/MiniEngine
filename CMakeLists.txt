cmake_minimum_required(VERSION 3.25)

project(MiniEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

if(MSVC)
    add_compile_options(/EHsc)
endif()

if(WIN32)
    set(_miniengine_default_fbx_sdk_root "")
    if(DEFINED ENV{FBX_SDK_ROOT} AND NOT "$ENV{FBX_SDK_ROOT}" STREQUAL "")
        set(_miniengine_default_fbx_sdk_root "$ENV{FBX_SDK_ROOT}")
    elseif(EXISTS "C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.9")
        set(_miniengine_default_fbx_sdk_root "C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.9")
    endif()

    _miniengine_set_cached_path(
        MINIENGINE_FBX_SDK_ROOT
        "${_miniengine_default_fbx_sdk_root}"
        "Path to the Autodesk FBX SDK installation root"
    )
    
    set(_miniengine_default_vulkan_sdk_root "")
    if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
        set(_miniengine_default_vulkan_sdk_root "$ENV{VULKAN_SDK}")
    else()
        _miniengine_find_latest_sdk_dir(
            _miniengine_default_vulkan_sdk_root
            "C:/VulkanSDK"
            "Include/vulkan/vulkan.h"
            "Lib/vulkan-1.lib"
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
        set(_miniengine_vulkan_library "${MINIENGINE_VULKAN_SDK_ROOT}/Lib/vulkan-1.lib")
        set(_miniengine_vulkan_glslc "${MINIENGINE_VULKAN_SDK_ROOT}/Bin/glslc.exe")

        if(EXISTS "${_miniengine_vulkan_include_dir}/vulkan/vulkan.h" AND EXISTS "${_miniengine_vulkan_library}")
            if(NOT TARGET Vulkan::Vulkan)
                add_library(Vulkan::Vulkan UNKNOWN IMPORTED GLOBAL)
                set_target_properties(Vulkan::Vulkan PROPERTIES
                    IMPORTED_LOCATION "${_miniengine_vulkan_library}"
                    INTERFACE_INCLUDE_DIRECTORIES "${_miniengine_vulkan_include_dir}"
                )
            endif()

            set(Vulkan_INCLUDE_DIR "${_miniengine_vulkan_include_dir}" CACHE PATH "Path to the Vulkan headers" FORCE)
            set(Vulkan_LIBRARY "${_miniengine_vulkan_library}" CACHE FILEPATH "Path to the Vulkan loader import library" FORCE)

            if(EXISTS "${_miniengine_vulkan_glslc}")
                set(Vulkan_GLSLC_EXECUTABLE "${_miniengine_vulkan_glslc}" CACHE FILEPATH "Path to glslc" FORCE)
            endif()
        endif()
    endif()
elseif(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
    set(Vulkan_ROOT "$ENV{VULKAN_SDK}")
endif()

include(cmake/AddFbxSdk.cmake)

find_package(spdlog CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
if(NOT TARGET Vulkan::Vulkan)
    find_package(Vulkan REQUIRED)
endif()
find_package(glm CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(imguizmo CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(assimp CONFIG REQUIRED)

if(NOT Vulkan_GLSLC_EXECUTABLE)
    set(_miniengine_glslc_hints "")

    if(DEFINED MINIENGINE_VULKAN_SDK_ROOT AND NOT "${MINIENGINE_VULKAN_SDK_ROOT}" STREQUAL "")
        list(APPEND _miniengine_glslc_hints
            "${MINIENGINE_VULKAN_SDK_ROOT}/Bin"
            "${MINIENGINE_VULKAN_SDK_ROOT}/Bin32"
        )
    elseif(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
        list(APPEND _miniengine_glslc_hints
            "$ENV{VULKAN_SDK}/Bin"
            "$ENV{VULKAN_SDK}/Bin32"
        )
    endif()

    if(DEFINED VCPKG_INSTALLED_DIR AND NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "")
        if(DEFINED VCPKG_HOST_TRIPLET AND NOT "${VCPKG_HOST_TRIPLET}" STREQUAL "")
            list(APPEND _miniengine_glslc_hints
                "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/shaderc"
                "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/vulkan"
            )
        endif()

        if(DEFINED VCPKG_TARGET_TRIPLET AND NOT "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
            list(APPEND _miniengine_glslc_hints
                "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shaderc"
                "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/vulkan"
            )
        endif()
    endif()

    find_program(Vulkan_GLSLC_EXECUTABLE
        NAMES glslc
        HINTS ${_miniengine_glslc_hints}
    )
endif()

if(NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR
        "glslc was not found. Install the Vulkan SDK or restore the vcpkg shaderc host dependency."
    )
endif()

add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/scene)
add_subdirectory(engine/logic)
add_subdirectory(engine/renderer)
add_subdirectory(engine/application)
add_subdirectory(app)
