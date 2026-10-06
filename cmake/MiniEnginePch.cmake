include_guard(GLOBAL)

# Precompiled headers. Most of a translation unit's compile time is the front end parsing the
# standard library, glm, EnTT, ImGui and spdlog again (code generation is a few percent in Debug),
# so cmake/miniengine_pch.h is compiled once and reused.
#
# Two shared PCHs are built: miniengine_pch_engine with engine_core's usage requirements, which
# nearly every target links (spdlog's FMT_SHARED and /utf-8, enkiTS's definitions, the vcpkg
# include directory), and miniengine_pch_plain, the standard library alone, for the targets that
# link nothing that adds to the command line. A target
# whose compile definitions and options match neither (engine_core's path macros, KTX, Jolt, a
# test's fixture directory) gets its own PCH when it has enough sources to pay for it, else none.
# MSVC warns (C4605/C4651) when a PCH is used with different macros, which the check avoids.

set(MINIENGINE_PCH_HEADER "${PROJECT_SOURCE_DIR}/cmake/miniengine_pch.h")

# A target compiling fewer sources than this gains less from its own PCH than the PCH costs.
set(MINIENGINE_OWN_PCH_MIN_SOURCES 6)

option(MINIENGINE_USE_PCH "Compile with precompiled headers" ON)

# The compile definitions and options a target gets from itself and from the usage requirements of
# what it links. Link-only dependencies ($<LINK_ONLY:...>, a static library's private links) do not
# reach the compile line and are skipped.
function(_miniengine_compile_usage target_name out_var)
    set(_usage "")
    set(_visited "")
    set(_pending "${target_name}")
    while(_pending)
        list(POP_FRONT _pending _item)
        if(NOT TARGET "${_item}")
            continue()
        endif()
        get_target_property(_aliased "${_item}" ALIASED_TARGET)
        if(_aliased)
            set(_item "${_aliased}")
        endif()
        if("${_item}" IN_LIST _visited)
            continue()
        endif()
        list(APPEND _visited "${_item}")

        set(_properties INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS INTERFACE_LINK_LIBRARIES)
        if("${_item}" STREQUAL "${target_name}")
            set(_properties COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES)
        endif()
        foreach(_property IN LISTS _properties)
            get_target_property(_values "${_item}" ${_property})
            if(NOT _values)
                continue()
            endif()
            if(_property MATCHES "LINK_LIBRARIES$")
                foreach(_link IN LISTS _values)
                    if(NOT _link MATCHES "\\$<")
                        list(APPEND _pending "${_link}")
                    endif()
                endforeach()
            else()
                foreach(_value IN LISTS _values)
                    list(APPEND _usage "${_value}")
                endforeach()
            endif()
        endforeach()
    endwhile()
    # yaml-cpp's $<$<NOT:$<BOOL:1>>:YAML_CPP_STATIC_DEFINE> and the like add nothing.
    list(FILTER _usage EXCLUDE REGEX "^\\$<\\$<NOT:\\$<BOOL:(1|ON|TRUE)>>:[^;$]*>$")
    list(REMOVE_DUPLICATES _usage)
    list(SORT _usage)
    set(${out_var} "${_usage}" PARENT_SCOPE)
endfunction()

function(_miniengine_add_pch_hosts)
    if(TARGET miniengine_pch_plain)
        return()
    endif()
    foreach(_host IN ITEMS plain engine)
        add_library(miniengine_pch_${_host} STATIC "${PROJECT_SOURCE_DIR}/cmake/miniengine_pch_host.cpp")
        target_precompile_headers(miniengine_pch_${_host} PRIVATE "${MINIENGINE_PCH_HEADER}")
        set_target_properties(miniengine_pch_${_host} PROPERTIES FOLDER "CMake/PCH")
    endforeach()
    target_link_libraries(miniengine_pch_engine PRIVATE engine_core)
endfunction()

# Gives target_name a shared PCH, its own, or none (see the top of this file).
function(miniengine_target_pch target_name)
    if(NOT MINIENGINE_USE_PCH)
        return()
    endif()
    _miniengine_add_pch_hosts()

    _miniengine_compile_usage(${target_name} _target_usage)
    foreach(_host IN ITEMS miniengine_pch_engine miniengine_pch_plain)
        _miniengine_compile_usage(${_host} _host_usage)
        if(_target_usage STREQUAL _host_usage)
            target_precompile_headers(${target_name} REUSE_FROM ${_host})
            message(VERBOSE "PCH: ${target_name} reuses ${_host}")
            return()
        endif()
    endforeach()

    get_target_property(_sources ${target_name} SOURCES)
    list(FILTER _sources INCLUDE REGEX "\\.(cpp|cxx|cc)$")
    list(LENGTH _sources _source_count)
    if(_source_count GREATER_EQUAL MINIENGINE_OWN_PCH_MIN_SOURCES)
        target_precompile_headers(${target_name} PRIVATE "${MINIENGINE_PCH_HEADER}")
        message(VERBOSE "PCH: ${target_name} builds its own")
    else()
        message(VERBOSE "PCH: ${target_name} has none")
    endif()
endfunction()

# Every executable and library defined in a directory (not its subdirectories).
function(miniengine_directory_pch directory)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if(_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            miniengine_target_pch(${_target})
        endif()
    endforeach()
endfunction()
