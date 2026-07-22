cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED MINIENGINE_REPO_ROOT OR "${MINIENGINE_REPO_ROOT}" STREQUAL "")
    message(FATAL_ERROR "MINIENGINE_REPO_ROOT is required")
endif()

include("${MINIENGINE_REPO_ROOT}/cmake/MiniEngineVcpkg.cmake")
file(WRITE "${CMAKE_BINARY_DIR}/module-returned-before-project.marker" "unexpected\n")

project(MiniEngineVcpkgConfigureProbe NONE)

file(WRITE "${CMAKE_BINARY_DIR}/project-completed.marker" "ok\n")
