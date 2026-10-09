cmake_minimum_required(VERSION 3.25)

include(cmake/MiniEngineVcpkg.cmake)

project(MiniEngine VERSION 0.1.2 LANGUAGES CXX)

if(DEFINED CMAKE_TOOLCHAIN_FILE)
    miniengine_is_vcpkg_toolchain(
        miniengine_vcpkg_active "${CMAKE_TOOLCHAIN_FILE}")
else()
    set(miniengine_vcpkg_active FALSE)
endif()

if(miniengine_vcpkg_active)
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
include(cmake/MiniEngineDlss.cmake)

add_subdirectory(third_party/enkiTS)
add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/audio)
add_subdirectory(engine/scene)
add_subdirectory(engine/asset)
add_subdirectory(engine/logic)
add_subdirectory(engine/physics)
add_subdirectory(engine/suspension)
add_subdirectory(engine/tyre)
# A build step of the renderer's shaders, so before it.
add_subdirectory(tools/spirv_invariant_position)
add_subdirectory(engine/renderer)
add_subdirectory(engine/editor)
add_subdirectory(engine/application)
add_subdirectory(app)
add_subdirectory(tools/suspension_rigs)
add_subdirectory(tools/flex_ring)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

include(cmake/MiniEnginePch.cmake)
# The check that picks a target's PCH sees target properties, not a directory's CMAKE_CXX_FLAGS:
# engine/renderer/optimized (Debug built with /O2) stays out of this list.
foreach(_miniengine_pch_directory IN ITEMS
    engine/core
    engine/platform
    engine/audio
    engine/scene
    engine/asset
    engine/logic
    engine/physics
    engine/suspension
    engine/tyre
    engine/renderer
    engine/editor
    engine/application
    app
    tools/suspension_rigs
    tools/flex_ring
)
    miniengine_directory_pch("${_miniengine_pch_directory}")
endforeach()
if(BUILD_TESTING)
    miniengine_directory_pch(tests)
endif()

include(cmake/MiniEngineIde.cmake)
