cmake_minimum_required(VERSION 3.25)

include(cmake/MiniEngineVcpkg.cmake)

project(MiniEngine LANGUAGES CXX)

if(DEFINED CMAKE_TOOLCHAIN_FILE)
    miniengine_validate_vcpkg_installed_dir(
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}"
        "${VCPKG_TARGET_TRIPLET}"
        "${VCPKG_INSTALLED_DIR}"
    )
endif()

include(CTest)
include(cmake/MiniEngineBuildOptions.cmake)
include(cmake/MiniEngineDependencies.cmake)

add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/scene)
add_subdirectory(engine/asset)
add_subdirectory(engine/logic)
add_subdirectory(engine/renderer)
add_subdirectory(engine/editor)
add_subdirectory(engine/application)
add_subdirectory(app)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

include(cmake/MiniEngineIde.cmake)
