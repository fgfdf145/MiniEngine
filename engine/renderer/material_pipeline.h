#pragma once

#include <engine/scene/material_graph.h>

#include <cstddef>
#include <span>
#include <vector>

namespace me
{

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
};

struct MaterialDrawSortKey
{
    MaterialPipelineKey pipeline;
    float viewDepth = 0.0f;
    // An Opaque or Mask draw the forward pass shades (kShadingFlagForward). Ignored for Blend, which
    // the forward pass always shades.
    bool forwardShaded = false;
    // A transmissive Opaque or Mask draw (kShadingFlagTransmission): drawn after the transmission
    // copy, back to front, never in the G-buffer. Ignored for Blend, which is drawn after it anyway.
    bool transmissive = false;
};

inline constexpr size_t kMaterialPipelineVariantCount = 6;

MaterialPipelineState GetMaterialPipelineState(MaterialPipelineKey key);
size_t GetMaterialPipelineIndex(MaterialPipelineKey key);
std::vector<size_t> BuildMaterialDrawOrder(std::span<const MaterialDrawSortKey> keys);
}
