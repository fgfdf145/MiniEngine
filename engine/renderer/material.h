#pragma once

#include <glm/glm.hpp>

#include <cstddef>

namespace me
{

struct alignas(16) MaterialPushConstants
{
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissiveFactor[3] = {0.0f, 0.0f, 0.0f};
    // Shares the emissive vec4's fourth component on the GPU (a vec3 followed by a float packs
    // exactly like a vec4), so this stays a named field without costing push constant budget --
    // ObjectPushConstants already sits at the 128-byte Vulkan minimum. Keep it directly after
    // emissiveFactor: the shader block declares the pair in this order.
    float alphaCutoff = 0.5f;
    float surfaceFactors[4] = {0.0f, 1.0f, 1.0f, 1.0f};
    float nodeGraphFactors[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};

struct alignas(16) ObjectPushConstants
{
    glm::mat4 model{1.0f};
    MaterialPushConstants material;
};

// Push constants are limited to 128 bytes by the Vulkan minimum guarantee
// (maxPushConstantsSize); this struct sits exactly at that limit.
static_assert(sizeof(MaterialPushConstants) == 64, "MaterialPushConstants must stay 4 x vec4");
// emissiveFactor + alphaCutoff must keep packing into one vec4 so the byte layout still matches the
// shader block. The offsets below are within MaterialPushConstants; the compiled SPIR-V reports the
// same two members at 80 and 92, which is these plus the 64-byte model matrix that precedes them.
static_assert(
    offsetof(MaterialPushConstants, alphaCutoff) == 28,
    "alphaCutoff must stay in the emissive vec4's w component");
static_assert(
    offsetof(MaterialPushConstants, surfaceFactors) == 32,
    "surfaceFactors must stay 16-byte aligned after the emissive vec4");
static_assert(sizeof(ObjectPushConstants) == 128, "ObjectPushConstants must not exceed the 128-byte push constant guarantee");
}
