set(MINIENGINE_HAS_FBX_SDK OFF)

if(NOT WIN32)
    return()
endif()

if(NOT DEFINED MINIENGINE_FBX_SDK_ROOT OR MINIENGINE_FBX_SDK_ROOT STREQUAL "")
    message(STATUS "FBX SDK root is not configured; .fbx files will use the Assimp fallback path.")
    return()
endif()

set(_miniengine_fbx_sdk_root "${MINIENGINE_FBX_SDK_ROOT}")
set(_miniengine_fbx_include_dir "${_miniengine_fbx_sdk_root}/include")

if(NOT EXISTS "${_miniengine_fbx_include_dir}/fbxsdk.h")
    message(WARNING "FBX SDK headers were not found under '${_miniengine_fbx_sdk_root}'. FBX SDK support will be disabled.")
    return()
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "The Autodesk FBX SDK installed on this machine only provides 64-bit libraries. FBX SDK support will be disabled for this build.")
    return()
endif()

set(_miniengine_fbx_arch "x64")
string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _miniengine_generator_platform_lower)
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _miniengine_system_processor_lower)
if(_miniengine_generator_platform_lower MATCHES "arm64" OR _miniengine_system_processor_lower MATCHES "arm64|aarch64")
    set(_miniengine_fbx_arch "arm64")
endif()

set(_miniengine_fbx_debug_implib "${_miniengine_fbx_sdk_root}/lib/${_miniengine_fbx_arch}/debug/libfbxsdk.lib")
set(_miniengine_fbx_release_implib "${_miniengine_fbx_sdk_root}/lib/${_miniengine_fbx_arch}/release/libfbxsdk.lib")
set(_miniengine_fbx_debug_dll "${_miniengine_fbx_sdk_root}/lib/${_miniengine_fbx_arch}/debug/libfbxsdk.dll")
set(_miniengine_fbx_release_dll "${_miniengine_fbx_sdk_root}/lib/${_miniengine_fbx_arch}/release/libfbxsdk.dll")

if(NOT EXISTS "${_miniengine_fbx_debug_implib}" OR NOT EXISTS "${_miniengine_fbx_release_implib}")
    message(WARNING "FBX SDK import libraries were not found for architecture '${_miniengine_fbx_arch}'. FBX SDK support will be disabled.")
    return()
endif()

if(TARGET FbxSdk::sdk)
    set(MINIENGINE_HAS_FBX_SDK ON)
    return()
endif()

add_library(FbxSdk::sdk SHARED IMPORTED GLOBAL)
set_target_properties(FbxSdk::sdk PROPERTIES
    IMPORTED_CONFIGURATIONS "Debug;Release"
    IMPORTED_IMPLIB_DEBUG "${_miniengine_fbx_debug_implib}"
    IMPORTED_IMPLIB_RELEASE "${_miniengine_fbx_release_implib}"
    IMPORTED_LOCATION_DEBUG "${_miniengine_fbx_debug_dll}"
    IMPORTED_LOCATION_RELEASE "${_miniengine_fbx_release_dll}"
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
    MAP_IMPORTED_CONFIG_MINSIZEREL Release
    INTERFACE_COMPILE_DEFINITIONS "FBXSDK_SHARED"
    INTERFACE_INCLUDE_DIRECTORIES "${_miniengine_fbx_include_dir}"
)

set(MINIENGINE_HAS_FBX_SDK ON)
message(STATUS "FBX SDK support enabled from '${_miniengine_fbx_sdk_root}' for architecture '${_miniengine_fbx_arch}'.")
