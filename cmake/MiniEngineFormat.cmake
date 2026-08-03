cmake_minimum_required(VERSION 3.25)

get_filename_component(MINIENGINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED MINIENGINE_FORMAT_MODE)
    set(MINIENGINE_FORMAT_MODE CHECK)
endif()
string(TOUPPER "${MINIENGINE_FORMAT_MODE}" MINIENGINE_FORMAT_MODE)
if(NOT MINIENGINE_FORMAT_MODE STREQUAL "CHECK" AND
   NOT MINIENGINE_FORMAT_MODE STREQUAL "APPLY")
    message(FATAL_ERROR "MINIENGINE_FORMAT_MODE must be CHECK or APPLY")
endif()

find_program(GIT_EXECUTABLE NAMES git REQUIRED)

if(DEFINED MINIENGINE_CLANG_FORMAT AND NOT MINIENGINE_CLANG_FORMAT STREQUAL "")
    set(CLANG_FORMAT_EXECUTABLE "${MINIENGINE_CLANG_FORMAT}")
else()
    set(_clang_format_hints)
    if(WIN32)
        list(APPEND _clang_format_hints
            "$ENV{ProgramFiles}/LLVM/bin"
            "$ENV{ProgramFiles}/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin")
    endif()
    find_program(CLANG_FORMAT_EXECUTABLE
        NAMES clang-format clang-format-22
        HINTS ${_clang_format_hints}
        REQUIRED)
endif()

execute_process(
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" --version
    RESULT_VARIABLE _version_result
    OUTPUT_VARIABLE _version_output
    ERROR_VARIABLE _version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _version_result EQUAL 0)
    message(FATAL_ERROR "clang-format --version failed: ${_version_error}")
endif()
if(NOT _version_output MATCHES "clang-format version 22\\.")
    message(FATAL_ERROR "clang-format 22.x is required; found: ${_version_output}")
endif()

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

set(_clang_files)
set(_script_files)
set(_whitespace_files)
foreach(_relative_path IN LISTS _tracked_files)
    set(_absolute_path "${MINIENGINE_ROOT}/${_relative_path}")
    if(_relative_path MATCHES "\\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|vert|frag|comp|geom|tesc|tese|glsl)$")
        list(APPEND _clang_files "${_absolute_path}")
    endif()
    if(_relative_path MATCHES "\\.(ps1|sh)$")
        list(APPEND _script_files "${_relative_path}")
    endif()
    if(_relative_path STREQUAL "CMakeLists.txt" OR
       _relative_path MATCHES "\\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|vert|frag|comp|geom|tesc|tese|glsl|ps1|sh|cmake)$")
        list(APPEND _whitespace_files "${_relative_path}")
    endif()
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
    file(READ "${MINIENGINE_ROOT}/${_relative_path}" _contents)
    if(_relative_path MATCHES "\\.ps1$")
        if(_contents MATCHES "(^|\n)[^\n]*\\)[ \t]*\\{" OR
           _contents MATCHES "(^|\n)[ \t]*(else|try|finally|do)[ \t]*\\{" OR
           _contents MATCHES "(^|\n)[ \t]*(class|enum)[^\n{]*\\{")
            list(APPEND _style_violations "${_relative_path}: PowerShell opening brace must be on the following line")
        endif()
    elseif(_contents MATCHES "(^|\n)[ \t]*(function[ \t]+)?[A-Za-z_][A-Za-z0-9_]*[ \t]*(\\(\\))?[ \t]*\\{")
        list(APPEND _style_violations "${_relative_path}: Bash function opening brace must be on the following line")
    endif()
endforeach()

foreach(_relative_path IN LISTS _whitespace_files)
    file(READ "${MINIENGINE_ROOT}/${_relative_path}" _contents)
    if(_contents MATCHES "[ \t]+(\r?\n|$)")
        list(APPEND _style_violations "${_relative_path}: trailing whitespace")
    endif()
endforeach()

if(_style_violations)
    list(JOIN _style_violations "\n  " _violation_text)
    message(FATAL_ERROR "Allman/style violations:\n  ${_violation_text}")
endif()

list(LENGTH _clang_files _clang_count)
list(LENGTH _script_files _script_count)
message(STATUS
    "MiniEngine format ${MINIENGINE_FORMAT_MODE} passed: ${_clang_count} clang-format files, ${_script_count} scripts")
