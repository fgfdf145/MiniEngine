cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED REPO_ROOT OR "${REPO_ROOT}" STREQUAL "")
    message(FATAL_ERROR "REPO_ROOT is required")
endif()

get_filename_component(REPO_ROOT "${REPO_ROOT}" ABSOLUTE)
file(TO_CMAKE_PATH "${REPO_ROOT}" REPO_ROOT)

set(case_script "${REPO_ROOT}/tests/vcpkg_layout_case.cmake")
set(configure_probe "${REPO_ROOT}/tests/fixtures/vcpkg-configure-probe")
set(vcpkg_probe_toolchain
    "${REPO_ROOT}/tests/fixtures/vcpkg/scripts/buildsystems/vcpkg.cmake")
set(dummy_toolchain "${REPO_ROOT}/tests/fixtures/toolchains/dummy.cmake")
set_property(GLOBAL PROPERTY MINIENGINE_CONTRACT_FAILURES "")

function(record_failure failure)
    string(REPLACE ";" "," failure "${failure}")
    set_property(GLOBAL APPEND PROPERTY MINIENGINE_CONTRACT_FAILURES "${failure}")
    message(STATUS "CONTRACT FAILURE: ${failure}")
endfunction()

function(assert_equal case_name actual expected)
    if(NOT "${actual}" STREQUAL "${expected}")
        record_failure(
            "${case_name}: expected '${expected}', received '${actual}'")
    endif()
endfunction()

function(assert_contains case_name text expected_text)
    string(FIND "${text}" "${expected_text}" match_index)
    if(match_index EQUAL -1)
        record_failure(
            "${case_name}: output did not contain '${expected_text}'")
    endif()
endfunction()

function(run_layout_case
        case_name triplet install_dir binary_root expect_success
        expect_policy_diagnostic)
    set(case_source_root "${REPO_ROOT}")
    if(ARGC GREATER 6)
        set(case_source_root "${ARGV6}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DREPO_ROOT=${REPO_ROOT}"
            "-DCASE_SOURCE_ROOT=${case_source_root}"
            "-DCASE_TRIPLET=${triplet}"
            "-DCASE_INSTALL_DIR=${install_dir}"
            "-DCASE_BINARY_ROOT=${binary_root}"
            -P "${case_script}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output
    )
    set(combined_output "${output}\n${error_output}")

    if(expect_success AND NOT result EQUAL 0)
        record_failure(
            "${case_name}: should pass but failed (${result}): ${combined_output}")
        return()
    endif()
    if(NOT expect_success AND result EQUAL 0)
        record_failure("${case_name}: should fail but passed")
        return()
    endif()

    if(NOT expect_success AND expect_policy_diagnostic)
        set(allowed_dir "${REPO_ROOT}/.deps/vcpkg_installed/${triplet}")
        string(REGEX REPLACE "-.*$" "" bucket "${triplet}")
        set(allowed_dir "${REPO_ROOT}/.deps/vcpkg_installed/${bucket}")
        assert_contains("${case_name} rejected path diagnostic"
            "${combined_output}" "${install_dir}")
        assert_contains("${case_name} allowed path diagnostic"
            "${combined_output}" "${allowed_dir}")
    endif()
endfunction()

function(to_bash_path out_var windows_path)
    file(TO_CMAKE_PATH "${windows_path}" normalized)
    if(WIN32 AND normalized MATCHES "^([A-Za-z]):/(.*)$")
        string(TOLOWER "${CMAKE_MATCH_1}" drive_letter)
        set(normalized "/${drive_letter}/${CMAKE_MATCH_2}")
    endif()
    set(${out_var} "${normalized}" PARENT_SCOPE)
endfunction()

