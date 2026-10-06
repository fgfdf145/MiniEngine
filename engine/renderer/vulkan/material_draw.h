#pragma once

#include "command.h"
#include "pipeline_set.h"

#include <functional>
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

class VulkanParallelRecorder;

// Below this many draws a pass records inline: the tasks and secondary command buffers would cost
// more than they save.
inline constexpr uint32_t kParallelMaterialDraws = 1024;
// Draws per secondary command buffer when a pass records in parallel.
inline constexpr uint32_t kMaterialDrawsPerSecondary = 512;

// One render pass of material draws: begins it, sets the viewport, records drawItems with
// RecordMaterialDrawItems, then `tail` (the ground plane, the sky, decals; may be empty), and ends
// it. With a recorder and at least kParallelMaterialDraws draws they are recorded on the task
// system into secondary command buffers, which the pass executes in order.
void RecordMaterialPass(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo& renderPassInfo,
    VulkanParallelRecorder* recorder,
    const VulkanPipelineSet& pipelines,
    VkDescriptorSet frameDescriptorSet,
    std::span<const VulkanDrawItem> drawItems,
    const std::function<void(VkCommandBuffer commandBuffer)>& tail);
}
