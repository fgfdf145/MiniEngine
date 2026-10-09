#include "imgui_nvrhi.h"

#include "nvrhi_native.h"
#include "nvrhi_pass.h"
#include "sampler_settings.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace me
{

namespace
{
ImGuiNvrhiRenderer* g_renderer = nullptr;

// imgui.vert's block: pixels to clip space.
struct ImGuiConstants
{
    float scale[2];
    float translate[2];
};

nvrhi::BufferHandle CreateMappedBuffer(nvrhi::IDevice* device, size_t byteSize, bool indices, void** mapped)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = byteSize;
    desc.isVertexBuffer = !indices;
    desc.isIndexBuffer = indices;
    desc.cpuAccess = nvrhi::CpuAccessMode::Write;
    desc.debugName = indices ? "ImGui indices" : "ImGui vertices";
    nvrhi::BufferHandle buffer = device->createBuffer(desc);
    if (!buffer)
    {
        throw std::runtime_error("Failed to create an ImGui buffer");
    }
    *mapped = device->mapBuffer(buffer, nvrhi::CpuAccessMode::Write);
    if (*mapped == nullptr)
    {
        throw std::runtime_error("Failed to map an ImGui buffer");
    }
    return buffer;
}
}

ImGuiNvrhiRenderer::ImGuiNvrhiRenderer(nvrhi::IDevice* device, uint32_t frameSlots)
    : m_device(device), m_frames(frameSlots)
{
    m_vertexShader = CreateNvrhiShader(m_device, nvrhi::ShaderType::Vertex, "imgui.vert.spv");
    m_fragmentShader = CreateNvrhiShader(m_device, nvrhi::ShaderType::Pixel, "imgui.frag.spv");
    m_hdr10FragmentShader = CreateNvrhiShader(m_device, nvrhi::ShaderType::Pixel, "imgui_hdr10.frag.spv");

    // In location order (NVRHI's Vulkan backend numbers them as listed): ImDrawVert.
    const nvrhi::VertexAttributeDesc attributes[] = {
        nvrhi::VertexAttributeDesc()
            .setName("POSITION")
            .setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(offsetof(ImDrawVert, pos))
            .setElementStride(sizeof(ImDrawVert)),
        nvrhi::VertexAttributeDesc()
            .setName("TEXCOORD")
            .setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(offsetof(ImDrawVert, uv))
            .setElementStride(sizeof(ImDrawVert)),
        nvrhi::VertexAttributeDesc()
            .setName("COLOR")
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setOffset(offsetof(ImDrawVert, col))
            .setElementStride(sizeof(ImDrawVert)),
    };
    m_inputLayout = m_device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), m_vertexShader);
    if (!m_inputLayout)
    {
        throw std::runtime_error("Failed to create the ImGui input layout");
    }

    nvrhi::BindingLayoutDesc layout;
    layout.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    layout.registerSpace = 0;
    layout.registerSpaceIsDescriptorSet = true;
    layout.bindingOffsets = ShaderBindingOffsets();
    layout.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(64),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ImGuiConstants))};
    m_bindingLayout = CreateNvrhiBindingLayout(m_device, layout, "Failed to create the ImGui binding layout");

    // Linear and clamped, as the Vulkan backend sampled: past an image's edge ImGui shows its edge.
    nvrhi::SamplerDesc sampler = BuildClampSamplerDesc(true);
    sampler.mipFilter = true;
    sampler.maxLod = 1000.0f;
    m_sampler = CreateNvrhiSampler(m_device, sampler, "Failed to create the ImGui sampler");

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "imgui_impl_nvrhi";
    io.BackendRendererUserData = this;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = 16384;
    platform.Renderer_TextureMaxHeight = 16384;
    g_renderer = this;
}

