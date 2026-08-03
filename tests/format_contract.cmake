cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED REPO_ROOT OR NOT DEFINED TEST_TEMP_ROOT OR NOT DEFINED TEST_CASE)
    message(FATAL_ERROR "REPO_ROOT, TEST_TEMP_ROOT, and TEST_CASE are required")
endif()

file(TO_CMAKE_PATH "${REPO_ROOT}" repo_root_cmake)
file(TO_CMAKE_PATH "${TEST_TEMP_ROOT}" test_temp_root_cmake)
include("${REPO_ROOT}/cmake/MiniEngineFormatSupport.cmake")

function(assert_equal actual expected context)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "${context}: expected '${expected}', got '${actual}'")
    endif()
endfunction()

function(assert_contains haystack needle context)
    string(FIND "${haystack}" "${needle}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "${context}: expected output to contain '${needle}':\n${haystack}")
    endif()
endfunction()

set(fixture_root "${REPO_ROOT}/tests/fixtures/format")

if(TEST_CASE STREQUAL "powershell_accepts_literals")
    miniengine_check_script_file(
        "${fixture_root}/accepted-powershell.txt" "accepted.ps1" violations)
    assert_equal("${violations}" "" "PowerShell literals and comments")
elseif(TEST_CASE STREQUAL "powershell_rejects_same_line_braces")
    miniengine_check_script_file(
        "${fixture_root}/rejected-powershell.txt" "rejected.ps1" violations)
    list(LENGTH violations violation_count)
    assert_equal("${violation_count}" "24" "PowerShell block and indentation coverage")
elseif(TEST_CASE STREQUAL "bash_accepts_heredoc_literals")
    miniengine_check_script_file(
        "${fixture_root}/accepted-bash.txt" "accepted.sh" violations)
    assert_equal("${violations}" "" "Bash heredoc, literals, and comments")
elseif(TEST_CASE STREQUAL "bash_rejects_hyphenated_function")
    miniengine_check_script_file(
        "${fixture_root}/rejected-bash.txt" "rejected.sh" violations)
    list(LENGTH violations violation_count)
    assert_equal("${violation_count}" "3" "Bash function and indentation coverage")
elseif(TEST_CASE STREQUAL "nested_cmakelists_classification")
    set(tracked_files
        CMakeLists.txt
        engine/core/CMakeLists.txt
        cmake/example.cmake
        README.md)
    miniengine_classify_format_files(
        "${tracked_files}" clang_files script_files whitespace_files)
    list(FIND whitespace_files "engine/core/CMakeLists.txt" nested_index)
    if(nested_index EQUAL -1)
        message(FATAL_ERROR "Nested CMakeLists.txt was not classified for whitespace checks")
    endif()
    set(nested_fixture "${test_temp_root_cmake}/nested/CMakeLists.txt")
    file(MAKE_DIRECTORY "${test_temp_root_cmake}/nested")
    file(WRITE "${nested_fixture}" "add_library(example INTERFACE) \n")
    miniengine_check_whitespace_file(
        "${nested_fixture}" "nested/CMakeLists.txt" violations)
    assert_contains(
        "${violations}" "nested/CMakeLists.txt: trailing whitespace"
        "Nested CMakeLists.txt whitespace")
elseif(TEST_CASE STREQUAL "explicit_formatter_override")
    set(MINIENGINE_CLANG_FORMAT "clang-format-explicit")
    set(ENV{CLANG_FORMAT} "clang-format-environment")
    miniengine_select_clang_format(formatter formatter_source)
    assert_equal("${formatter}" "clang-format-explicit" "Explicit override value")
    assert_equal("${formatter_source}" "MINIENGINE_CLANG_FORMAT" "Explicit override source")
elseif(TEST_CASE STREQUAL "environment_formatter_override")
    unset(MINIENGINE_CLANG_FORMAT)
    set(ENV{CLANG_FORMAT} "clang-format-environment")
    miniengine_select_clang_format(formatter formatter_source)
    assert_equal("${formatter}" "clang-format-environment" "Environment override value")
    assert_equal("${formatter_source}" "CLANG_FORMAT environment variable" "Environment override source")
elseif(TEST_CASE STREQUAL "automatic_formatter_preference")
    unset(MINIENGINE_CLANG_FORMAT)
    set(ENV{CLANG_FORMAT} "")
    set(tool_dir "${test_temp_root_cmake}/format-contract-tools")
    file(MAKE_DIRECTORY "${tool_dir}")
    if(WIN32)
        set(tool_suffix ".exe")
    else()
        set(tool_suffix "")
    endif()
    file(COPY_FILE "${CMAKE_COMMAND}" "${tool_dir}/clang-format${tool_suffix}")
    file(COPY_FILE "${CMAKE_COMMAND}" "${tool_dir}/clang-format-22${tool_suffix}")
    if(NOT WIN32)
        file(CHMOD
            "${tool_dir}/clang-format"
            "${tool_dir}/clang-format-22"
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    endif()
    set(MINIENGINE_CLANG_FORMAT_HINTS "${tool_dir}")
    miniengine_select_clang_format(formatter formatter_source)
    get_filename_component(formatter_name "${formatter}" NAME_WE)
    assert_equal("${formatter_name}" "clang-format-22" "Automatic discovery order")
    assert_equal("${formatter_source}" "automatic discovery" "Automatic discovery source")
elseif(TEST_CASE STREQUAL "missing_formatter_diagnostic" OR
       TEST_CASE STREQUAL "wrong_formatter_diagnostic")
    set(child_dir "${test_temp_root_cmake}/format-contract-${TEST_CASE}")
    file(MAKE_DIRECTORY "${child_dir}")
    if(TEST_CASE STREQUAL "missing_formatter_diagnostic")
        set(formatter "miniengine-definitely-missing-clang-format")
        set(expected_result "result/cause:")
    elseif(WIN32)
        set(formatter "${child_dir}/wrong-clang-format.cmd")
        file(WRITE "${formatter}" "@echo off\r\necho clang-format version 21.0.0\r\n")
        set(expected_result "clang-format 22.x is required")
    else()
        set(formatter "${child_dir}/wrong-clang-format")
        file(WRITE "${formatter}" "#!/usr/bin/env sh\necho 'clang-format version 21.0.0'\n")
        file(CHMOD "${formatter}"
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        set(expected_result "clang-format 22.x is required")
    endif()
    set(child_script "${child_dir}/probe.cmake")
    file(WRITE "${child_script}"
        "include(\"${repo_root_cmake}/cmake/MiniEngineFormatSupport.cmake\")\n"
        "miniengine_require_clang_format_22(\"${formatter}\" \"contract override\")\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -P "${child_script}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    if(result EQUAL 0)
        message(FATAL_ERROR "Expected formatter probe to fail")
    endif()
    set(output "${stdout}${stderr}")
    assert_contains("${output}" "${formatter}" "Formatter diagnostic executable")
    assert_contains("${output}" "contract override" "Formatter diagnostic source")
    assert_contains("${output}" "${expected_result}" "Formatter diagnostic cause")
elseif(TEST_CASE STREQUAL "powershell_command_resolution")
    if(NOT WIN32)
        return()
    endif()
    execute_process(
        COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command
            ". '${REPO_ROOT}/scripts/MiniEngineFormatCommand.ps1'; Resolve-MiniEngineFormatCommand 'clang-format-22'; Resolve-MiniEngineFormatCommand './tools/clang-format.exe'"
        WORKING_DIRECTORY "${REPO_ROOT}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "PowerShell resolver failed: ${stderr}")
    endif()
    string(REPLACE "\r\n" "\n" stdout "${stdout}")
    string(REPLACE "\n" ";" output_lines "${stdout}")
    list(GET output_lines 0 command_value)
    list(GET output_lines 1 path_value)
    assert_equal("${command_value}" "clang-format-22" "PATH command preservation")
    if(NOT IS_ABSOLUTE "${path_value}")
        message(FATAL_ERROR "Path-shaped formatter override was not canonicalized: ${path_value}")
    endif()
else()
    message(FATAL_ERROR "Unknown TEST_CASE: ${TEST_CASE}")
endif()
