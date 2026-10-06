#include "material_draw.h"

#include "parallel_recorder.h"
#include "scene_pass.h"

#include <array>

namespace me
{

void RecordMaterialDrawItems(
    VkCommandBuffer commandBuffer,
    const VulkanPipelineSet& pipelines,
    VkDescriptorSet frameDescriptorSet,
    std::span<const VulkanDrawItem> drawItems)
{
    if (drawItems.empty())
    {
        return;
    }

    const VkPipelineLayout pipelineLayout = pipelines.GetLayout();
    VkPipeline boundPipeline = VK_NULL_HANDLE;

    // Set 0 (the camera uniform buffer and the material buffer) is the same for every item, so it
    // is bound once rather than per item. Set 1 (the material samplers) varies per item and is
    // bound in the loop.
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout,
        0,
        1,
        &frameDescriptorSet,
        0,
        nullptr);

    for (const VulkanDrawItem& drawItem : drawItems)
    {
        const VkPipeline requiredPipeline = pipelines.Get(drawItem.pipelineKey);
        if (requiredPipeline != boundPipeline)
        {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, requiredPipeline);
            boundPipeline = requiredPipeline;
        }

        const VkBuffer vertexBuffers[] = {drawItem.vertexBuffer};
        const VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, drawItem.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout,
            1,
            1,
            &drawItem.descriptorSet,
            0,
            nullptr);
        vkCmdPushConstants(
            commandBuffer,
            pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(ObjectPushConstants),
            &drawItem.drawConstants);
        vkCmdDrawIndexed(commandBuffer, drawItem.indexCount, 1, 0, 0, drawItem.motionSlot);
    }
}

void RecordMaterialPass(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo& renderPassInfo,
    VulkanParallelRecorder* recorder,
    const VulkanPipelineSet& pipelines,
    VkDescriptorSet frameDescriptorSet,
    std::span<const VulkanDrawItem> drawItems,
    const std::function<void(VkCommandBuffer commandBuffer)>& tail)
{
    const VkExtent2D extent = renderPassInfo.renderArea.extent;
    if (recorder == nullptr || drawItems.size() < kParallelMaterialDraws)
    {
        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        SetViewportAndScissor(commandBuffer, extent);
        RecordMaterialDrawItems(commandBuffer, pipelines, frameDescriptorSet, drawItems);
        if (tail)
        {
            tail(commandBuffer);
        }
        vkCmdEndRenderPass(commandBuffer);
        return;
    }

    // A secondary inherits no state: each sets its viewport and binds what it draws with.
    std::array<VulkanParallelRecorder::Batch, 2> batches;
    batches[0].renderPass = renderPassInfo.renderPass;
    batches[0].framebuffer = renderPassInfo.framebuffer;
    batches[0].itemCount = static_cast<uint32_t>(drawItems.size());
    batches[0].record = [&](VkCommandBuffer secondary, uint32_t begin, uint32_t end)
    {
        SetViewportAndScissor(secondary, extent);
        RecordMaterialDrawItems(secondary, pipelines, frameDescriptorSet, drawItems.subspan(begin, end - begin));
    };
    batches[1].renderPass = renderPassInfo.renderPass;
    batches[1].framebuffer = renderPassInfo.framebuffer;
    batches[1].itemCount = tail ? 1u : 0u;
    batches[1].record = [&](VkCommandBuffer secondary, uint32_t, uint32_t)
    {
        SetViewportAndScissor(secondary, extent);
        tail(secondary);
    };
    recorder->Record(batches, kMaterialDrawsPerSecondary);

    std::vector<VkCommandBuffer> secondaries;
    for (const VulkanParallelRecorder::Batch& batch : batches)
    {
        secondaries.insert(secondaries.end(), batch.buffers.begin(), batch.buffers.end());
    }
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdExecuteCommands(commandBuffer, static_cast<uint32_t>(secondaries.size()), secondaries.data());
    vkCmdEndRenderPass(commandBuffer);
}
}
