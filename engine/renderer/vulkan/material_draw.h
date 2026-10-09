#pragma once

#include "command.h"
#include "pipeline_set.h"

#include <functional>
#include <span>

namespace me
{

// The state every material draw of one pass shares: its framebuffer and viewport, the frame set
// (set 0), and the draws' push constant layout's set (register space 2).
struct MaterialDrawTarget
{
    nvrhi::IFramebuffer* framebuffer = nullptr;
    nvrhi::ViewportState viewport;
    nvrhi::IBindingSet* frameSet = nullptr;
    nvrhi::IBindingSet* drawConstantsSet = nullptr;
};

// Records material draw items against one pipeline set: per item the pipeline (its variant), the
// vertex and index buffers, set 1 and the push constants, then the indexed draw with the item's draw
// slot as its first instance. The caller has set the targets' states. The geometry and forward passes
// both call this; they differ in framebuffer and in which items they pass, never in how an item is
// drawn.
void RecordMaterialDrawItems(
    nvrhi::ICommandList* commandList,
    const VulkanPipelineSet& pipelines,
    const MaterialDrawTarget& target,
    std::span<const VulkanDrawItem> drawItems);

class VulkanParallelRecorder;

// Below this many draws a pass records inline: the tasks and command lists would cost more than they
// save.
inline constexpr uint32_t kParallelMaterialDraws = 1024;
// Draws per command list when a pass records in parallel.
inline constexpr uint32_t kMaterialDrawsPerSecondary = 512;

// One pass of material draws into target: drawItems with RecordMaterialDrawItems, then `tail` (the
// ground plane, the sky, decals; may be empty).
void RecordMaterialPass(
    nvrhi::ICommandList* commandList,
    VulkanParallelRecorder* recorder,
    const VulkanPipelineSet& pipelines,
    const MaterialDrawTarget& target,
    std::span<const VulkanDrawItem> drawItems,
    const std::function<void(nvrhi::ICommandList* commandList)>& tail);
}
