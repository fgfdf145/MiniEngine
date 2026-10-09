#include "nvrhi_pass.h"

#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>

#include <algorithm>
#include <mutex>
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
        if (texture.state != nvrhi::ResourceStates::Common && texture.state != nvrhi::ResourceStates::Unknown)
        {
            m_commandList->setTextureState(texture.texture, nvrhi::AllSubresources, texture.state);
        }
    }
    for (const NvrhiSharedBuffer& buffer : m_sharedBuffers)
    {
        if (buffer.state != nvrhi::ResourceStates::Common && buffer.state != nvrhi::ResourceStates::Unknown)
        {
            m_commandList->setBufferState(buffer.buffer, buffer.state);
        }
    }
    m_commandList->commitBarriers();
}

NvrhiPassScope::NvrhiPassScope(nvrhi::ICommandList* commandList, std::span<const NvrhiSharedTexture> shared)
    : m_commandList(commandList),
      m_shared(shared.begin(), shared.end())
{
    if (m_commandList == nullptr)
    {
        throw std::runtime_error("An NVRHI pass needs the frame's command list");
    }
    m_commandList->clearState();
    for (const NvrhiSharedTexture& texture : m_shared)
    {
        if (texture.state != nvrhi::ResourceStates::Common && texture.state != nvrhi::ResourceStates::Unknown)
        {
            m_commandList->setTextureState(texture.texture, nvrhi::AllSubresources, texture.state);
        }
    }
    m_commandList->commitBarriers();
}

NvrhiPassScope::~NvrhiPassScope()
{
    // A texture the pass left in the same state gets a barrier all the same (NVRHI's UAV barrier
    // when that state is UnorderedAccess), so its writes are visible to what follows.
    for (const NvrhiSharedTexture& texture : m_shared)
    {
        const nvrhi::ResourceStates exit = texture.exitState != nvrhi::ResourceStates::Unknown ? texture.exitState : texture.state;
        if (exit != nvrhi::ResourceStates::Common && exit != nvrhi::ResourceStates::Unknown)
        {
            m_commandList->setTextureState(texture.texture, nvrhi::AllSubresources, exit);
        }
    }
    for (const NvrhiSharedBuffer& buffer : m_sharedBuffers)
    {
        const nvrhi::ResourceStates exit = buffer.exitState != nvrhi::ResourceStates::Unknown ? buffer.exitState : buffer.state;
        if (exit != nvrhi::ResourceStates::Common && exit != nvrhi::ResourceStates::Unknown)
        {
            m_commandList->setBufferState(buffer.buffer, exit);
        }
    }
    m_commandList->commitBarriers();
    m_commandList->clearState();
}

namespace
{
std::mutex g_initialTransitionsMutex;
std::vector<nvrhi::TextureHandle> g_initialTransitions;
}

void RegisterInitialTransition(nvrhi::ITexture* texture)
{
    const nvrhi::TextureDesc& desc = texture->getDesc();
    if (!desc.keepInitialState || desc.initialState == nvrhi::ResourceStates::Common || desc.initialState == nvrhi::ResourceStates::Unknown)
    {
        return;
    }
    const std::lock_guard lock(g_initialTransitionsMutex);
    g_initialTransitions.emplace_back(texture);
}

void RecordInitialTransitions(nvrhi::ICommandList* commandList)
{
    std::vector<nvrhi::TextureHandle> textures;
    {
        const std::lock_guard lock(g_initialTransitionsMutex);
        textures.swap(g_initialTransitions);
    }
    if (textures.empty())
    {
        return;
    }
    // NVRHI starts a texture it has never moved in Common (UNDEFINED); one moved already is in its
    // initial state, and this is no barrier at all.
    for (const nvrhi::TextureHandle& texture : textures)
    {
        commandList->setTextureState(texture, nvrhi::AllSubresources, texture->getDesc().initialState);
    }
    commandList->commitBarriers();
}

