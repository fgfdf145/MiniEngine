include_guard(GLOBAL)

function(miniengine_vcpkg_architecture_bucket out_var triplet)
    if("${triplet}" MATCHES "^x64-")
        set(bucket "x64")
    elseif("${triplet}" MATCHES "^x86-")
        set(bucket "x86")
    elseif("${triplet}" MATCHES "^arm64-")
        set(bucket "arm64")
    else()
        message(FATAL_ERROR
            "Unsupported vcpkg triplet architecture: '${triplet}'. "
            "Expected an x64-, x86-, or arm64- prefix."
        )
    endif()
    set(${out_var} "${bucket}" PARENT_SCOPE)
endfunction()

function(miniengine_vcpkg_default_installed_dir out_var source_root triplet)
    miniengine_vcpkg_architecture_bucket(bucket "${triplet}")
    get_filename_component(
        installed_dir
        "${source_root}/.deps/vcpkg_installed/${bucket}"
        ABSOLUTE
    )
    file(TO_CMAKE_PATH "${installed_dir}" installed_dir)
    set(${out_var} "${installed_dir}" PARENT_SCOPE)
endfunction()

function(miniengine_validate_vcpkg_installed_dir source_root binary_root triplet install_dir)
    miniengine_vcpkg_default_installed_dir(
        allowed_repo_dir "${source_root}" "${triplet}"
    )
    get_filename_component(source_dir "${source_root}" ABSOLUTE)
    get_filename_component(
        resolved_install_dir "${install_dir}" ABSOLUTE BASE_DIR "${source_root}"
    )
    file(TO_CMAKE_PATH "${source_dir}" source_dir)
    file(TO_CMAKE_PATH "${resolved_install_dir}" resolved_install_dir)

    if(resolved_install_dir STREQUAL allowed_repo_dir)
        return()
    endif()

    string(TOLOWER "${source_dir}/" source_prefix)
    string(TOLOWER "${resolved_install_dir}/" install_prefix)
    string(FIND "${install_prefix}" "${source_prefix}" source_prefix_index)
    if(source_prefix_index EQUAL 0)
        message(FATAL_ERROR
            "MiniEngine rejects repository-local VCPKG_INSTALLED_DIR "
            "'${resolved_install_dir}'. Use '${allowed_repo_dir}' for "
            "triplet '${triplet}', or use an explicit path outside the repository."
        )
    endif()
endfunction()

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

if(DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED VCPKG_TARGET_TRIPLET OR "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
        message(FATAL_ERROR
            "MiniEngine requires VCPKG_TARGET_TRIPLET when the vcpkg toolchain is enabled."
        )
    endif()

    miniengine_vcpkg_default_installed_dir(
        default_installed_dir
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${VCPKG_TARGET_TRIPLET}"
    )
    if(NOT DEFINED VCPKG_INSTALLED_DIR OR "${VCPKG_INSTALLED_DIR}" STREQUAL "")
        set(VCPKG_INSTALLED_DIR
            "${default_installed_dir}"
            CACHE PATH
            "Architecture-isolated vcpkg installed-packages directory"
        )
    endif()

    miniengine_validate_vcpkg_installed_dir(
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}"
        "${VCPKG_TARGET_TRIPLET}"
        "${VCPKG_INSTALLED_DIR}"
    )
endif()
