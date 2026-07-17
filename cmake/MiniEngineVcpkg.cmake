include_guard(GLOBAL)

# The toolchain must be selected before project() enables a compiler.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE
            "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH
            "Path to the vcpkg toolchain file"
        )
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.deps/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE
            "${CMAKE_CURRENT_SOURCE_DIR}/.deps/vcpkg/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH
            "Path to the vcpkg toolchain file"
        )
    endif()

endif()

if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT DEFINED VCPKG_INSTALLED_DIR)
    set(_miniengine_vcpkg_install_root "${CMAKE_CURRENT_SOURCE_DIR}/.deps/vcpkg_installed")
    if(DEFINED VCPKG_TARGET_TRIPLET AND NOT "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
        string(APPEND _miniengine_vcpkg_install_root "/${VCPKG_TARGET_TRIPLET}")
    endif()

    set(VCPKG_INSTALLED_DIR
        "${_miniengine_vcpkg_install_root}"
        CACHE PATH
        "Target-isolated vcpkg installed-packages directory"
    )
endif()
