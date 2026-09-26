#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace me
{

// What a surface carries beyond the plain base, as bits of its shading flags. Written to GB2.a by
// gbuffer.frag (flags / 255), so they must stay below 256 and match the SHADING_FLAG_* constants
// in shaders/vulkan/gbuffer_common.glsl. The lighting pass reads only the G-buffer targets the
// flags name, so a surface with none of them shades exactly as the plain base always did.
// A clear dielectric coat (KHR_materials_clearcoat): GB6.rg.
inline constexpr uint32_t kShadingFlagClearcoat = 1u;
// Cloth-like sheen (KHR_materials_sheen): GB7.
inline constexpr uint32_t kShadingFlagSheen = 2u;
// An anisotropic base (KHR_materials_anisotropy): GB6.ba.
inline constexpr uint32_t kShadingFlagAnisotropy = 4u;
// A dielectric F0 and F90 other than 0.04 and 1 (KHR_materials_ior, KHR_materials_specular): GB5.
inline constexpr uint32_t kShadingFlagSpecular = 8u;
// A coat with its own normal map: the velocity target's .ba. Without it the coat takes the
// geometric normal.
inline constexpr uint32_t kShadingFlagCoatNormal = 16u;
// Shaded by the forward pass (features with no room in the G-buffer, such as iridescence). The
// geometry pass still writes the surface, so depth, motion, normals and AO stay; the lighting pass
// skips its pixels and the forward pass draws over them.
inline constexpr uint32_t kShadingFlagForward = 32u;
// KHR_materials_unlit: the base colour, shown at the display's paper white, no lighting.
inline constexpr uint32_t kShadingFlagUnlit = 64u;
// KHR_materials_transmission (with _volume): drawn after the scene behind it is copied (the
// transmission copy), which it samples through its surface. Always with kShadingFlagForward, and
// unlike other forward-shaded surfaces it is not in the G-buffer at all.
inline constexpr uint32_t kShadingFlagTransmission = 128u;

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
    // x = kShadingFlag* bits, y = 1 when any texture is transformed or reads the second UV set
    // (the shaders then read GpuTextureTransforms); zw reserved.
    uint32_t shadingModel[4] = {0u, 0u, 0u, 0u};
    // x = clearcoat factor, y = clearcoat perceptual roughness, both [0, 1], z = the coat normal
    // map's scale; w unused. Read only with kShadingFlagClearcoat.
    float clearcoatFactors[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // rgb = sheen colour (linear), a = sheen perceptual roughness. Read only with kShadingFlagSheen.
    float sheenFactors[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // x = anisotropy strength [0, 1], y = cos(rotation), z = sin(rotation); w unused. Read only with
    // kShadingFlagAnisotropy.
    float anisotropyFactors[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    // rgb = the dielectric F0 from the IOR times the specular colour factor (ComputeDielectricF0
    // before its clamp and factor), a = the specular factor. Read only with kShadingFlagSpecular.
    float specularFactors[4] = {0.04f, 0.04f, 0.04f, 1.0f};
    // x = iridescence factor, y = the film's IOR, z = thickness minimum, w = maximum (nm). Read only
    // by the forward pass, which is where a film sends a material (kShadingFlagForward).
    float iridescenceFactors[4] = {0.0f, 1.3f, 100.0f, 400.0f};
    // x = transmission [0, 1], y = volume thickness (mesh units, 0 = thin), z = attenuation distance
    // (metres, 0 = no absorption), w = dispersion (KHR_materials_dispersion, 0 = none). Read only with kShadingFlagTransmission.
    float transmissionFactors[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // rgb = the attenuation colour; a = the IOR the view ray refracts by (KHR_materials_ior; its 0,
    // an infinite index, is stored as a large one).
    float attenuationColor[4] = {1.0f, 1.0f, 1.0f, 1.5f};
    // xyz = the glTF node's scale per axis, which the vertices already carry baked in; the volume's
    // thickness scales by it and the entity's scale. w unused.
    float volumeScale[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    // KHR_materials_diffuse_transmission: rgb = the transmitted light's colour factor, a = the factor.
    // A factor above 0 makes the material forward shaded (kShadingFlagForward); the forward pass
    // reads it, no flag bit (GB2.a keeps the flags below 256).
    float diffuseTransmission[4] = {1.0f, 1.0f, 1.0f, 0.0f};
};

// Every texture slot's KHR_texture_transform for one draw, set 0 binding 17 (material_uv.glsl): per
// slot, in the material set's binding order, two rows, (a, b, tx, UV set) and (c, d, ty, 0), as
// ComputeTextureTransformRows writes them.
inline constexpr uint32_t kGpuTextureTransformSlots = 27;
struct GpuTextureTransforms
{
    float rows[kGpuTextureTransformSlots * 8] = {};
};

static_assert(sizeof(GpuTextureTransforms) == 27 * 32, "GpuTextureTransforms must stay two vec4 per slot");

// The per-draw push constant: only the model matrix. The material moved to the material buffer
// once it outgrew the 128 bytes Vulkan guarantees for push constants.
struct alignas(16) ObjectPushConstants
{
    glm::mat4 model{1.0f};
};

static_assert(sizeof(GpuMaterialData) == 224, "GpuMaterialData must stay 14 x vec4 to match the shader struct");
static_assert(offsetof(GpuMaterialData, diffuseTransmission) == 208, "diffuseTransmission must be the fourteenth vec4");
static_assert(offsetof(GpuMaterialData, emissiveFactor) == 16, "emissiveFactor must start the second vec4");
static_assert(offsetof(GpuMaterialData, alphaCutoff) == 28, "alphaCutoff must stay in the emissive vec4's w component");
static_assert(offsetof(GpuMaterialData, surfaceFactors) == 32, "surfaceFactors must be the third vec4");
static_assert(offsetof(GpuMaterialData, nodeGraphFactors) == 48, "nodeGraphFactors must be the fourth vec4");
static_assert(offsetof(GpuMaterialData, shadingModel) == 64, "shadingModel must be the fifth vec4");
static_assert(offsetof(GpuMaterialData, clearcoatFactors) == 80, "clearcoatFactors must be the sixth vec4");
static_assert(offsetof(GpuMaterialData, sheenFactors) == 96, "sheenFactors must be the seventh vec4");
static_assert(offsetof(GpuMaterialData, anisotropyFactors) == 112, "anisotropyFactors must be the eighth vec4");
static_assert(offsetof(GpuMaterialData, specularFactors) == 128, "specularFactors must be the ninth vec4");
static_assert(offsetof(GpuMaterialData, iridescenceFactors) == 144, "iridescenceFactors must be the tenth vec4");
static_assert(sizeof(ObjectPushConstants) == 64, "ObjectPushConstants must match triangle.vert's push constant block");
}