ImGuiNvrhiRenderer::~ImGuiNvrhiRenderer()
{
    // The textures ImGui asked for go with the renderer; ImGui makes them again for the next one.
    if (ImGui::GetCurrentContext() != nullptr)
    {
        for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
        {
            if (texture->RefCount == 1 && texture->Status != ImTextureStatus_Destroyed)
            {
                DestroyTexture(texture);
            }
        }
        ImGuiIO& io = ImGui::GetIO();
        if (io.BackendRendererUserData == this)
        {
            io.BackendRendererName = nullptr;
            io.BackendRendererUserData = nullptr;
            io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
        }
    }
    if (g_renderer == this)
    {
        g_renderer = nullptr;
    }
}

ImGuiNvrhiRenderer* ImGuiNvrhiRenderer::Get()
{
    return g_renderer;
}

ImTextureID ImGuiNvrhiRenderer::AddTexture(nvrhi::ITexture* texture, nvrhi::Format viewFormat)
{
    const std::lock_guard lock(m_mutex);
    const ImTextureID id = m_nextId++;
    TextureEntry entry;
    entry.texture = texture;
    entry.viewFormat = viewFormat;
    m_textures.emplace(id, std::move(entry));
    return id;
}

void ImGuiNvrhiRenderer::RemoveTexture(ImTextureID id)
{
    const std::lock_guard lock(m_mutex);
    m_textures.erase(id);
}

void ImGuiNvrhiRenderer::UpdateTexture(ImTextureData* texture)
{
    if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0)
    {
        // Unused for a frame, which every frame in flight has finished by the time ImGui asks (it
        // asks with the render thread idle).
        DestroyTexture(texture);
        return;
    }
    if (texture->Status != ImTextureStatus_WantCreate && texture->Status != ImTextureStatus_WantUpdates)
    {
        return;
    }
    if (texture->Format != ImTextureFormat_RGBA32)
    {
        throw std::runtime_error("The ImGui renderer takes RGBA32 textures only");
    }

    nvrhi::CommandListHandle commandList = m_device->createCommandList();
    commandList->open();
    if (texture->Status == ImTextureStatus_WantCreate)
    {
        nvrhi::TextureDesc desc;
        desc.width = static_cast<uint32_t>(texture->Width);
        desc.height = static_cast<uint32_t>(texture->Height);
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.dimension = nvrhi::TextureDimension::Texture2D;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "ImGui texture";
        nvrhi::TextureHandle created = m_device->createTexture(desc);
        if (!created)
        {
            throw std::runtime_error("Failed to create an ImGui texture");
        }
        commandList->writeTexture(created, 0, 0, texture->GetPixels(), static_cast<size_t>(texture->GetPitch()));
        ImTextureID id = 0;
        {
            const std::lock_guard lock(m_mutex);
            id = m_nextId++;
            TextureEntry entry;
            entry.texture = created;
            entry.owned = true;
            m_textures.emplace(id, std::move(entry));
        }
        texture->SetTexID(id);
    }
    else
    {
        nvrhi::ITexture* target = nullptr;
        {
            const std::lock_guard lock(m_mutex);
            target = m_textures.at(texture->GetTexID()).texture;
        }
        // The updated rectangle, row by row, through a staging copy of it: NVRHI writes whole
        // subresources only.
        const ImTextureRect& rect = texture->UpdateRect;
        nvrhi::TextureDesc desc = target->getDesc();
        desc.width = rect.w;
        desc.height = rect.h;
        desc.isShaderResource = false;
        desc.keepInitialState = false;
        desc.initialState = nvrhi::ResourceStates::Unknown;
        desc.debugName = "ImGui texture update";
        nvrhi::StagingTextureHandle staging = m_device->createStagingTexture(desc, nvrhi::CpuAccessMode::Write);
        size_t rowPitch = 0;
        auto* mapped = static_cast<uint8_t*>(m_device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Write, &rowPitch));
        if (mapped == nullptr)
        {
            throw std::runtime_error("Failed to map an ImGui texture update");
        }
        for (int row = 0; row < rect.h; ++row)
        {
            std::memcpy(mapped + row * rowPitch, texture->GetPixelsAt(rect.x, rect.y + row), static_cast<size_t>(rect.w) * 4);
        }
        m_device->unmapStagingTexture(staging);
        commandList->copyTexture(
            target,
            nvrhi::TextureSlice().setOrigin(rect.x, rect.y).setWidth(rect.w).setHeight(rect.h),
            staging,
            nvrhi::TextureSlice().setWidth(rect.w).setHeight(rect.h));
    }
    commandList->close();
    m_device->executeCommandList(commandList);
    m_device->waitForIdle();
    texture->SetStatus(ImTextureStatus_OK);
}

