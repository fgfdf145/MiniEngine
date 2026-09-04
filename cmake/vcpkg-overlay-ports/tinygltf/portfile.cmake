# Header-only library
#
# Overlay rationale: the upstream vcpkg port fetches GitHub's auto-generated
# source archive and pins it by SHA512. GitHub re-generated that archive with
# different gzip output, so the recorded hash no longer matches what the server
# serves and the port fails to download. The archive contents were verified to
# be byte-identical to upstream tag v3.0.0 (commit cfcadfa8), so this overlay
# fetches that commit directly instead: git objects are content-addressed and
# cannot drift the way a re-compressed tarball can.
#
# Drop this overlay once the upstream port records a matching hash.
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/syoyo/tinygltf.git
    REF cfcadfa8d14eb489d97b6324838ae100410edcc7
    HEAD_REF master
)

# Put the licence file where vcpkg expects it
# Copy the tinygltf header files and fix the path to json
vcpkg_replace_string("${SOURCE_PATH}/tiny_gltf.h" "#include \"json.hpp\"" "#include <nlohmann/json.hpp>")
file(INSTALL
        "${SOURCE_PATH}/tiny_gltf.h"
        "${SOURCE_PATH}/tiny_gltf_v3.h"
        "${SOURCE_PATH}/tinygltf_json.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include"
)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
