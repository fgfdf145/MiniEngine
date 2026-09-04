cmake_minimum_required(VERSION 3.25)

# Script-mode entry point for the Doxygen + Graphviz documentation build.
# Run it through scripts/build-docs.ps1 or scripts/build-docs.sh, or directly:
#
#   cmake -P cmake/MiniEngineDocs.cmake
#
# Overrides: MINIENGINE_DOXYGEN, MINIENGINE_DOT, MINIENGINE_DOCS_OUTPUT.

get_filename_component(MINIENGINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Doxygen 1.9.5 introduced the HTML_COLORSTYLE values the template relies on.
set(MINIENGINE_DOXYGEN_MINIMUM_VERSION 1.9.5)

if(NOT DEFINED MINIENGINE_DOCS_OUTPUT OR MINIENGINE_DOCS_OUTPUT STREQUAL "")
    set(MINIENGINE_DOCS_OUTPUT "${MINIENGINE_ROOT}/out/docs")
endif()
get_filename_component(
    MINIENGINE_DOCS_OUTPUT "${MINIENGINE_DOCS_OUTPUT}" ABSOLUTE)

function(miniengine_find_docs_tool output_variable override_value display_name)
    set(_names ${ARGN})
    if(DEFINED override_value AND NOT override_value STREQUAL "")
        if(NOT EXISTS "${override_value}")
            message(FATAL_ERROR
                "${display_name} override does not exist: ${override_value}")
        endif()
        set(${output_variable} "${override_value}" PARENT_SCOPE)
        return()
    endif()

    # A shell started before the installer ran still carries the old PATH, so
    # look in the default Windows install locations as well.
    set(_hints)
    if(WIN32)
        list(APPEND _hints
            "$ENV{ProgramFiles}/doxygen/bin"
            "$ENV{ProgramFiles}/Graphviz/bin"
            "$ENV{ProgramFiles\(x86\)}/doxygen/bin"
            "$ENV{ProgramFiles\(x86\)}/Graphviz/bin")
    endif()

    unset(_discovered CACHE)
    unset(_discovered)
    find_program(_discovered NAMES ${_names} HINTS ${_hints})
    if(NOT _discovered)
        list(JOIN _names "', '" _name_text)
        message(FATAL_ERROR
            "Could not find ${display_name}. Tried command names '${_name_text}'. "
            "Install it and make sure it is on PATH, or set the "
            "${output_variable} variable explicitly.")
    endif()
    set(${output_variable} "${_discovered}" PARENT_SCOPE)
endfunction()

miniengine_find_docs_tool(
    MINIENGINE_DOXYGEN "${MINIENGINE_DOXYGEN}" "doxygen" doxygen)
miniengine_find_docs_tool(
    MINIENGINE_DOT "${MINIENGINE_DOT}" "the Graphviz 'dot' tool" dot)

execute_process(
    COMMAND "${MINIENGINE_DOXYGEN}" --version
    RESULT_VARIABLE _doxygen_version_result
    OUTPUT_VARIABLE _doxygen_version_output
    ERROR_VARIABLE _doxygen_version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT _doxygen_version_result EQUAL 0)
    message(FATAL_ERROR
        "Running '${MINIENGINE_DOXYGEN} --version' failed: ${_doxygen_version_error}")
endif()
# doxygen reports "1.18.0 (<commit>)"; keep the numeric part.
string(REGEX MATCH "^[0-9]+\.[0-9]+(\.[0-9]+)?"
    MINIENGINE_DOXYGEN_VERSION "${_doxygen_version_output}")
if(MINIENGINE_DOXYGEN_VERSION STREQUAL "")
    message(FATAL_ERROR
        "Could not parse a version out of doxygen --version: ${_doxygen_version_output}")
endif()
if(MINIENGINE_DOXYGEN_VERSION VERSION_LESS MINIENGINE_DOXYGEN_MINIMUM_VERSION)
    message(FATAL_ERROR
        "doxygen ${MINIENGINE_DOXYGEN_VERSION} is too old; "
        "${MINIENGINE_DOXYGEN_MINIMUM_VERSION} or newer is required "
        "(found ${MINIENGINE_DOXYGEN}).")
endif()

# Graphviz is optional as far as doxygen is concerned: without a working dot it
# silently drops every graph, which is the whole point of this configuration.
execute_process(
    COMMAND "${MINIENGINE_DOT}" -V
    RESULT_VARIABLE _dot_version_result
    OUTPUT_VARIABLE _dot_version_output
    ERROR_VARIABLE _dot_version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT _dot_version_result EQUAL 0)
    message(FATAL_ERROR
        "Running '${MINIENGINE_DOT} -V' failed, so no graphs would be "
        "generated: ${_dot_version_error}")
endif()
set(MINIENGINE_DOT_VERSION "${_dot_version_output}${_dot_version_error}")

get_filename_component(MINIENGINE_DOT_PATH "${MINIENGINE_DOT}" DIRECTORY)

cmake_host_system_information(
    RESULT MINIENGINE_DOT_NUM_THREADS
    QUERY NUMBER_OF_LOGICAL_CORES)
if(NOT MINIENGINE_DOT_NUM_THREADS OR MINIENGINE_DOT_NUM_THREADS LESS 1)
    set(MINIENGINE_DOT_NUM_THREADS 1)
endif()
# DOT_NUM_THREADS caps out at 32 in doxygen.
if(MINIENGINE_DOT_NUM_THREADS GREATER 32)
    set(MINIENGINE_DOT_NUM_THREADS 32)
endif()

# Stamp the documentation with the commit it describes.
find_program(GIT_EXECUTABLE NAMES git)
set(MINIENGINE_DOCS_REVISION "unversioned")
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${MINIENGINE_ROOT}" rev-parse --short HEAD
        RESULT_VARIABLE _revision_result
        OUTPUT_VARIABLE _revision_output
        ERROR_VARIABLE _revision_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_revision_result EQUAL 0 AND NOT _revision_output STREQUAL "")
        set(MINIENGINE_DOCS_REVISION "${_revision_output}")
    endif()
