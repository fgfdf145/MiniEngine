cmake_minimum_required(VERSION 3.25)

get_filename_component(MINIENGINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/MiniEngineFormatSupport.cmake")

if(NOT DEFINED MINIENGINE_FORMAT_MODE)
    set(MINIENGINE_FORMAT_MODE CHECK)
endif()
string(TOUPPER "${MINIENGINE_FORMAT_MODE}" MINIENGINE_FORMAT_MODE)
if(NOT MINIENGINE_FORMAT_MODE STREQUAL "CHECK" AND
   NOT MINIENGINE_FORMAT_MODE STREQUAL "APPLY")
    message(FATAL_ERROR "MINIENGINE_FORMAT_MODE must be CHECK or APPLY")
endif()

find_program(GIT_EXECUTABLE NAMES git REQUIRED)
miniengine_select_clang_format(CLANG_FORMAT_EXECUTABLE CLANG_FORMAT_SOURCE)
miniengine_require_clang_format_22(
    "${CLANG_FORMAT_EXECUTABLE}" "${CLANG_FORMAT_SOURCE}")

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${MINIENGINE_ROOT}" ls-files --
        CMakeLists.txt app engine tests shaders scripts cmake
    RESULT_VARIABLE _git_result
    OUTPUT_VARIABLE _tracked_output
    ERROR_VARIABLE _git_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _git_result EQUAL 0)
    message(FATAL_ERROR "git ls-files failed: ${_git_error}")
endif()

string(REPLACE "\r\n" "\n" _tracked_output "${_tracked_output}")
string(REPLACE "\n" ";" _tracked_files "${_tracked_output}")
list(FILTER _tracked_files EXCLUDE REGEX "^$")

miniengine_classify_format_files(
    "${_tracked_files}" _clang_relative_files _script_files _whitespace_files)
set(_clang_files)
foreach(_relative_path IN LISTS _clang_relative_files)
    list(APPEND _clang_files "${MINIENGINE_ROOT}/${_relative_path}")
endforeach()

if(NOT _clang_files)
    message(FATAL_ERROR "No C/C++ or GLSL files were discovered")
endif()

if(MINIENGINE_FORMAT_MODE STREQUAL "APPLY")
    set(_clang_arguments -i --style=file --fallback-style=none --)
else()
    set(_clang_arguments --dry-run --Werror --style=file --fallback-style=none --)
endif()

execute_process(
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" ${_clang_arguments} ${_clang_files}
    WORKING_DIRECTORY "${MINIENGINE_ROOT}"
    RESULT_VARIABLE _clang_result
    OUTPUT_VARIABLE _clang_output
    ERROR_VARIABLE _clang_error)
if(NOT _clang_result EQUAL 0)
    message(FATAL_ERROR
        "clang-format ${MINIENGINE_FORMAT_MODE} failed:\n${_clang_output}${_clang_error}")
endif()

set(_style_violations)
foreach(_relative_path IN LISTS _script_files)
    miniengine_check_script_file(
        "${MINIENGINE_ROOT}/${_relative_path}" "${_relative_path}"
        _script_violations)
    list(APPEND _style_violations ${_script_violations})
endforeach()

foreach(_relative_path IN LISTS _whitespace_files)
    miniengine_check_whitespace_file(
        "${MINIENGINE_ROOT}/${_relative_path}" "${_relative_path}"
        _whitespace_violations)
    list(APPEND _style_violations ${_whitespace_violations})
endforeach()

if(_style_violations)
    list(JOIN _style_violations "\n  " _violation_text)
    message(FATAL_ERROR "Allman/style violations:\n  ${_violation_text}")
endif()

list(LENGTH _clang_files _clang_count)
list(LENGTH _script_files _script_count)
message(STATUS
    "MiniEngine format ${MINIENGINE_FORMAT_MODE} passed: ${_clang_count} clang-format files, ${_script_count} scripts")
