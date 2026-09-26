#include "material_graph.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace me
{

std::optional<MaterialAlphaMode> ParseMaterialAlphaMode(std::string_view value)
{
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character)
                   {
                       return static_cast<char>(std::tolower(character));
                   });
    if (normalized == "opaque")
        return MaterialAlphaMode::Opaque;
    if (normalized == "mask")
        return MaterialAlphaMode::Mask;
    if (normalized == "blend")
        return MaterialAlphaMode::Blend;
    return std::nullopt;
}

const char* ToString(MaterialAlphaMode mode)
{
    switch (mode)
    {
    case MaterialAlphaMode::Mask:
        return "mask";
    case MaterialAlphaMode::Blend:
        return "blend";
    case MaterialAlphaMode::Opaque:
    default:
        return "opaque";
    }
}

float ClampMaterialAlphaValue(float value)
{
    return ClampMaterialAlphaValue(value, 0.0f);
}

float ClampMaterialAlphaValue(float value, float fallback)
{
    const float finiteFallback = std::isfinite(fallback) ? fallback : 0.0f;
    return std::clamp(std::isfinite(value) ? value : finiteFallback, 0.0f, 1.0f);
}

float ResolveMaterialCoverageAlpha(MaterialAlphaMode mode, float alpha, float cutoff)
{
    const float clampedAlpha = ClampMaterialAlphaValue(alpha);
    switch (mode)
    {
    case MaterialAlphaMode::Mask:
        return clampedAlpha < ClampMaterialAlphaValue(cutoff, 0.5f) ? 0.0f : 1.0f;
    case MaterialAlphaMode::Blend:
        return clampedAlpha;
    case MaterialAlphaMode::Opaque:
    default:
        return 1.0f;
    }
}

float SanitizeIor(float ior)
{
    if (!std::isfinite(ior) || (ior != 0.0f && ior < 1.0f))
    {
        return 1.5f;
    }
    return ior;
}

void ComputeDielectricF0(float ior, const float specularColor[3], float specular, float f0[3])
{
    ior = SanitizeIor(ior);
    const float reflectance = ior == 0.0f ? 1.0f : ((ior - 1.0f) / (ior + 1.0f)) * ((ior - 1.0f) / (ior + 1.0f));
    for (size_t index = 0; index < 3; ++index)
    {
        f0[index] = std::min(reflectance * std::max(specularColor[index], 0.0f), 1.0f) * std::clamp(specular, 0.0f, 1.0f);
    }
}

const char* MaterialTextureSlotName(uint32_t slot)
{
    switch (static_cast<MaterialTextureSlot>(slot))
    {
    case MaterialTextureSlot::BaseColor:
        return "base_color";
    case MaterialTextureSlot::Normal:
        return "normal";
    case MaterialTextureSlot::Metallic:
        return "metallic";
    case MaterialTextureSlot::Roughness:
        return "roughness";
    case MaterialTextureSlot::Occlusion:
        return "occlusion";
    case MaterialTextureSlot::Emissive:
        return "emissive";
    case MaterialTextureSlot::Clearcoat:
        return "clearcoat";
    case MaterialTextureSlot::ClearcoatRoughness:
        return "clearcoat_roughness";
    case MaterialTextureSlot::SheenColor:
        return "sheen_color";
    case MaterialTextureSlot::SheenRoughness:
        return "sheen_roughness";
    case MaterialTextureSlot::Anisotropy:
        return "anisotropy";
    case MaterialTextureSlot::Specular:
        return "specular";
    case MaterialTextureSlot::SpecularColor:
        return "specular_color";
    case MaterialTextureSlot::ClearcoatNormal:
        return "clearcoat_normal";
    case MaterialTextureSlot::Iridescence:
        return "iridescence";
    case MaterialTextureSlot::IridescenceThickness:
        return "iridescence_thickness";
    case MaterialTextureSlot::Transmission:
        return "transmission";
    case MaterialTextureSlot::Thickness:
        return "thickness";
    case MaterialTextureSlot::DiffuseTransmission:
        return "diffuse_transmission";
    case MaterialTextureSlot::DiffuseTransmissionColor:
        return "diffuse_transmission_color";
    }
    return nullptr;
}

bool TextureTransform::IsIdentity() const
{
    return offset[0] == 0.0f && offset[1] == 0.0f && rotation == 0.0f && scale[0] == 1.0f && scale[1] == 1.0f && texCoord == 0;
}

TextureSampler TextureSamplerFromGltf(int wrapS, int wrapT, int magFilter, int minFilter)
{
    const auto wrap = [](int value)
    {
        switch (value)
        {
        case 33071:
            return TextureWrap::ClampToEdge;
        case 33648:
            return TextureWrap::MirroredRepeat;
        default:
            return TextureWrap::Repeat;
        }
    };
    TextureSampler sampler;
    sampler.wrapS = wrap(wrapS);
    sampler.wrapT = wrap(wrapT);
    if (magFilter == 9728)
    {
        sampler.magFilter = TextureFilter::Nearest;
    }
    switch (minFilter)
    {
    case 9728: // NEAREST
        sampler.minFilter = TextureFilter::Nearest;
        sampler.mipFilter = TextureMipFilter::None;
        break;
    case 9729: // LINEAR
        sampler.mipFilter = TextureMipFilter::None;
        break;
    case 9984: // NEAREST_MIPMAP_NEAREST
        sampler.minFilter = TextureFilter::Nearest;
        sampler.mipFilter = TextureMipFilter::Nearest;
        break;
    case 9985: // LINEAR_MIPMAP_NEAREST
        sampler.mipFilter = TextureMipFilter::Nearest;
        break;
    case 9986: // NEAREST_MIPMAP_LINEAR
        sampler.minFilter = TextureFilter::Nearest;
        break;
    default: // LINEAR_MIPMAP_LINEAR, unset or unknown
        break;
    }
    return sampler;
}

bool AreIdentity(const MaterialTextureTransforms& transforms)
{
    return std::all_of(transforms.begin(), transforms.end(), [](const TextureTransform& transform)
                       {
                           return transform.IsIdentity();
                       });
}

void ComputeTextureTransformRows(const TextureTransform& transform, float row0[4], float row1[4])
{
    // T * R * S with R = [[cos, sin], [-sin, cos]], as KHR_texture_transform writes it.
    const float c = std::cos(transform.rotation);
    const float s = std::sin(transform.rotation);
    row0[0] = c * transform.scale[0];
    row0[1] = s * transform.scale[1];
    row0[2] = transform.offset[0];
    row0[3] = static_cast<float>(transform.texCoord);
    row1[0] = -s * transform.scale[0];
    row1[1] = c * transform.scale[1];
    row1[2] = transform.offset[1];
    row1[3] = 0.0f;
}
}