endif()

# Drop the previous run so removed symbols do not linger as stale pages.
if(IS_DIRECTORY "${MINIENGINE_DOCS_OUTPUT}/html")
    file(REMOVE_RECURSE "${MINIENGINE_DOCS_OUTPUT}/html")
endif()
file(MAKE_DIRECTORY "${MINIENGINE_DOCS_OUTPUT}")
file(REMOVE "${MINIENGINE_DOCS_OUTPUT}/doxygen-warnings.log")

set(MINIENGINE_DOXYFILE "${MINIENGINE_DOCS_OUTPUT}/Doxyfile")
configure_file(
    "${MINIENGINE_ROOT}/docs/Doxyfile.in"
    "${MINIENGINE_DOXYFILE}"
    @ONLY)

message(STATUS "doxygen ${MINIENGINE_DOXYGEN_VERSION}: ${MINIENGINE_DOXYGEN}")
message(STATUS "${MINIENGINE_DOT_VERSION}")
message(STATUS "Generating MiniEngine docs into ${MINIENGINE_DOCS_OUTPUT}")

execute_process(
    COMMAND "${MINIENGINE_DOXYGEN}" "${MINIENGINE_DOXYFILE}"
    WORKING_DIRECTORY "${MINIENGINE_ROOT}"
    RESULT_VARIABLE _doxygen_result)
if(NOT _doxygen_result EQUAL 0)
    message(FATAL_ERROR "doxygen failed with exit code ${_doxygen_result}")
endif()

set(MINIENGINE_DOCS_INDEX "${MINIENGINE_DOCS_OUTPUT}/html/index.html")
if(NOT EXISTS "${MINIENGINE_DOCS_INDEX}")
    message(FATAL_ERROR
        "doxygen reported success but ${MINIENGINE_DOCS_INDEX} was not written")
endif()

file(GLOB_RECURSE _generated_graphs "${MINIENGINE_DOCS_OUTPUT}/html/*.svg")
list(LENGTH _generated_graphs _graph_count)
if(_graph_count EQUAL 0)
    message(FATAL_ERROR
        "No Graphviz output was produced; check that '${MINIENGINE_DOT}' works.")
endif()

set(_warning_log "${MINIENGINE_DOCS_OUTPUT}/doxygen-warnings.log")
set(_warning_count 0)
if(EXISTS "${_warning_log}")
    file(STRINGS "${_warning_log}" _warning_lines REGEX "warning:")
    list(LENGTH _warning_lines _warning_count)
endif()

message(STATUS
    "MiniEngine docs generated: ${_graph_count} graphs, ${_warning_count} warnings")
if(_warning_count GREATER 0)
    message(STATUS "Warning log: ${_warning_log}")
endif()
message(STATUS "Open ${MINIENGINE_DOCS_INDEX}")
