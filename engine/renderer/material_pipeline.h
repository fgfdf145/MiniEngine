#pragma once

#include <material_graph.h>

#include <cstddef>
#include <span>
#include <vector>

struct MaterialPipelineKey
{
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    bool doubleSided = false;

    bool operator==(const MaterialPipelineKey&) const = default;
};

struct MaterialPipelineState
{
    bool blendEnabled = false;
    bool depthWriteEnabled = true;
    bool alphaMaskEnabled = false;
    bool cullBackFaces = true;
    bool writeAttachmentAlpha = false;
};

struct MaterialDrawSortKey
{
    MaterialPipelineKey pipeline;
    float viewDepth = 0.0f;
};

inline constexpr size_t kMaterialPipelineVariantCount = 6;

MaterialPipelineState GetMaterialPipelineState(MaterialPipelineKey key);
size_t GetMaterialPipelineIndex(MaterialPipelineKey key);
std::vector<size_t> BuildMaterialDrawOrder(std::span<const MaterialDrawSortKey> keys);