void ImGuiNvrhiRenderer::DestroyTexture(ImTextureData* texture)
{
    {
        const std::lock_guard lock(m_mutex);
        m_textures.erase(texture->GetTexID());
    }
    texture->SetTexID(ImTextureID_Invalid);
    texture->SetStatus(ImTextureStatus_Destroyed);
}

nvrhi::IGraphicsPipeline* ImGuiNvrhiRenderer::Pipeline(nvrhi::IFramebuffer* framebuffer, bool hdr10)
{
    const nvrhi::FramebufferInfo& info = framebuffer->getFramebufferInfo();
    const nvrhi::Format format = info.colorFormats.empty() ? nvrhi::Format::UNKNOWN : info.colorFormats[0];
    for (const PipelineEntry& entry : m_pipelines)
    {
        if (entry.format == format && entry.hdr10 == hdr10)
        {
            return entry.pipeline;
        }
    }
    nvrhi::GraphicsPipelineDesc desc;
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.inputLayout = m_inputLayout;
    desc.VS = m_vertexShader;
    desc.PS = hdr10 ? m_hdr10FragmentShader : m_fragmentShader;
    desc.bindingLayouts = {m_bindingLayout};
    nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[0];
    blend.setBlendEnable(true)
        .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
        .setBlendOp(nvrhi::BlendOp::Add)
        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
        .setBlendOpAlpha(nvrhi::BlendOp::Add);
    desc.renderState.rasterState.setCullNone().setScissorEnable(true);
    desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilEnable(false);
    nvrhi::GraphicsPipelineHandle pipeline = m_device->createGraphicsPipeline(desc, info);
    if (!pipeline)
    {
        throw std::runtime_error("Failed to create the ImGui pipeline");
    }
    m_pipelines.push_back(PipelineEntry{format, hdr10, pipeline});
    return pipeline;
}

nvrhi::IBindingSet* ImGuiNvrhiRenderer::BindingSet(ImTextureID id)
{
    const std::lock_guard lock(m_mutex);
    const auto found = m_textures.find(id);
    if (found == m_textures.end())
    {
        return nullptr;
    }
    TextureEntry& entry = found->second;
    if (!entry.bindingSet)
    {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, entry.texture, entry.viewFormat),
            nvrhi::BindingSetItem::Sampler(64, m_sampler),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(ImGuiConstants))};
        entry.bindingSet = CreateNvrhiBindingSet(m_device, desc, m_bindingLayout, "Failed to create an ImGui binding set");
    }
    return entry.bindingSet;
}

void ImGuiNvrhiRenderer::Reserve(FrameBuffers& buffers, size_t vertexCount, size_t indexCount)
{
    // The slot's last frame has finished: its buffers can go.
    if (vertexCount > buffers.vertexCapacity)
    {
        buffers.vertexCapacity = std::max<size_t>(vertexCount + 5000, buffers.vertexCapacity * 2);
        buffers.vertices = CreateMappedBuffer(m_device, buffers.vertexCapacity * sizeof(ImDrawVert), false, &buffers.mappedVertices);
    }
    if (indexCount > buffers.indexCapacity)
    {
        buffers.indexCapacity = std::max<size_t>(indexCount + 10000, buffers.indexCapacity * 2);
        buffers.indices = CreateMappedBuffer(m_device, buffers.indexCapacity * sizeof(ImDrawIdx), true, &buffers.mappedIndices);
    }
}