function(find_configure_preset_index out_var preset_name)
    string(JSON preset_count LENGTH "${preset_json}" configurePresets)
    math(EXPR last_index "${preset_count} - 1")
    foreach(index RANGE 0 ${last_index})
        string(JSON candidate_name GET
            "${preset_json}" configurePresets ${index} name)
        if(candidate_name STREQUAL preset_name)
            set(${out_var} "${index}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(get_configure_preset_cache_value out_var preset_name key)
    find_configure_preset_index(preset_index "${preset_name}")
    if("${preset_index}" STREQUAL "")
        set(${out_var} "__MISSING_PRESET__" PARENT_SCOPE)
        return()
    endif()

    string(JSON direct_value ERROR_VARIABLE direct_error GET
        "${preset_json}" configurePresets ${preset_index} cacheVariables "${key}")
    if(direct_error STREQUAL "NOTFOUND")
        set(${out_var} "${direct_value}" PARENT_SCOPE)
        return()
    endif()

    string(JSON inherits_type ERROR_VARIABLE inherits_error TYPE
        "${preset_json}" configurePresets ${preset_index} inherits)
    if(NOT inherits_error STREQUAL "NOTFOUND")
        set(${out_var} "__MISSING_VALUE__" PARENT_SCOPE)
        return()
    endif()

    set(parent_names "")
    if(inherits_type STREQUAL "STRING")
        string(JSON parent_name GET
            "${preset_json}" configurePresets ${preset_index} inherits)
        list(APPEND parent_names "${parent_name}")
    elseif(inherits_type STREQUAL "ARRAY")
        string(JSON parent_count LENGTH
            "${preset_json}" configurePresets ${preset_index} inherits)
        math(EXPR last_parent "${parent_count} - 1")
        foreach(parent_index RANGE 0 ${last_parent})
            string(JSON parent_name GET
                "${preset_json}" configurePresets ${preset_index}
                inherits ${parent_index})
            list(APPEND parent_names "${parent_name}")
        endforeach()
    endif()

    foreach(parent_name IN LISTS parent_names)
        get_configure_preset_cache_value(
            inherited_value "${parent_name}" "${key}")
        if(NOT inherited_value MATCHES "^__MISSING_")
            set(${out_var} "${inherited_value}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${out_var} "__MISSING_VALUE__" PARENT_SCOPE)
endfunction()

function(assert_configure_preset_mapping preset_name triplet bucket)
    get_configure_preset_cache_value(
        actual_triplet "${preset_name}" VCPKG_TARGET_TRIPLET)
    get_configure_preset_cache_value(
        actual_root "${preset_name}" VCPKG_INSTALLED_DIR)
    assert_equal("${preset_name} triplet" "${actual_triplet}" "${triplet}")
    assert_equal("${preset_name} install root" "${actual_root}"
        "\${sourceDir}/.deps/vcpkg_installed/${bucket}")
endfunction()

function(normalize_output_path out_var raw_output)
    string(STRIP "${raw_output}" normalized)
    file(TO_CMAKE_PATH "${normalized}" normalized)
    if(WIN32 AND normalized MATCHES "^/([A-Za-z])/(.*)$")
        string(TOUPPER "${CMAKE_MATCH_1}" drive_letter)
        set(normalized "${drive_letter}:/${CMAKE_MATCH_2}")
    endif()
    set(${out_var} "${normalized}" PARENT_SCOPE)
endfunction()

function(assert_bootstrap_output
        case_name executable script print_option triplet bucket vcpkg_option)
    set(probe_root "${contract_temp}/${case_name}-vcpkg")
    execute_process(
        COMMAND "${executable}" ${bootstrap_prefix_args} "${script}"
            "${vcpkg_option}" "${probe_root}"
            "${triplet_option}" "${triplet}"
            "${print_option}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output
    )
    if(NOT result EQUAL 0)
        record_failure(
            "${case_name}: print mode failed (${result}): ${output} ${error_output}")
    else()
        normalize_output_path(actual_root "${output}")
        assert_equal("${case_name} root" "${actual_root}"
            "${REPO_ROOT}/.deps/vcpkg_installed/${bucket}")
    endif()
    if(EXISTS "${probe_root}")
        record_failure("${case_name}: print mode created '${probe_root}'")
    endif()
endfunction()

function(assert_build_default case_name host_system host_arch expected_preset)
    set(log_file "${contract_temp}/${case_name}.log")
    file(REMOVE "${log_file}")
    to_bash_path(log_file_shell "${log_file}")
    to_bash_path(build_script_shell "${REPO_ROOT}/scripts/build.sh")
    set(shell_command
        "export PATH='${fake_bin_shell}':\"$PATH\"; export MINIENGINE_TEST_HOST_SYSTEM='${host_system}'; export MINIENGINE_TEST_HOST_ARCH='${host_arch}'; export MINIENGINE_TEST_CMAKE_LOG='${log_file_shell}'; '${build_script_shell}'")
    execute_process(
        COMMAND "${bash_executable}" -c "${shell_command}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output
    )
    if(NOT result EQUAL 0)
        record_failure(
            "${case_name}: build script probe failed (${result}): ${output} ${error_output}")
        return()
    endif()
    assert_contains("${case_name} selected preset" "${output}"
        "[build] Preset: ${expected_preset}")
    if(EXISTS "${log_file}")
        file(READ "${log_file}" cmake_log)
        assert_contains("${case_name} CMake preset" "${cmake_log}"
            "--preset ${expected_preset}")
    else()
        record_failure("${case_name}: fake CMake was not invoked")
    endif()
endfunction()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef contract_id)
if(DEFINED ENV{TEMP} AND NOT "$ENV{TEMP}" STREQUAL "")
    set(temp_parent "$ENV{TEMP}")
elseif(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
    set(temp_parent "$ENV{TMPDIR}")
else()
    set(temp_parent "/tmp")
endif()
file(TO_CMAKE_PATH "${temp_parent}" temp_parent)
set(contract_temp "${temp_parent}/miniengine-vcpkg-contract-${contract_id}")
file(MAKE_DIRECTORY "${contract_temp}/working")

set(default_binary_root "${REPO_ROOT}/out/build/layout-contract")
run_layout_case(x64_bucket x64-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/x64"
    "${default_binary_root}" TRUE FALSE)
run_layout_case(x86_bucket x86-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/x86"
    "${default_binary_root}" TRUE FALSE)
run_layout_case(arm64_bucket arm64-osx
    "${REPO_ROOT}/.deps/vcpkg_installed/arm64"
    "${default_binary_root}" TRUE FALSE)
run_layout_case(external_shared_root x64-windows
    "${contract_temp}/shared/x64"
    "${contract_temp}/build" TRUE FALSE)
run_layout_case(repository_root x64-windows
    "${REPO_ROOT}/vcpkg_installed"
    "${default_binary_root}" FALSE TRUE)
run_layout_case(repository_build_tree x64-windows
    "${REPO_ROOT}/out/build/layout-contract/vcpkg_installed"
    "${default_binary_root}" FALSE TRUE)
run_layout_case(clion_tree x64-windows
    "${REPO_ROOT}/cmake-build-debug/vcpkg_installed"
    "${default_binary_root}" FALSE TRUE)
run_layout_case(external_binary_tree x64-windows
    "${contract_temp}/external-build/vcpkg_installed"
    "${contract_temp}/external-build" FALSE TRUE)
run_layout_case(external_binary_root x64-windows
    "${contract_temp}/external-build"
    "${contract_temp}/external-build" FALSE TRUE)
run_layout_case(wrong_bucket x64-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/x86"
    "${default_binary_root}" FALSE TRUE)
run_layout_case(unsupported_prefix foo-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/foo"
    "${default_binary_root}" FALSE FALSE)
run_layout_case(mixed_case_prefix X64-windows
    "${REPO_ROOT}/.deps/vcpkg_installed/X64"
    "${default_binary_root}" FALSE FALSE)

if(WIN32)
    set(casefold_source "C:/MiniEngineCaseFoldContract")
    set(casefold_allowed "c:/minienginecasefoldcontract/.deps/vcpkg_installed/x64")
    run_layout_case(windows_casefold_allowed x64-windows
        "${casefold_allowed}" "C:/MiniEngineCaseFoldBuild" TRUE FALSE
        "${casefold_source}")
    string(TOUPPER "${contract_temp}/external-build/nested" casefold_binary_install)
    run_layout_case(windows_casefold_binary_tree x64-windows
        "${casefold_binary_install}" "${contract_temp}/external-build"
        FALSE TRUE)
else()
    run_layout_case(posix_case_sensitive_external x64-windows
        "/tmp/minienginecasefoldcontract/.deps/vcpkg_installed/x64"
        "/tmp/MiniEngineCaseFoldBuild" TRUE FALSE
        "/tmp/MiniEngineCaseFoldContract")
endif()

file(READ "${REPO_ROOT}/CMakePresets.json" preset_json)
string(JSON preset_type ERROR_VARIABLE preset_json_error TYPE "${preset_json}")
if(NOT preset_json_error STREQUAL "NOTFOUND" OR NOT preset_type STREQUAL "OBJECT")
    record_failure("CMakePresets.json is not a valid JSON object: ${preset_json_error}")
else()
    assert_configure_preset_mapping(x64-debug x64-windows x64)
    assert_configure_preset_mapping(x64-release x64-windows x64)
    assert_configure_preset_mapping(vs2026-x64 x64-windows x64)
    assert_configure_preset_mapping(x86-debug x86-windows x86)
    assert_configure_preset_mapping(x86-release x86-windows x86)
    assert_configure_preset_mapping(vs2026-x86 x86-windows x86)
    assert_configure_preset_mapping(macos-debug arm64-osx arm64)
    assert_configure_preset_mapping(macos-release arm64-osx arm64)
    assert_configure_preset_mapping(macos-x64-debug x64-osx x64)
    assert_configure_preset_mapping(macos-x64-release x64-osx x64)
    assert_configure_preset_mapping(linux-debug x64-linux x64)
    assert_configure_preset_mapping(linux-arm64-debug arm64-linux arm64)
endif()

set(non_vcpkg_binary "${contract_temp}/non-vcpkg-build")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${configure_probe}"
        -B "${non_vcpkg_binary}"
        "-DMINIENGINE_REPO_ROOT=${REPO_ROOT}"
        "-DCMAKE_TOOLCHAIN_FILE=${dummy_toolchain}"
    WORKING_DIRECTORY "${contract_temp}/working"
    RESULT_VARIABLE non_vcpkg_result
    OUTPUT_VARIABLE non_vcpkg_output
    ERROR_VARIABLE non_vcpkg_error
)
if(NOT non_vcpkg_result EQUAL 0)
    record_failure(
        "non-vcpkg toolchain should pass without VCPKG variables: ${non_vcpkg_output} ${non_vcpkg_error}")
elseif(NOT EXISTS "${non_vcpkg_binary}/project-completed.marker")
    record_failure("non-vcpkg toolchain did not complete project()")
endif()

set(relative_binary "${contract_temp}/relative-root-build")
set(relative_install_root ".deps/vcpkg_installed/x64")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${configure_probe}"
        -B "${relative_binary}"
        "-DMINIENGINE_REPO_ROOT=${REPO_ROOT}"
        "-DCMAKE_TOOLCHAIN_FILE=${vcpkg_probe_toolchain}"
        -DVCPKG_TARGET_TRIPLET=x64-windows
        "-DVCPKG_INSTALLED_DIR=${relative_install_root}"
    WORKING_DIRECTORY "${contract_temp}/working"
    RESULT_VARIABLE relative_result
    OUTPUT_VARIABLE relative_output
    ERROR_VARIABLE relative_error
)
set(relative_combined "${relative_output}\n${relative_error}")
if(relative_result EQUAL 0)
    record_failure("relative VCPKG_INSTALLED_DIR should fail before project()")
