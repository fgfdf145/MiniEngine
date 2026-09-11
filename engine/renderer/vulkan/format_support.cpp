#include "format_support.h"

#include <ios>
#include <sstream>
#include <stdexcept>

namespace me
{

VkFormat ChooseFormat(
    std::string_view label,
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

    // Printed as real hex: std::to_string emits decimal, so a required mask of 0x201 under an
    // "0x" prefix would read as 0x513 and match nothing in the Vulkan spec.
    std::ostringstream message;
    message << "No candidate format for the " << label
            << " target supports the required features (0x"
            << std::hex << static_cast<uint32_t>(requiredFeatures) << ")";
    throw std::runtime_error(message.str());
}
}