void ImGuiNvrhiRenderer::Render(
    nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, ImDrawData* drawData, uint32_t frameSlot, bool hdr10)
{
    if (drawData == nullptr || drawData->TotalVtxCount == 0)
    {
        return;
    }
    const float width = drawData->DisplaySize.x * drawData->FramebufferScale.x;
    const float height = drawData->DisplaySize.y * drawData->FramebufferScale.y;
    if (width <= 0.0f || height <= 0.0f)
    {
        return;
    }

    FrameBuffers& buffers = m_frames.at(frameSlot);
    Reserve(buffers, static_cast<size_t>(drawData->TotalVtxCount), static_cast<size_t>(drawData->TotalIdxCount));
    auto* vertices = static_cast<ImDrawVert*>(buffers.mappedVertices);
    auto* indices = static_cast<ImDrawIdx*>(buffers.mappedIndices);
    for (const ImDrawList* list : drawData->CmdLists)
    {
        std::memcpy(vertices, list->VtxBuffer.Data, static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
        std::memcpy(indices, list->IdxBuffer.Data, static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
        vertices += list->VtxBuffer.Size;
        indices += list->IdxBuffer.Size;
    }

    ImGuiConstants constants{};
    constants.scale[0] = 2.0f / drawData->DisplaySize.x;
    constants.scale[1] = 2.0f / drawData->DisplaySize.y;
    constants.translate[0] = -1.0f - drawData->DisplayPos.x * constants.scale[0];
    constants.translate[1] = -1.0f - drawData->DisplayPos.y * constants.scale[1];

    nvrhi::GraphicsState state;
    state.pipeline = Pipeline(framebuffer, hdr10);
    state.framebuffer = framebuffer;
    const nvrhi::Viewport viewport =
        NativeViewportState(VkExtent2D{static_cast<uint32_t>(width), static_cast<uint32_t>(height)}).viewports.front();
    state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(buffers.vertices).setSlot(0).setOffset(0)};
    state.indexBuffer = nvrhi::IndexBufferBinding()
                            .setBuffer(buffers.indices)
                            .setFormat(sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT)
                            .setOffset(0);

    const ImVec2 clipOffset = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;
    uint32_t vertexOffset = 0;
    uint32_t indexOffset = 0;
    for (const ImDrawList* list : drawData->CmdLists)
    {
        for (const ImDrawCmd& command : list->CmdBuffer)
        {
            if (command.UserCallback != nullptr)
            {
                if (command.UserCallback != ImDrawCallback_ResetRenderState)
                {
                    command.UserCallback(list, &command);
                }
                continue;
            }
            // The clip rectangle in framebuffer pixels, clamped to it.
            const float minX = std::max((command.ClipRect.x - clipOffset.x) * clipScale.x, 0.0f);
            const float minY = std::max((command.ClipRect.y - clipOffset.y) * clipScale.y, 0.0f);
            const float maxX = std::min((command.ClipRect.z - clipOffset.x) * clipScale.x, width);
            const float maxY = std::min((command.ClipRect.w - clipOffset.y) * clipScale.y, height);
            if (maxX <= minX || maxY <= minY)
            {
                continue;
            }
            nvrhi::IBindingSet* set = BindingSet(command.GetTexID());
            if (set == nullptr)
            {
                continue;
            }
            nvrhi::ViewportState viewportState;
            viewportState.addViewport(viewport);
            viewportState.addScissorRect(nvrhi::Rect(
                static_cast<int>(minX), static_cast<int>(maxX), static_cast<int>(minY), static_cast<int>(maxY)));
            state.viewport = viewportState;
            state.bindings = {set};
            commandList->setGraphicsState(state);
            // After every state: NVRHI's validation wants them with each.
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->drawIndexed(nvrhi::DrawArguments()
                                         .setVertexCount(command.ElemCount)
                                         .setStartIndexLocation(indexOffset + command.IdxOffset)
                                         .setStartVertexLocation(vertexOffset + command.VtxOffset));
        }
        vertexOffset += static_cast<uint32_t>(list->VtxBuffer.Size);
        indexOffset += static_cast<uint32_t>(list->IdxBuffer.Size);
    }
}
}
