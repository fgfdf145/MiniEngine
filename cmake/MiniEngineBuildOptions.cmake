include_guard(GLOBAL)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")

cmake_host_system_information(
    RESULT MINIENGINE_CPU_COUNT
    QUERY NUMBER_OF_LOGICAL_CORES
)
message(STATUS "Parallel compilation: ${MINIENGINE_CPU_COUNT} logical cores")

if(MSVC)
    add_compile_options(/EHsc)

    # CMake --build --parallel schedules projects. /MP also parallelizes the
    # compiler invocations inside each Visual Studio project.
    if(CMAKE_GENERATOR MATCHES "^Visual Studio")
        add_compile_options("/MP${MINIENGINE_CPU_COUNT}")
    endif()
endif()

# Keep the generated Visual Studio solution navigable without maintaining a
# second source tree in an IDE-specific project file.
function(miniengine_group_target_sources target_name)
    get_target_property(_miniengine_sources ${target_name} SOURCES)
    if(NOT _miniengine_sources)
        return()
    endif()

    set(_miniengine_absolute_sources "")
    foreach(_miniengine_source IN LISTS _miniengine_sources)
        if(_miniengine_source MATCHES "^\\$<")
            continue()
        endif()

        get_filename_component(
            _miniengine_absolute_source
            "${_miniengine_source}"
            ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        list(APPEND _miniengine_absolute_sources "${_miniengine_absolute_source}")
    endforeach()

    if(_miniengine_absolute_sources)
        # source_group(TREE ...) requires every file to live under the tree root. Most targets'
        # sources all sit under their own CMAKE_CURRENT_SOURCE_DIR, but a test target can compile
        # another target's .cpp directly (see miniengine_scene_pass_tests) to avoid linking a
        # whole backend into a unit test. Fall back to the project root, which is a common
        # ancestor of everything, only when that mixed case shows up.
        set(_miniengine_tree_root "${CMAKE_CURRENT_SOURCE_DIR}")
        foreach(_miniengine_absolute_source IN LISTS _miniengine_absolute_sources)
            cmake_path(
                IS_PREFIX CMAKE_CURRENT_SOURCE_DIR "${_miniengine_absolute_source}"
                NORMALIZE _miniengine_is_under_current_dir
            )
            if(NOT _miniengine_is_under_current_dir)
                set(_miniengine_tree_root "${PROJECT_SOURCE_DIR}")
                break()
            endif()
        endforeach()

        source_group(
            TREE "${_miniengine_tree_root}"
            FILES ${_miniengine_absolute_sources}
        )
    endif()
endfunction()

# Canonical runtime directory defaults. These are baked into engine_core as the
# last-resort fallbacks for EnginePaths; every other target resolves its paths
# through EnginePaths at runtime instead of hardcoding a build-machine path.
set(MINIENGINE_SHADER_OUTPUT_DIR "${PROJECT_BINARY_DIR}/shaders")
set(MINIENGINE_DEFAULT_CACHE_DIR "${PROJECT_BINARY_DIR}/cache")
