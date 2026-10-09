# Overlay rationale: the engine uses NVRHI's Vulkan backend only
# (docs/design/2026-10-08-nvrhi-backend-design.md). Upstream's port (same commit) builds the D3D11 and
# D3D12 backends on Windows and depends on directx-headers, is missing from this project's
# builtin-baseline, and does not support macOS or x86. With a dynamic triplet NVRHI is one shared
# library that owns vulkan.hpp's dispatcher (initialised in nvrhi::vulkan::createDevice), so the
# engine's own Vulkan headers never have to match NVRHI's.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO NVIDIA-RTX/NVRHI
    REF 54100464714de88a5a5059d25808f5ccb914ad7d
    SHA512 56d5de1cc0840e29d8df976a5fe7b13d676c110ba24c09ff5e0caaa73f4aa56cc78d2ec2c31b1cb8da9f5b099c8b8598410792f8343a77ba928da28ba8146b1f
    HEAD_REF main
    PATCHES
        fix-vcpkg-deps.patch
        # SamplerDesc gains the comparison a shadow sampler makes and min/max LOD clamps: the engine's
        # shadows compare LESS_OR_EQUAL, and glTF's non-mipmapped filters sample level 0 alone
        # (maxLod 0.25), which upstream cannot express.
        sampler-lod-and-comparison.patch
        # A heap can be restricted to the memory types its resources allow (the engine's memory pool
        # picks the type as it did before NVRHI), and gives out its VkDeviceMemory for the resources
        # still bound natively (acceleration structures).
        heap-memory-type-and-native.patch
        # A binding layout can hand out its binding sets from descriptor pools it shares, many sets a
        # pool, instead of one pool per binding set: the engine keeps a binding set per material, tens
        # of thousands on the streamed maps.
        shared-descriptor-pools.patch
        # Binding sets can name an acceleration structure the engine built itself (the ray scene's top
        # levels, with their compaction and batched builds), so the ray scene set can be NVRHI's.
        native-accel-struct.patch
        # Bindless layouts for the ray texture table: a descriptor set of their own choosing, a
        # variable-count last array sized per table (resizeDescriptorTable), and
        # update-unused-while-pending, so a streamed map can write new slots while frames read others.
        bindless-table-set-and-variable-count.patch
)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    set(NVRHI_SHARED ON)
else()
    set(NVRHI_SHARED OFF)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DNVRHI_BUILD_SHARED=${NVRHI_SHARED}
        -DNVRHI_INSTALL=ON
        -DNVRHI_INSTALL_EXPORTS=ON
        -DNVRHI_WITH_VULKAN=ON
        -DNVRHI_WITH_VALIDATION=ON
        -DNVRHI_WITH_DX11=OFF
        -DNVRHI_WITH_DX12=OFF
        -DNVRHI_WITH_NVAPI=OFF
        -DNVRHI_WITH_AFTERMATH=OFF
        -DNVRHI_WITH_RTXMU=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/nvrhi")
vcpkg_copy_pdbs()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
