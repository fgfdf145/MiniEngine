#pragma once

#include <vulkan/vulkan.h>

namespace me
{

// The scene's depth is reverse-Z: the render projection (Camera::GetProjectionMatrix with
// reverseDepth) puts the near plane at depth 1 and the far plane at 0, so D32_SFLOAT's precision,
// densest near 0, goes to the distance where the projection's own is thinnest. Depth attachments
// clear to the far plane and a nearer surface has the greater depth. shaders/vulkan/reverse_depth.glsl
// is the shader side. The shadow maps keep conventional depth: their own projections are short.
constexpr float kReverseDepthNear = 1.0f;
constexpr float kReverseDepthFar = 0.0f;
// Passes a fragment nearer than what the attachment holds.
constexpr VkCompareOp kReverseDepthNearer = VK_COMPARE_OP_GREATER;
// The same, or at the same depth: redrawing a surface already in the buffer.
constexpr VkCompareOp kReverseDepthNearerOrEqual = VK_COMPARE_OP_GREATER_OR_EQUAL;
}
