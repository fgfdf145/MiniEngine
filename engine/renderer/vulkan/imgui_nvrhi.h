#pragma once

#include <nvrhi/nvrhi.h>

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace me
{

// Dear ImGui's renderer on NVRHI, the same on Vulkan and Direct3D 12 (docs/design/2026-10-09-d3d12-backend-design.md):
// it makes and updates the textures ImGui asks for (the font atlas, ImGuiBackendFlags_RendererHasTextures)
// and draws the frame's draw data into a framebuffer, SDR or HDR10 (imgui.frag). The images the
// engine shows in ImGui (the viewport, the minimap) are added under an ImTextureID with AddTexture.
//
// One instance at a time, registered with the ImGui context (its io.BackendRendererUserData), whose
// thread rules it follows: textures are made and drawn on the thread that owns the device's queue.
class ImGuiNvrhiRenderer
{
  public:
    // frameSlots: frames in flight, each with its own vertex and index buffers.
    ImGuiNvrhiRenderer(nvrhi::IDevice* device, uint32_t frameSlots);
    ~ImGuiNvrhiRenderer();

    ImGuiNvrhiRenderer(const ImGuiNvrhiRenderer&) = delete;
    ImGuiNvrhiRenderer& operator=(const ImGuiNvrhiRenderer&) = delete;

    // The registered instance, or null.
    static ImGuiNvrhiRenderer* Get();

    // An engine image ImGui::Image can show: texture through a view of viewFormat (UNKNOWN: the
    // texture's own), sampled linear and clamped. The texture must stay alive until RemoveTexture.
    ImTextureID AddTexture(nvrhi::ITexture* texture, nvrhi::Format viewFormat = nvrhi::Format::UNKNOWN);
    void RemoveTexture(ImTextureID id);

    // One of ImGui's texture requests (create, update, destroy), run at once with a command list of
    // its own.
    void UpdateTexture(ImTextureData* texture);

    // Draws drawData into framebuffer, which the caller has made a render target (and cleared), in
    // frameSlot's buffers. hdr10 selects the HDR10 shader (the swapchain is PQ Rec.2020).
    void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, ImDrawData* drawData, uint32_t frameSlot, bool hdr10);

  private:
    struct TextureEntry
    {
        nvrhi::TextureHandle texture;
        nvrhi::Format viewFormat = nvrhi::Format::UNKNOWN;
        nvrhi::BindingSetHandle bindingSet;
        // A float texture holds linear light (the scene image under HDR output); the others hold what
        // the SDR swapchain shows, sRGB-encoded.
        bool linear = false;
        // Made here for an ImTextureData (the font atlas pages), not the engine's.
        bool owned = false;
    };
    struct FrameBuffers
    {
        nvrhi::BufferHandle vertices;
        nvrhi::BufferHandle indices;
        void* mappedVertices = nullptr;
        void* mappedIndices = nullptr;
        size_t vertexCapacity = 0;
        size_t indexCapacity = 0;
    };

    nvrhi::IGraphicsPipeline* Pipeline(nvrhi::IFramebuffer* framebuffer, bool hdr10);
    // The texture's binding set, and whether it holds linear light; null when the ID is unknown.
    nvrhi::IBindingSet* BindingSet(ImTextureID id, bool& linear);
    void Reserve(FrameBuffers& buffers, size_t vertexCount, size_t indexCount);
    void DestroyTexture(ImTextureData* texture);

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::ShaderHandle m_vertexShader;
    nvrhi::ShaderHandle m_fragmentShader;
    nvrhi::ShaderHandle m_hdr10FragmentShader;
    nvrhi::InputLayoutHandle m_inputLayout;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::SamplerHandle m_sampler;
    // Pipelines by framebuffer format and output.
    struct PipelineEntry
    {
        nvrhi::Format format = nvrhi::Format::UNKNOWN;
        bool hdr10 = false;
        nvrhi::GraphicsPipelineHandle pipeline;
    };
    std::vector<PipelineEntry> m_pipelines;
    std::vector<FrameBuffers> m_frames;
    std::mutex m_mutex;
    std::unordered_map<ImTextureID, TextureEntry> m_textures;
    ImTextureID m_nextId = 1;
};
}