void DropInitialTransitions()
{
    const std::lock_guard lock(g_initialTransitionsMutex);
    g_initialTransitions.clear();
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

void ClearDepth(nvrhi::ICommandList* commandList, nvrhi::ITexture* texture, float depth)
{
    commandList->setEnableAutomaticBarriers(true);
    commandList->clearDepthStencilTexture(texture, nvrhi::AllSubresources, true, depth, false, 0);
    commandList->setEnableAutomaticBarriers(false);
}

nvrhi::ShaderHandle SpecializeShader(
    nvrhi::IDevice* device, nvrhi::IShader* shader, std::span<const nvrhi::ShaderSpecialization> constants, const char* name)
{
    nvrhi::ShaderHandle specialized = device->createShaderSpecialization(shader, constants.data(), static_cast<uint32_t>(constants.size()));
    if (!specialized)
    {
        throw std::runtime_error(std::string("Failed to specialise the shader ") + name);
    }
    return specialized;
}

PushConstantLayout CreatePushConstantLayout(nvrhi::IDevice* device, uint32_t registerSpace, uint32_t size, nvrhi::ShaderType visibility)
{
    PushConstantLayout result;
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = visibility;
    desc.registerSpace = registerSpace;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    desc.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, size)};
    result.layout = CreateNvrhiBindingLayout(device, desc, "Failed to create a push constant layout");
    nvrhi::BindingSetDesc set;
    set.bindings = {nvrhi::BindingSetItem::PushConstants(0, size)};
    result.set = CreateNvrhiBindingSet(device, set, result.layout, "Failed to create a push constant set");
    return result;
}

nvrhi::FramebufferHandle CreateNvrhiFramebuffer(nvrhi::IDevice* device, std::initializer_list<nvrhi::ITexture*> colors, nvrhi::ITexture* depth)
{
    nvrhi::FramebufferDesc desc;
    for (nvrhi::ITexture* color : colors)
    {
        desc.addColorAttachment(color);
    }
    if (depth != nullptr)
    {
        desc.setDepthAttachment(depth);
    }
    nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(desc);
    if (!framebuffer)
    {
        throw std::runtime_error("Failed to create a framebuffer");
    }
    return framebuffer;
}

nvrhi::GraphicsPipelineHandle CreateFullscreenNvrhiPipeline(
    nvrhi::IDevice* device,
    const nvrhi::FramebufferInfo& framebuffer,
    std::initializer_list<nvrhi::IBindingLayout*> layouts,
    const char* fragmentShader,
    const FullscreenNvrhiOptions& options)
{
    nvrhi::GraphicsPipelineDesc desc;
    desc.VS = CreateNvrhiShader(device, nvrhi::ShaderType::Vertex, options.vertexShader);
    desc.PS = CreateNvrhiShader(device, nvrhi::ShaderType::Pixel, fragmentShader);
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    for (nvrhi::IBindingLayout* layout : layouts)
    {
        desc.bindingLayouts.push_back(layout);
    }
    desc.renderState.rasterState.setCullNone();
    nvrhi::DepthStencilState& depth = desc.renderState.depthStencilState;
    depth.setDepthTestEnable(options.depthTestAtFarPlane || options.depthTestAndWrite);
    depth.setDepthWriteEnable(options.depthTestAndWrite);
    depth.setDepthFunc(options.depthTestAndWrite ? nvrhi::ComparisonFunc::Greater : nvrhi::ComparisonFunc::GreaterOrEqual);
    depth.setStencilEnable(false);
    for (uint32_t attachment = 0; attachment < std::max(options.colorAttachmentCount, 1u); ++attachment)
    {
        nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[attachment];
        blend.setColorWriteMask(nvrhi::ColorMask::All);
        if (options.additiveBlend)
        {
            blend.setBlendEnable(true)
                .setSrcBlend(nvrhi::BlendFactor::One)
                .setDestBlend(nvrhi::BlendFactor::One)
                .setBlendOp(nvrhi::BlendOp::Add)
                .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
                .setDestBlendAlpha(nvrhi::BlendFactor::One)
                .setBlendOpAlpha(nvrhi::BlendOp::Add);
        }
    }
    nvrhi::GraphicsPipelineHandle pipeline = device->createGraphicsPipeline(desc, framebuffer);
    if (!pipeline)
    {
        throw std::runtime_error(std::string("Failed to create the full-screen pipeline for ") + fragmentShader);
    }
    return pipeline;
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

nvrhi::ViewportState NativeViewportRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    const float left = static_cast<float>(x);
    const float top = static_cast<float>(y);
    nvrhi::ViewportState state;
    state.addViewport(nvrhi::Viewport(left, left + static_cast<float>(width), top + static_cast<float>(height), top, 0.0f, 1.0f));
    state.addScissorRect(nvrhi::Rect(static_cast<int>(x), static_cast<int>(x + width), static_cast<int>(y), static_cast<int>(y + height)));
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
