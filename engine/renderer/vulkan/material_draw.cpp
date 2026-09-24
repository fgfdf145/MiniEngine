#include "material_draw.h"

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
}
