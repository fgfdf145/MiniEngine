cmake_minimum_required(VERSION 3.25)

foreach(required_variable IN ITEMS
        REPO_ROOT
        CASE_SOURCE_ROOT
        CASE_TRIPLET
        CASE_INSTALL_DIR
        CASE_BINARY_ROOT)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${required_variable}")
    endif()
endforeach()

set(VCPKG_TARGET_TRIPLET "${CASE_TRIPLET}")
set(VCPKG_INSTALLED_DIR "${CASE_INSTALL_DIR}")
# Keep direct function cases independent from any repository-local vcpkg
# checkout discovered by the module in script mode.
set(CMAKE_TOOLCHAIN_FILE
    "${REPO_ROOT}/tests/fixtures/toolchains/dummy.cmake")
include("${REPO_ROOT}/cmake/MiniEngineVcpkg.cmake")
miniengine_validate_vcpkg_installed_dir(
    "${CASE_SOURCE_ROOT}"
    "${CASE_BINARY_ROOT}"
    "${CASE_TRIPLET}"
    "${CASE_INSTALL_DIR}"
)
