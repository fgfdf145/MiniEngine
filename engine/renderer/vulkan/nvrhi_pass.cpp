#include "nvrhi_pass.h"

#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>

#include <stdexcept>
#include <string>

namespace me
{

NvrhiPassScope::NvrhiPassScope(
    nvrhi::ICommandList* commandList,
    std::initializer_list<NvrhiSharedTexture> shared,
    std::initializer_list<NvrhiSharedBuffer> sharedBuffers)
    : m_commandList(commandList),
      m_shared(shared),
      m_sharedBuffers(sharedBuffers)
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
    for (const NvrhiSharedBuffer& buffer : m_sharedBuffers)
    {
        m_commandList->beginTrackingBufferState(buffer.buffer, buffer.state);
    }
}

NvrhiPassScope::~NvrhiPassScope()
{
    // A texture the pass left in the same state gets a barrier all the same (NVRHI's UAV barrier
    // when that state is UnorderedAccess), so its writes are visible to what follows.
    for (const NvrhiSharedTexture& texture : m_shared)
    {
        const nvrhi::ResourceStates exit = texture.exitState != nvrhi::ResourceStates::Unknown ? texture.exitState : texture.state;
        m_commandList->setTextureState(texture.texture, nvrhi::AllSubresources, exit);
    }
    for (const NvrhiSharedBuffer& buffer : m_sharedBuffers)
    {
        m_commandList->setBufferState(buffer.buffer, buffer.exitState != nvrhi::ResourceStates::Unknown ? buffer.exitState : buffer.state);
    }
    m_commandList->commitBarriers();
    m_commandList->clearState();
}

void ClearBufferUInt(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, uint32_t value)
{
    commandList->setEnableAutomaticBarriers(true);
    commandList->clearBufferUInt(buffer, value);
    commandList->setEnableAutomaticBarriers(false);
}

void ClearTextureFloat(nvrhi::ICommandList* commandList, nvrhi::ITexture* texture, const nvrhi::Color& value)
{
    commandList->setEnableAutomaticBarriers(true);
    commandList->clearTextureFloat(texture, nvrhi::AllSubresources, value);
    commandList->setEnableAutomaticBarriers(false);
}

void ClearTextureUInt(nvrhi::ICommandList* commandList, nvrhi::ITexture* texture, uint32_t value)
{
    commandList->setEnableAutomaticBarriers(true);
    commandList->clearTextureUInt(texture, nvrhi::AllSubresources, value);
    commandList->setEnableAutomaticBarriers(false);
}

namespace
{
nvrhi::BufferHandle CreateBufferOrThrow(nvrhi::IDevice* device, const nvrhi::BufferDesc& desc)
{
    nvrhi::BufferHandle buffer = device->createBuffer(desc);
    if (!buffer)
    {
        throw std::runtime_error(std::string("Failed to create the buffer ") + desc.debugName);
    }
    return buffer;
}
}

nvrhi::BufferHandle CreateDeviceBuffer(nvrhi::IDevice* device, uint64_t byteSize, uint32_t stride, bool uav, const char* name)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = byteSize;
    desc.structStride = stride;
    desc.canHaveUAVs = uav;
    desc.canHaveRawViews = true;
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    return CreateBufferOrThrow(device, desc);
}

nvrhi::BufferHandle CreateUploadBuffer(nvrhi::IDevice* device, uint64_t byteSize, uint32_t stride, const char* name, void** mapped)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = byteSize;
    desc.structStride = stride;
    desc.canHaveRawViews = true;
    desc.cpuAccess = nvrhi::CpuAccessMode::Write;
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    nvrhi::BufferHandle buffer = CreateBufferOrThrow(device, desc);
    *mapped = device->mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
    if (*mapped == nullptr)
    {
        throw std::runtime_error(std::string("Failed to map the buffer ") + name);
    }
    return buffer;
}

nvrhi::BufferHandle CreateReadbackBuffer(nvrhi::IDevice* device, uint64_t byteSize, const char* name, void** mapped)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = byteSize;
    desc.cpuAccess = nvrhi::CpuAccessMode::Read;
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    nvrhi::BufferHandle buffer = CreateBufferOrThrow(device, desc);
    *mapped = device->mapBuffer(buffer, nvrhi::CpuAccessMode::Read);
    if (*mapped == nullptr)
    {
        throw std::runtime_error(std::string("Failed to map the buffer ") + name);
    }
    return buffer;
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

nvrhi::ViewportState NativeViewportState(VkExtent2D extent)
{
    const float width = static_cast<float>(extent.width);
    const float height = static_cast<float>(extent.height);
    nvrhi::ViewportState state;
    state.addViewport(nvrhi::Viewport(0.0f, width, height, 0.0f, 0.0f, 1.0f));
    state.addScissorRect(nvrhi::Rect(0, static_cast<int>(extent.width), 0, static_cast<int>(extent.height)));
    return state;
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
