cmake_minimum_required(VERSION 3.25)

foreach(required_variable IN ITEMS REPO_ROOT CASE_TRIPLET CASE_INSTALL_DIR)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${required_variable}")
    endif()
endforeach()

set(VCPKG_TARGET_TRIPLET "${CASE_TRIPLET}")
set(VCPKG_INSTALLED_DIR "${CASE_INSTALL_DIR}")
include("${REPO_ROOT}/cmake/MiniEngineVcpkg.cmake")
miniengine_validate_vcpkg_installed_dir(
    "${REPO_ROOT}"
    "${REPO_ROOT}/out/build/layout-contract"
    "${CASE_TRIPLET}"
    "${CASE_INSTALL_DIR}"
)
