include_guard(GLOBAL)

function(miniengine_is_vcpkg_toolchain out_var toolchain_file)
    file(TO_CMAKE_PATH "${toolchain_file}" normalized_toolchain)
    if(WIN32)
        string(TOLOWER "${normalized_toolchain}" normalized_toolchain)
    endif()
    if(normalized_toolchain MATCHES
            "(^|/)scripts/buildsystems/vcpkg\\.cmake$")
        set(is_vcpkg TRUE)
    else()
        set(is_vcpkg FALSE)
    endif()
    set(${out_var} "${is_vcpkg}" PARENT_SCOPE)
endfunction()

function(miniengine_path_is_equal_or_below out_var candidate root)
    set(compared_candidate "${candidate}")
    set(compared_root "${root}")
    if(WIN32)
        string(TOLOWER "${compared_candidate}" compared_candidate)
        string(TOLOWER "${compared_root}" compared_root)
    endif()

    if(compared_candidate STREQUAL compared_root)
        set(is_equal_or_below TRUE)
    else()
        string(REGEX REPLACE "/+$" "" compared_root "${compared_root}")
        string(FIND "${compared_candidate}/" "${compared_root}/" prefix_index)
        if(prefix_index EQUAL 0)
            set(is_equal_or_below TRUE)
        else()
            set(is_equal_or_below FALSE)
        endif()
    endif()
    set(${out_var} "${is_equal_or_below}" PARENT_SCOPE)
endfunction()

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
    if(NOT IS_ABSOLUTE "${install_dir}")
        message(FATAL_ERROR
            "MiniEngine rejects relative VCPKG_INSTALLED_DIR '${install_dir}'. "
            "Use the absolute repository-local path '${allowed_repo_dir}' for "
            "triplet '${triplet}', or use an explicit absolute path outside "
            "both the repository and active CMake binary tree."
        )
    endif()

    get_filename_component(source_dir "${source_root}" ABSOLUTE)
    get_filename_component(binary_dir "${binary_root}" ABSOLUTE)
    get_filename_component(
        resolved_install_dir "${install_dir}" ABSOLUTE BASE_DIR "${source_root}"
    )
    file(TO_CMAKE_PATH "${source_dir}" source_dir)
    file(TO_CMAKE_PATH "${binary_dir}" binary_dir)
    file(TO_CMAKE_PATH "${resolved_install_dir}" resolved_install_dir)

    miniengine_path_is_equal_or_below(
        install_is_in_binary "${resolved_install_dir}" "${binary_dir}")
    if(install_is_in_binary)
        message(FATAL_ERROR
            "MiniEngine rejects VCPKG_INSTALLED_DIR '${install_dir}' "
            "(resolved as '${resolved_install_dir}') "
            "because it is equal to or beneath the active CMake binary tree "
            "'${binary_dir}'. Use '${allowed_repo_dir}' for triplet "
            "'${triplet}', or use an explicit path outside both the repository "
            "and active CMake binary tree."
        )
    endif()

    miniengine_path_is_equal_or_below(
        install_is_allowed_repo_dir
        "${resolved_install_dir}"
        "${allowed_repo_dir}"
    )
    miniengine_path_is_equal_or_below(
        allowed_repo_dir_is_install_child
        "${allowed_repo_dir}"
        "${resolved_install_dir}"
    )
    if(install_is_allowed_repo_dir AND allowed_repo_dir_is_install_child)
        return()
    endif()

    miniengine_path_is_equal_or_below(
        install_is_in_source "${resolved_install_dir}" "${source_dir}")
    if(install_is_in_source)
        message(FATAL_ERROR
            "MiniEngine rejects repository-local VCPKG_INSTALLED_DIR "
            "'${install_dir}' (resolved as '${resolved_install_dir}'). "
            "Use '${allowed_repo_dir}' for "
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
    miniengine_is_vcpkg_toolchain(
        miniengine_vcpkg_active "${CMAKE_TOOLCHAIN_FILE}")
else()
    set(miniengine_vcpkg_active FALSE)
endif()

if(miniengine_vcpkg_active)
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
