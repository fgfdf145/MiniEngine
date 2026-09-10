#pragma once

// Deliberately not including "common.h"; see render_target_layout.h for why.
#include <vulkan/vulkan.h>

#include <functional>
#include <span>

namespace me
{

// Returns the optimal-tiling format features of one format. Injected so ChooseFormat can be
// tested without a physical device; the production caller wraps
// vkGetPhysicalDeviceFormatProperties.
using FormatFeatureQuery = std::function<VkFormatFeatureFlags(VkFormat)>;

// Returns the first candidate whose optimal-tiling features include every bit in
// requiredFeatures. Throws std::runtime_error when no candidate qualifies rather than returning
// VK_FORMAT_UNDEFINED, so a missing format is a startup failure with a message instead of a
// confusing image creation error later.
VkFormat ChooseFormat(
    std::span<const VkFormat> candidates,
    VkFormatFeatureFlags requiredFeatures,
    const FormatFeatureQuery& query);
}