else()
    assert_contains("relative root rejected path diagnostic"
        "${relative_combined}" "${relative_install_root}")
    assert_contains("relative root allowed path diagnostic"
        "${relative_combined}"
        "${configure_probe}/.deps/vcpkg_installed/x64")
endif()
if(EXISTS "${relative_binary}/module-returned-before-project.marker")
    record_failure("relative VCPKG_INSTALLED_DIR reached the project() boundary")
endif()

find_program(powershell_executable NAMES pwsh powershell)
if(powershell_executable)
    set(bootstrap_prefix_args -NoProfile -ExecutionPolicy Bypass -File)
    set(triplet_option -Triplet)
    foreach(mapping IN ITEMS x64-windows|x64 x86-windows|x86 arm64-windows|arm64)
        string(REPLACE "|" ";" mapping_parts "${mapping}")
        list(GET mapping_parts 0 triplet)
        list(GET mapping_parts 1 bucket)
        assert_bootstrap_output(
            "powershell-${bucket}" "${powershell_executable}"
            "${REPO_ROOT}/scripts/bootstrap-deps.ps1"
            -PrintInstallRoot "${triplet}" "${bucket}" -VcpkgRoot)
    endforeach()
    set(mixed_case_root "${REPO_ROOT}/.deps/vcpkg_installed/X64")
    set(mixed_case_existed FALSE)
    if(EXISTS "${mixed_case_root}")
        set(mixed_case_existed TRUE)
    endif()
    execute_process(
        COMMAND "${powershell_executable}"
            -NoProfile -ExecutionPolicy Bypass -File
            "${REPO_ROOT}/scripts/bootstrap-deps.ps1"
            -Triplet X64-windows -PrintInstallRoot
        RESULT_VARIABLE powershell_mixed_result
        OUTPUT_VARIABLE powershell_mixed_output
        ERROR_VARIABLE powershell_mixed_error
    )
    if(powershell_mixed_result EQUAL 0)
        record_failure("PowerShell mixed-case triplet should fail")
    else()
        assert_contains("PowerShell mixed-case diagnostic"
            "${powershell_mixed_output}\n${powershell_mixed_error}"
            "Unsupported vcpkg triplet architecture: 'X64-windows'")
    endif()
    if(NOT mixed_case_existed AND EXISTS "${mixed_case_root}")
        record_failure("PowerShell mixed-case triplet created '${mixed_case_root}'")
    endif()
