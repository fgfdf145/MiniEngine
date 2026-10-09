#include "material_draw.h"

#include "parallel_recorder.h"
#include "scene_pass.h"

namespace me
{

void RecordMaterialDrawItems(
    nvrhi::ICommandList* commandList,
    const VulkanPipelineSet& pipelines,
    const MaterialDrawTarget& target,
    std::span<const VulkanDrawItem> drawItems)
{
    if (drawItems.empty())
    {
        return;
    }
    // Set 0 (the camera block and the material buffer) and the push constants' set are the same for
    // every item; set 1 (the material's textures) and the buffers vary. NVRHI binds what changed.
    nvrhi::GraphicsState state;
    state.framebuffer = target.framebuffer;
    state.viewport = target.viewport;
    state.bindings = {target.frameSet, nullptr, target.drawConstantsSet};
    state.vertexBuffers = {nvrhi::VertexBufferBinding().setSlot(0).setOffset(0), nvrhi::VertexBufferBinding().setSlot(1).setOffset(0)};
    state.indexBuffer.setFormat(nvrhi::Format::R32_UINT).setOffset(0);
    for (const VulkanDrawItem& drawItem : drawItems)
    {
        state.pipeline = pipelines.Get(drawItem.pipelineKey);
        state.bindings[1] = drawItem.materialSet;
        state.vertexBuffers[0].setBuffer(drawItem.vertexBuffer);
        state.vertexBuffers[1].setBuffer(drawItem.previousPositionBuffer);
        state.indexBuffer.setBuffer(drawItem.indexBuffer);
        commandList->setGraphicsState(state);
        commandList->setPushConstants(&drawItem.drawConstants, sizeof(drawItem.drawConstants));
        // firstInstance carries the draw slot, which indexes the material and previous model matrix
        // (gl_InstanceIndex / SV_StartInstanceLocation in triangle.vert).
        commandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(drawItem.indexCount).setStartInstanceLocation(drawItem.motionSlot));
    }
}

void RecordMaterialPass(
    nvrhi::ICommandList* commandList,
    VulkanParallelRecorder* recorder,
    const VulkanPipelineSet& pipelines,
    const MaterialDrawTarget& target,
    std::span<const VulkanDrawItem> drawItems,
    const std::function<void(nvrhi::ICommandList* commandList)>& tail)
{
    // Inline: the frame's command list records every draw (parallel recording into command lists of
    // their own comes back with the frame split into several).
    (void)recorder;
    RecordMaterialDrawItems(commandList, pipelines, target, drawItems);
    if (tail)
    {
        tail(commandList);
    }
}
}
