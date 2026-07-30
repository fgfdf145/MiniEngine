#include "material_graph.h"

#include <algorithm>
#include <cctype>
#include <string>

std::optional<MaterialAlphaMode> ParseMaterialAlphaMode(std::string_view value)
{
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    if (normalized == "opaque") return MaterialAlphaMode::Opaque;
    if (normalized == "mask") return MaterialAlphaMode::Mask;
    if (normalized == "blend") return MaterialAlphaMode::Blend;
    return std::nullopt;
}

const char* ToString(MaterialAlphaMode mode)
{
    switch (mode)
    {
    case MaterialAlphaMode::Mask: return "mask";
    case MaterialAlphaMode::Blend: return "blend";
    case MaterialAlphaMode::Opaque:
    default: return "opaque";
    }
}

float ClampMaterialAlphaValue(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

float ResolveMaterialCoverageAlpha(MaterialAlphaMode mode, float alpha, float cutoff)
{
    const float clampedAlpha = ClampMaterialAlphaValue(alpha);
    switch (mode)
    {
    case MaterialAlphaMode::Mask:
        return clampedAlpha < ClampMaterialAlphaValue(cutoff) ? 0.0f : 1.0f;
    case MaterialAlphaMode::Blend:
        return clampedAlpha;
    case MaterialAlphaMode::Opaque:
    default:
        return 1.0f;
    }
}
