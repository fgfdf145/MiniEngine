#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace me
{

// Which lighting model shades a surface. Written to GB2.a by gbuffer.frag (id / 255), so it must
// stay below 256 and match the SHADING_MODEL_* constants in shaders/vulkan/gbuffer_common.glsl.
enum class ShadingModel : uint32_t
{
    DefaultLit = 0,
    // A clear dielectric coat over the base (KHR_materials_clearcoat). GB5.rg holds its factor and
    // roughness.
    Clearcoat = 1,
    // Cloth-like sheen over the base (KHR_materials_sheen). GB5.rgb holds its colour, GB5.a its
    // roughness. A material with both a coat and a sheen gets Clearcoat: GB5 holds one layer.
    Sheen = 2
};

// Set on top of the layer in the shading model id when the base is anisotropic
// (KHR_materials_anisotropy); GB5.ba then holds the direction's angle and the strength. Never with
// Sheen, which needs all of GB5. SHADING_MODEL_ANISOTROPY_BIT in gbuffer_common.glsl.
inline constexpr uint32_t kShadingModelAnisotropyBit = 4u;

// One draw's material parameters as the fragment shaders read them from the material buffer (set 0
// binding 12, MaterialData in shaders/vulkan/material_common.glsl), indexed by the draw's slot.
// Every member is a 16-byte multiple, so the C++ layout is the std430 one without alignment macros.
struct alignas(16) GpuMaterialData
{
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissiveFactor[3] = {0.0f, 0.0f, 0.0f};
    // Shares the emissive vec4's fourth component on the GPU (a vec3 followed by a float packs
    // exactly like a vec4). Keep it directly after emissiveFactor: the shader struct declares the
    // pair in this order.
    float alphaCutoff = 0.5f;
    float surfaceFactors[4] = {0.0f, 1.0f, 1.0f, 1.0f};
    float nodeGraphFactors[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    // x = ShadingModel, plus kShadingModelAnisotropyBit for an anisotropic base; yzw reserved.
    uint32_t shadingModel[4] = {static_cast<uint32_t>(ShadingModel::DefaultLit), 0u, 0u, 0u};
    // x = clearcoat factor, y = clearcoat perceptual roughness, both [0, 1]; zw unused. Read only
    // when shadingModel is Clearcoat.
    float clearcoatFactors[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // rgb = sheen colour (linear), a = sheen perceptual roughness. Read only when shadingModel is
    // Sheen.
    float sheenFactors[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // x = anisotropy strength [0, 1], y = cos(rotation), z = sin(rotation); w unused. Read only with
    // kShadingModelAnisotropyBit.
    float anisotropyFactors[4] = {0.0f, 1.0f, 0.0f, 0.0f};
};

// The per-draw push constant: only the model matrix. The material moved to the material buffer
// once it outgrew the 128 bytes Vulkan guarantees for push constants.
struct alignas(16) ObjectPushConstants
{
    glm::mat4 model{1.0f};
};

static_assert(sizeof(GpuMaterialData) == 128, "GpuMaterialData must stay 8 x vec4 to match the shader struct");
static_assert(offsetof(GpuMaterialData, emissiveFactor) == 16, "emissiveFactor must start the second vec4");
static_assert(offsetof(GpuMaterialData, alphaCutoff) == 28, "alphaCutoff must stay in the emissive vec4's w component");
static_assert(offsetof(GpuMaterialData, surfaceFactors) == 32, "surfaceFactors must be the third vec4");
static_assert(offsetof(GpuMaterialData, nodeGraphFactors) == 48, "nodeGraphFactors must be the fourth vec4");
static_assert(offsetof(GpuMaterialData, shadingModel) == 64, "shadingModel must be the fifth vec4");
static_assert(offsetof(GpuMaterialData, clearcoatFactors) == 80, "clearcoatFactors must be the sixth vec4");
static_assert(offsetof(GpuMaterialData, sheenFactors) == 96, "sheenFactors must be the seventh vec4");
static_assert(offsetof(GpuMaterialData, anisotropyFactors) == 112, "anisotropyFactors must be the eighth vec4");
static_assert(sizeof(ObjectPushConstants) == 64, "ObjectPushConstants must match triangle.vert's push constant block");
}
