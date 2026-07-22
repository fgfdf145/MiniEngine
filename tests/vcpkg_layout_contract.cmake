cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED REPO_ROOT OR "${REPO_ROOT}" STREQUAL "")
    message(FATAL_ERROR "REPO_ROOT is required")
endif()

set(case_script "${REPO_ROOT}/tests/vcpkg_layout_case.cmake")

function(run_layout_case case_name triplet install_dir expect_success)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DREPO_ROOT=${REPO_ROOT}"
            "-DCASE_TRIPLET=${triplet}"
            "-DCASE_INSTALL_DIR=${install_dir}"
            -P "${case_script}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output
    )

    if(expect_success AND NOT result EQUAL 0)
        message(FATAL_ERROR
            "${case_name} should pass but failed (${result})\n${output}\n${error_output}")
    endif()
    if(NOT expect_success AND result EQUAL 0)
        message(FATAL_ERROR "${case_name} should fail but passed")
    endif()
endfunction()

function(require_file_contains relative_path expected_text)
    file(READ "${REPO_ROOT}/${relative_path}" contents)
    string(FIND "${contents}" "${expected_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "${relative_path} does not contain: ${expected_text}")
    endif()
endfunction()

function(require_file_not_contains relative_path unexpected_text)
    file(READ "${REPO_ROOT}/${relative_path}" contents)
    string(FIND "${contents}" "${unexpected_text}" match_index)
    if(NOT match_index EQUAL -1)
        message(FATAL_ERROR "${relative_path} unexpectedly contains: ${unexpected_text}")
    endif()
endfunction()

run_layout_case(
    x64_bucket x64-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/x64" TRUE
)
run_layout_case(
    x86_bucket x86-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/x86" TRUE
)
run_layout_case(
    arm64_bucket arm64-osx
    "${REPO_ROOT}/.deps/vcpkg_installed/arm64" TRUE
)
run_layout_case(
    external_root x64-windows
    "${REPO_ROOT}/../miniengine-shared-vcpkg/x64" TRUE
)
run_layout_case(
    repository_root x64-windows
    "${REPO_ROOT}/vcpkg_installed" FALSE
)
run_layout_case(
    build_tree x64-windows
    "${REPO_ROOT}/out/build/layout-contract/vcpkg_installed" FALSE
)
run_layout_case(
    clion_tree x64-windows
    "${REPO_ROOT}/cmake-build-debug/vcpkg_installed" FALSE
)
run_layout_case(
    unsupported_prefix foo-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/foo" FALSE
)
run_layout_case(
    wrong_bucket x64-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/x86" FALSE
)

require_file_contains("CMakePresets.json" ".deps/vcpkg_installed/x64")
require_file_contains("CMakePresets.json" ".deps/vcpkg_installed/x86")
require_file_contains("CMakePresets.json" ".deps/vcpkg_installed/arm64")
require_file_contains("CMakePresets.json" "\"VCPKG_TARGET_TRIPLET\": \"x64-linux\"")
require_file_not_contains(
    "CMakePresets.json"
    "\"VCPKG_INSTALLED_DIR\": \"\${sourceDir}/.deps/vcpkg_installed\""
)
require_file_contains("scripts/bootstrap-deps.ps1" "--x-install-root=")
require_file_contains("scripts/bootstrap-deps.sh" "--x-install-root=")

message(STATUS "MiniEngine vcpkg layout contract passed")