elseif(WIN32)
    record_failure("PowerShell executable was not found")
endif()

if(WIN32 AND EXISTS "C:/Program Files/Git/bin/bash.exe")
    set(bash_executable "C:/Program Files/Git/bin/bash.exe")
else()
    find_program(bash_executable NAMES bash)
endif()
if(bash_executable)
    set(bootstrap_prefix_args "")
    set(triplet_option --triplet)
    foreach(mapping IN ITEMS x64-linux|x64 x86-linux|x86 arm64-linux|arm64)
        string(REPLACE "|" ";" mapping_parts "${mapping}")
        list(GET mapping_parts 0 triplet)
        list(GET mapping_parts 1 bucket)
        assert_bootstrap_output(
            "bash-${bucket}" "${bash_executable}"
            "${REPO_ROOT}/scripts/bootstrap-deps.sh"
            --print-install-root "${triplet}" "${bucket}" --vcpkg-root)
    endforeach()
    set(bash_mixed_root "${REPO_ROOT}/.deps/vcpkg_installed/X64")
    set(bash_mixed_existed FALSE)
    if(EXISTS "${bash_mixed_root}")
        set(bash_mixed_existed TRUE)
    endif()
    execute_process(
        COMMAND "${bash_executable}"
            "${REPO_ROOT}/scripts/bootstrap-deps.sh"
            --triplet X64-linux --print-install-root
        RESULT_VARIABLE bash_mixed_result
        OUTPUT_VARIABLE bash_mixed_output
        ERROR_VARIABLE bash_mixed_error
    )
    if(bash_mixed_result EQUAL 0)
        record_failure("Bash mixed-case triplet should fail")
    else()
        assert_contains("Bash mixed-case diagnostic"
            "${bash_mixed_output}\n${bash_mixed_error}"
            "Unsupported vcpkg triplet architecture: 'X64-linux'")
    endif()
    if(NOT bash_mixed_existed AND EXISTS "${bash_mixed_root}")
        record_failure("Bash mixed-case triplet created '${bash_mixed_root}'")
    endif()

    set(fake_bin "${contract_temp}/fake-bin")
    file(MAKE_DIRECTORY "${fake_bin}")
    to_bash_path(fake_bin_shell "${fake_bin}")
    file(WRITE "${fake_bin}/uname" [=[#!/usr/bin/env bash
case "${1:-}" in
  -s) printf '%s\n' "$MINIENGINE_TEST_HOST_SYSTEM" ;;
  -m) printf '%s\n' "$MINIENGINE_TEST_HOST_ARCH" ;;
  *) exit 2 ;;
esac
]=])
    file(WRITE "${fake_bin}/cmake" [=[#!/usr/bin/env bash
printf '%s\n' "$*" >> "$MINIENGINE_TEST_CMAKE_LOG"
]=])
    file(CHMOD "${fake_bin}/uname" "${fake_bin}/cmake"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    assert_build_default(linux-x64 Linux x86_64 linux-debug)
    assert_build_default(linux-arm64 Linux aarch64 linux-arm64-debug)
    assert_build_default(macos-arm64 Darwin arm64 macos-debug)
    assert_build_default(macos-x64 Darwin x86_64 macos-x64-debug)
else()
    record_failure("Bash executable was not found")
endif()

file(REMOVE_RECURSE "${contract_temp}")

get_property(contract_failures GLOBAL PROPERTY MINIENGINE_CONTRACT_FAILURES)
if(contract_failures)
    string(JOIN "\n- " formatted_failures ${contract_failures})
    message(FATAL_ERROR
        "MiniEngine vcpkg layout contract failed:\n- ${formatted_failures}")
endif()

message(STATUS "MiniEngine vcpkg layout contract passed")
