# Copied into <source>/miniengine by portfile.cmake: builds the portable encoders and the BC7
# decoder of bc7enc_rdo as one static library.
cmake_minimum_required(VERSION 3.20)
project(bc7enc_rdo LANGUAGES CXX)

set(BC7ENC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(bc7enc-rdo STATIC
    "${BC7ENC_ROOT}/bc7enc.cpp"
    "${BC7ENC_ROOT}/rgbcx.cpp"
    "${BC7ENC_ROOT}/bc7decomp.cpp")
target_include_directories(bc7enc-rdo PUBLIC
    "$<BUILD_INTERFACE:${BC7ENC_ROOT}>"
    "$<INSTALL_INTERFACE:include/bc7enc_rdo>")
target_compile_features(bc7enc-rdo PUBLIC cxx_std_11)

# The encoders run for seconds on a 4K texture when optimized and for most of a minute when not,
# which in a Debug editor is long enough for Windows to report it as hung. Optimize the Debug
# library too; it still links against the Debug runtime. MSVC's run-time checks cannot be combined
# with optimization, so they are dropped for this library.
if(MSVC)
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    target_compile_options(bc7enc-rdo PRIVATE "$<$<CONFIG:Debug>:/O2;/Ob2>")
else()
    target_compile_options(bc7enc-rdo PRIVATE "$<$<CONFIG:Debug>:-O2>")
endif()

install(TARGETS bc7enc-rdo EXPORT unofficial-bc7enc-rdo-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
install(FILES
    "${BC7ENC_ROOT}/bc7enc.h"
    "${BC7ENC_ROOT}/rgbcx.h"
    "${BC7ENC_ROOT}/bc7decomp.h"
    DESTINATION include/bc7enc_rdo)
install(EXPORT unofficial-bc7enc-rdo-targets
    NAMESPACE unofficial::bc7enc-rdo::
    DESTINATION share/unofficial-bc7enc-rdo)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/unofficial-bc7enc-rdo-config.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/unofficial-bc7enc-rdo-targets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/unofficial-bc7enc-rdo-config.cmake"
    DESTINATION share/unofficial-bc7enc-rdo)
