include_guard(GLOBAL)

# NVIDIA DLSS (the NGX SDK), optional. scripts/fetch-dlss-sdk.sh puts the SDK in .deps/dlss; when it
# is there the renderer builds its DLSS path (MINIENGINE_WITH_DLSS) and the DLSS runtime is copied
# next to the executables that ask for it (miniengine_copy_dlss_runtime). Without it everything
# builds as before and the temporal resolve is the engine's own TAA.
set(MINIENGINE_DLSS_SDK_DIR "${PROJECT_SOURCE_DIR}/.deps/dlss" CACHE PATH
    "NVIDIA DLSS SDK (scripts/fetch-dlss-sdk.sh); empty or missing builds without DLSS")
option(MINIENGINE_ENABLE_DLSS "Build the DLSS upscaler when the SDK is present" ON)

set(MINIENGINE_DLSS_FOUND FALSE)
set(MINIENGINE_DLSS_RUNTIME "")
if(MINIENGINE_ENABLE_DLSS AND EXISTS "${MINIENGINE_DLSS_SDK_DIR}/include/nvsdk_ngx.h")
    set(_dlss_lib_dir "")
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_dlss_lib_dir "${MINIENGINE_DLSS_SDK_DIR}/lib/Windows_x86_64")
        # The /MD libraries: _dbg pairs with the debug CRT the Debug configuration links.
        set(_dlss_release_lib "${_dlss_lib_dir}/x64/nvsdk_ngx_d.lib")
        set(_dlss_debug_lib "${_dlss_lib_dir}/x64/nvsdk_ngx_d_dbg.lib")
        set(_dlss_runtime "${_dlss_lib_dir}/rel/nvngx_dlss.dll")
        set(_dlss_rr_runtime "${_dlss_lib_dir}/rel/nvngx_dlssd.dll")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        set(_dlss_lib_dir "${MINIENGINE_DLSS_SDK_DIR}/lib/Linux_x86_64")
        set(_dlss_release_lib "${_dlss_lib_dir}/libnvsdk_ngx.a")
        set(_dlss_debug_lib "${_dlss_release_lib}")
        file(GLOB _dlss_runtime "${_dlss_lib_dir}/rel/libnvidia-ngx-dlss.so.*")
        file(GLOB _dlss_rr_runtime "${_dlss_lib_dir}/rel/libnvidia-ngx-dlssd.so.*")
    endif()

    if(_dlss_lib_dir AND EXISTS "${_dlss_release_lib}" AND EXISTS "${_dlss_debug_lib}" AND _dlss_runtime AND EXISTS "${_dlss_runtime}")
        add_library(miniengine_ngx INTERFACE)
        target_include_directories(miniengine_ngx SYSTEM INTERFACE "${MINIENGINE_DLSS_SDK_DIR}/include")
        target_link_libraries(miniengine_ngx INTERFACE "$<IF:$<CONFIG:Debug>,${_dlss_debug_lib},${_dlss_release_lib}>")
        if(NOT WIN32)
            target_link_libraries(miniengine_ngx INTERFACE ${CMAKE_DL_LIBS} stdc++)
        endif()
        target_compile_definitions(miniengine_ngx INTERFACE MINIENGINE_WITH_DLSS=1)
        set(MINIENGINE_DLSS_FOUND TRUE)
        set(MINIENGINE_DLSS_RUNTIME "${_dlss_runtime}")
        # Ray reconstruction's runtime, when the SDK fetch brought it (scripts/fetch-dlss-sdk.sh layout 3).
        if(_dlss_rr_runtime AND EXISTS "${_dlss_rr_runtime}")
            list(APPEND MINIENGINE_DLSS_RUNTIME "${_dlss_rr_runtime}")
        endif()
        file(READ "${MINIENGINE_DLSS_SDK_DIR}/VERSION" _dlss_version)
        message(STATUS "DLSS: SDK ${_dlss_version} in ${MINIENGINE_DLSS_SDK_DIR}")
    else()
        message(STATUS "DLSS: SDK in ${MINIENGINE_DLSS_SDK_DIR} has no libraries for this platform; building without it")
    endif()
else()
    message(STATUS "DLSS: no SDK (run scripts/fetch-dlss-sdk.sh); building without it")
endif()

# Copies the DLSS runtimes next to target's executable, where NGX looks for them first.
function(miniengine_copy_dlss_runtime target_name)
    if(MINIENGINE_DLSS_FOUND)
        foreach(_dlss_runtime_file IN LISTS MINIENGINE_DLSS_RUNTIME)
            get_filename_component(_dlss_runtime_name "${_dlss_runtime_file}" NAME)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dlss_runtime_file}"
                    "$<TARGET_FILE_DIR:${target_name}>/${_dlss_runtime_name}"
            )
        endforeach()
    endif()
endfunction()
