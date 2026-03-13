cmake_minimum_required(VERSION 3.25)

project(MiniEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    set(_miniengine_default_fbx_sdk_root "")
    if(DEFINED ENV{FBX_SDK_ROOT} AND NOT "$ENV{FBX_SDK_ROOT}" STREQUAL "")
        set(_miniengine_default_fbx_sdk_root "$ENV{FBX_SDK_ROOT}")
    elseif(EXISTS "C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.9")
        set(_miniengine_default_fbx_sdk_root "C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.9")
    endif()

    set(
        MINIENGINE_FBX_SDK_ROOT
        "${_miniengine_default_fbx_sdk_root}"
        CACHE PATH
        "Path to the Autodesk FBX SDK installation root"
    )
endif()

if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
    set(Vulkan_ROOT "$ENV{VULKAN_SDK}")
endif()

include(cmake/AddFbxSdk.cmake)

find_package(spdlog CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(imguizmo CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(assimp CONFIG REQUIRED)

add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/renderer)
add_subdirectory(app)
