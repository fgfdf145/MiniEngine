#include "format_support.h"

#include <stdexcept>
#include <string>

namespace me
{

VkFormat ChooseFormat(
    std::span<const VkFormat> candidates,
    VkFormatFeatureFlags requiredFeatures,
    const FormatFeatureQuery& query)
{
    if (!query)
    {
        throw std::runtime_error("ChooseFormat requires a format feature query");
    }

    for (const VkFormat candidate : candidates)
    {
        if ((query(candidate) & requiredFeatures) == requiredFeatures)
        {
            return candidate;
        }
    }

    throw std::runtime_error(
        "No candidate format supports the required features (0x" +
        std::to_string(static_cast<uint32_t>(requiredFeatures)) + ")");
}
}
