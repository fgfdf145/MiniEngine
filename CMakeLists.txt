cmake_minimum_required(VERSION 3.25)

project(MiniEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VULKAN_SDK "C:/VulkanSDK/1.4.341.1")
set(Vulkan_ROOT "${VULKAN_SDK}")

find_package(spdlog CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)

add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/renderer)
add_subdirectory(app)
