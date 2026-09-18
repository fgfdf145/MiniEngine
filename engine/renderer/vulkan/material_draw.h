#pragma once

#include "command.h"
#include "pipeline_set.h"

#include <span>

namespace me
{

// Records material draw items against one pipeline set: set 0 bound once, then per item the
// pipeline (only when the variant changes), the vertex and index buffers, set 1 and the push
// constants. The caller owns the render pass and the viewport. The geometry and forward passes
// both call this; they differ in render pass and in which items they pass, never in how an item
// is drawn.
void RecordMaterialDrawItems(
    VkCommandBuffer commandBuffer,
    const VulkanPipelineSet& pipelines,
    VkDescriptorSet frameDescriptorSet,
    std::span<const VulkanDrawItem> drawItems);
}
