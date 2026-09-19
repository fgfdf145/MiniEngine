# bc7enc_rdo has no vcpkg port. The upstream repository ships a sample executable, an ISPC encoder
# and an RDO encoder; the engine needs only the three portable encoders and decoder, built here as
# one static library by the CMakeLists.txt beside this file. Fetched by commit so the sources
# cannot drift.
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/richgel999/bc7enc_rdo.git
    REF b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}/miniengine")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}/miniengine")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-bc7enc-rdo)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
