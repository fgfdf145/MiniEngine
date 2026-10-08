#include "nvrhi_pass.h"

#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>

#include <stdexcept>
#include <string>

namespace me
{

NvrhiPassScope::NvrhiPassScope(nvrhi::ICommandList* commandList, std::initializer_list<NvrhiSharedTexture> shared)
    : m_commandList(commandList),
      m_shared(shared)
{
    if (m_commandList == nullptr)
    {
        throw std::runtime_error("An NVRHI pass needs the frame's command list");
    }
    m_commandList->clearState();
    for (const NvrhiSharedTexture& texture : m_shared)
    {
        m_commandList->beginTrackingTextureState(texture.texture, nvrhi::AllSubresources, texture.state);
    }
}

NvrhiPassScope::~NvrhiPassScope()
{
    // A texture the pass left in the same state gets a barrier all the same (NVRHI's UAV barrier
    // when that state is UnorderedAccess), so its writes are visible to what follows.
    for (const NvrhiSharedTexture& texture : m_shared)
    {
        m_commandList->setTextureState(texture.texture, nvrhi::AllSubresources, texture.state);
    }
    m_commandList->commitBarriers();
    m_commandList->clearState();
}

nvrhi::ShaderHandle CreateNvrhiShader(nvrhi::IDevice* device, nvrhi::ShaderType type, const char* shaderName)
{
    const std::vector<char> code = ReadSpirvFile(EnginePaths::ShaderRoot() / shaderName);
    nvrhi::ShaderDesc desc;
    desc.shaderType = type;
    desc.debugName = shaderName;
    desc.entryName = "main";
    nvrhi::ShaderHandle shader = device->createShader(desc, code.data(), code.size());
    if (!shader)
    {
        throw std::runtime_error(std::string("Failed to create the shader ") + shaderName);
    }
    return shader;
}

nvrhi::ComputePipelineHandle CreateNvrhiComputePipeline(
    nvrhi::IDevice* device,
    const char* shaderName,
    std::initializer_list<nvrhi::IBindingLayout*> layouts)
{
    nvrhi::ComputePipelineDesc desc;
    desc.CS = CreateNvrhiShader(device, nvrhi::ShaderType::Compute, shaderName);
    for (nvrhi::IBindingLayout* layout : layouts)
    {
        desc.bindingLayouts.push_back(layout);
    }
    nvrhi::ComputePipelineHandle pipeline = device->createComputePipeline(desc);
    if (!pipeline)
    {
        throw std::runtime_error(std::string("Failed to create the compute pipeline for ") + shaderName);
    }
    return pipeline;
}

nvrhi::VulkanBindingOffsets ShaderBindingOffsets()
{
    return nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setConstantBufferOffset(0)
        .setUnorderedAccessViewOffset(0);
}

nvrhi::BindingLayoutHandle CreateNvrhiBindingLayout(nvrhi::IDevice* device, const nvrhi::BindingLayoutDesc& desc, const char* failureMessage)
{
    nvrhi::BindingLayoutHandle layout = device->createBindingLayout(desc);
    if (!layout)
    {
        throw std::runtime_error(failureMessage);
    }
    return layout;
}

nvrhi::BindingSetHandle CreateNvrhiBindingSet(
    nvrhi::IDevice* device,
    const nvrhi::BindingSetDesc& desc,
    nvrhi::IBindingLayout* layout,
    const char* failureMessage)
{
    nvrhi::BindingSetHandle set = device->createBindingSet(desc, layout);
    if (!set)
    {
        throw std::runtime_error(failureMessage);
    }
    return set;
}
}
