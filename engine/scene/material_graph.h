#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace me
{

enum class MaterialAlphaMode
{
    Opaque,
    Mask,
    Blend
};

std::optional<MaterialAlphaMode> ParseMaterialAlphaMode(std::string_view value);

// KHR_materials_ior accepts 0 (an infinite index) or anything from 1 up; values in between, and
// negative ones, are not indices of refraction and read as 1.5, the extension's default.
float SanitizeIor(float ior);

// The material's texture slots, in the order of the material descriptor set's bindings (set 1,
// VulkanUniformBuffer); texture transforms are kept per slot. Slots 6 to 12 are the blend graph's
// layer B and mask, which glTF does not describe and which always sample untransformed.
enum class MaterialTextureSlot : uint32_t
{
    BaseColor = 0,
    Normal = 1,
    Metallic = 2,
    Roughness = 3,
    Occlusion = 4,
    Emissive = 5,
    Clearcoat = 13,
    ClearcoatRoughness = 14,
    SheenColor = 15,
    SheenRoughness = 16,
    Anisotropy = 17,
    Specular = 18,
    SpecularColor = 19,
    ClearcoatNormal = 20,
    Iridescence = 21,
    IridescenceThickness = 22,
    Transmission = 23,
    Thickness = 24,
    DiffuseTransmission = 25,
    DiffuseTransmissionColor = 26
};

inline constexpr uint32_t kMaterialTextureSlotCount = 27;

// The slot's name in a sidecar's texture_transforms, or nullptr for the blend graph's slots.
const char* MaterialTextureSlotName(uint32_t slot);

// KHR_texture_transform, plus the textureInfo's texCoord: which UV set a texture reads, and how that
// UV is moved before sampling, uv' = offset + R(rotation) * (scale * uv) (glTF's T * R * S).
struct TextureTransform
{
    float offset[2] = {0.0f, 0.0f};
    // Radians, as glTF has it.
    float rotation = 0.0f;
    float scale[2] = {1.0f, 1.0f};
    // 0 for TEXCOORD_0, 1 for TEXCOORD_1; the engine reads no others.
    uint32_t texCoord = 0;

    // Samples the first UV set untouched.
    bool IsIdentity() const;
};

using MaterialTextureTransforms = std::array<TextureTransform, kMaterialTextureSlotCount>;

bool AreIdentity(const MaterialTextureTransforms& transforms);

// A glTF sampler (textures[i].sampler): how a texture wraps outside [0, 1] on each axis and how it
// is filtered. The default is the engine's sampler for every texture before samplers were read:
// repeat, linear, linear mipmaps (with anisotropic filtering).
enum class TextureWrap : uint8_t
{
    Repeat,
    ClampToEdge,
    MirroredRepeat
};

enum class TextureFilter : uint8_t
{
    Linear,
    Nearest
};

// How minification moves between mip levels; None samples the base level only (glTF's NEAREST and
// LINEAR minification filters).
enum class TextureMipFilter : uint8_t
{
    Linear,
    Nearest,
    None
};

struct TextureSampler
{
    TextureWrap wrapS = TextureWrap::Repeat;
    TextureWrap wrapT = TextureWrap::Repeat;
    TextureFilter magFilter = TextureFilter::Linear;
    TextureFilter minFilter = TextureFilter::Linear;
    TextureMipFilter mipFilter = TextureMipFilter::Linear;

    bool IsDefault() const
    {
        return *this == TextureSampler{};
    }
    bool operator==(const TextureSampler&) const = default;
};

using MaterialTextureSamplers = std::array<TextureSampler, kMaterialTextureSlotCount>;

// glTF's sampler enums (-1 where the sampler leaves a field out). Unset or unknown values keep the
// default: glTF leaves undefined filtering to the implementation, and wrapping defaults to REPEAT.
TextureSampler TextureSamplerFromGltf(int wrapS, int wrapT, int magFilter, int minFilter);

// The transform as the shader applies it: uv' = (row0.x u + row0.y v + row0.z,
// row1.x u + row1.y v + row1.z), with row0.w the UV set.
void ComputeTextureTransformRows(const TextureTransform& transform, float row0[4], float row1[4]);

// The dielectric F0 KHR_materials_ior and KHR_materials_specular give, before the maps:
// min(((ior - 1) / (ior + 1))^2 * specularColor, 1) * specular. F90 is the specular factor.
void ComputeDielectricF0(float ior, const float specularColor[3], float specular, float f0[3]);
const char* ToString(MaterialAlphaMode mode);
float ClampMaterialAlphaValue(float value);
float ClampMaterialAlphaValue(float value, float fallback);
float ResolveMaterialCoverageAlpha(MaterialAlphaMode mode, float alpha, float cutoff);

struct MaterialPbrSurfaceSettings
{
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissiveColor[3] = {0.0f, 0.0f, 0.0f};
    float metallicFactor = 0.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float emissiveIntensity = 1.0f;
    float opacity = 1.0f;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    // KHR_materials_clearcoat: the coat's weight and its perceptual roughness, both [0, 1]. A
    // factor of 0 means no coat.
    float clearcoatFactor = 0.0f;
    float clearcoatRoughnessFactor = 0.0f;
    // KHR_materials_sheen: the sheen's linear colour and perceptual roughness, all [0, 1]. A black
    // colour means no sheen.
    float sheenColorFactor[3] = {0.0f, 0.0f, 0.0f};
    float sheenRoughnessFactor = 0.0f;
    // KHR_materials_anisotropy: the strength [0, 1] and the direction's rotation in radians,
    // counter-clockwise from the tangent. A strength of 0 means an isotropic base.
    float anisotropyStrength = 0.0f;
    float anisotropyRotation = 0.0f;
    // KHR_materials_ior: the dielectric's index of refraction; 1.5 gives the usual F0 of 0.04, 0
    // stands for an infinite index (F0 = 1).
    float ior = 1.5f;
    // KHR_materials_specular: scales the dielectric's F0 and F90 alike, and tints its F0.
    float specularFactor = 1.0f;
    float specularColorFactor[3] = {1.0f, 1.0f, 1.0f};
    // KHR_materials_clearcoat's clearcoatNormalTexture scale.
    float clearcoatNormalScale = 1.0f;
    // KHR_materials_iridescence: the thin film's weight, index of refraction and thickness range in
    // nanometres (the thickness map picks between them). A factor of 0 means no film.
    float iridescenceFactor = 0.0f;
    float iridescenceIor = 1.3f;
    float iridescenceThicknessMinimum = 100.0f;
    float iridescenceThicknessMaximum = 400.0f;
    // KHR_materials_transmission: the share of the dielectric base's diffuse light replaced by light
    // transmitted through the surface. 0 is opaque.
    float transmissionFactor = 0.0f;
    // KHR_materials_volume: the thickness below the surface in mesh units (0 is a thin wall), and the
    // absorption: white light turns attenuationColor after attenuationDistance metres. An
    // attenuation distance of 0 stands for the extension's infinite default, no absorption.
    float thicknessFactor = 0.0f;
    float attenuationDistance = 0.0f;
    float attenuationColor[3] = {1.0f, 1.0f, 1.0f};
    // KHR_materials_dispersion: how far the refraction's IOR spreads over the three channels
    // ((ior - 1) * 0.025 * dispersion either side of green). 0 disperses nothing.
    float dispersion = 0.0f;
    // KHR_materials_diffuse_transmission: the share of the diffuse lobe that passes through the
    // surface as a Lambertian lobe on its far side, and that light's colour.
    float diffuseTransmissionFactor = 0.0f;
    float diffuseTransmissionColor[3] = {1.0f, 1.0f, 1.0f};
    // KHR_materials_unlit: the base colour alone, no lighting.
    bool unlit = false;
};

struct MaterialGraphNodePosition
{
    float x = 0.0f;
    float y = 0.0f;
};

struct MaterialShaderNodeLayout
{
    MaterialGraphNodePosition primarySurfaceNode{72.0f, 80.0f};
    MaterialGraphNodePosition blendNode{420.0f, 260.0f};
    MaterialGraphNodePosition secondarySurfaceNode{760.0f, 80.0f};
    MaterialGraphNodePosition outputNode{1120.0f, 240.0f};
};

enum class MaterialShaderNodeType : uint32_t
{
    Texture = 0,
    Scalar = 1,
    Color = 2,
    Surface = 3,
    Blend = 4,
    Output = 5
};

struct MaterialShaderNode
{
    uint32_t id = 0;
    MaterialShaderNodeType type = MaterialShaderNodeType::Texture;
    std::string name;
    MaterialGraphNodePosition position;
    float width = 0.0f;
    float height = 0.0f;
    std::string texturePath;
    float scalarValue = 1.0f;
    float colorValue[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    MaterialPbrSurfaceSettings pbr;
};

struct MaterialShaderLink
{
    uint32_t id = 0;
    uint32_t fromNodeId = 0;
    std::string fromSlot;
    uint32_t toNodeId = 0;
    std::string toSlot;
};

struct MaterialShaderGraph
{
    uint32_t nextNodeId = 1;
    uint32_t nextLinkId = 1;
    std::vector<MaterialShaderNode> nodes;
    std::vector<MaterialShaderLink> links;

    bool IsEmpty() const
    {
        return nodes.empty();
    }
};

struct MaterialTextureBlendGraph
{
    bool enabled = false;
    float blendFactor = 0.0f;
    std::string blendMaskTexturePath;
    std::string secondaryBaseColorTexturePath;
    std::string secondaryNormalTexturePath;
    std::string secondaryMetallicTexturePath;
    std::string secondaryRoughnessTexturePath;
    std::string secondaryOcclusionTexturePath;
    std::string secondaryEmissiveTexturePath;
};
}
