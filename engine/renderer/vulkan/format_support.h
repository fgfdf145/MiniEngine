#pragma once

// Deliberately not including "common.h"; see render_target_layout.h for why.
#include <vulkan/vulkan.h>

#include <functional>
#include <span>
#include <string_view>

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
//
// label names the target whose selection failed and goes into that message. It is not optional:
// the failure is one line of diagnostic on a startup abort, and "no candidate format" without a
// target name leaves the reader guessing between every target this class selects for.
VkFormat ChooseFormat(
    std::string_view label,
    std::span<const VkFormat> candidates,
    VkFormatFeatureFlags requiredFeatures,
    const FormatFeatureQuery& query);
}
