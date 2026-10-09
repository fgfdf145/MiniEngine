#pragma once

#include "nvrhi_native.h"

#include <initializer_list>
#include <span>
#include <vector>

namespace me
{

// An image a pass uses and the state its commands need it in as the pass begins; and as it ends,
// the state it leaves it in: exitState, or the same state again. A state of Common asks for nothing
// at the start: the pass rewrites the image whole, whatever it held.
struct NvrhiSharedTexture
{
    nvrhi::ITexture* texture = nullptr;
    nvrhi::ResourceStates state = nvrhi::ResourceStates::Unknown;
    nvrhi::ResourceStates exitState = nvrhi::ResourceStates::Unknown;
};

// The same for a buffer.
struct NvrhiSharedBuffer
{
    nvrhi::IBuffer* buffer = nullptr;
    nvrhi::ResourceStates state = nvrhi::ResourceStates::Unknown;
    nvrhi::ResourceStates exitState = nvrhi::ResourceStates::Unknown;
};

// One pass's commands in the frame's command list (docs/design/2026-10-09-d3d12-backend-design.md).
// The list runs without automatic barriers: a pass sets the states its commands need itself
// (setTextureState, commitBarriers), from the states NVRHI tracks for every resource (each one rests
// in its initialState between command lists, keepInitialState). The scope forgets what was last
// bound (a native command, NGX's, may have rebound the command buffer since); moves each shared image
// into the state the pass begins with; and as it ends, leaves each in its exit state, its writes made
// visible to the passes after it.
class NvrhiPassScope
{
  public:
    NvrhiPassScope(
        nvrhi::ICommandList* commandList,
        std::initializer_list<NvrhiSharedTexture> shared,
        std::initializer_list<NvrhiSharedBuffer> sharedBuffers = {});
    NvrhiPassScope(nvrhi::ICommandList* commandList, std::span<const NvrhiSharedTexture> shared);
    ~NvrhiPassScope();

    NvrhiPassScope(const NvrhiPassScope&) = delete;
    NvrhiPassScope& operator=(const NvrhiPassScope&) = delete;

  private:
    nvrhi::ICommandList* m_commandList = nullptr;
    std::vector<NvrhiSharedTexture> m_shared;
    std::vector<NvrhiSharedBuffer> m_sharedBuffers;
};

// Clears that need a different state on each backend (Vulkan clears as a copy destination, D3D12
// through an unordered access or render target view): NVRHI's automatic barriers are on for the clear
// alone, so the resource must be one NVRHI tracks (keepInitialState, or a scope's shared resource).
void ClearBufferUInt(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, uint32_t value);
void ClearTextureFloat(nvrhi::ICommandList* commandList, nvrhi::ITexture* texture, const nvrhi::Color& value);
void ClearTextureUInt(nvrhi::ICommandList* commandList, nvrhi::ITexture* texture, uint32_t value);

// Buffers as the passes make them. Device-local ones rest as shader resources between frames
// (keepInitialState); stride, when non-zero, is the element size shaders declare it with
// (StructuredBuffer<T>, which D3D12 views need), and uav lets them write it. Upload buffers are
// written by the CPU through mapped (mapped for good) and read by shaders; readback buffers are copy
// destinations the CPU reads through mapped once the frame that copied has finished.
nvrhi::BufferHandle CreateDeviceBuffer(nvrhi::IDevice* device, uint64_t byteSize, uint32_t stride, bool uav, const char* name);
nvrhi::BufferHandle CreateUploadBuffer(nvrhi::IDevice* device, uint64_t byteSize, uint32_t stride, const char* name, void** mapped);
nvrhi::BufferHandle CreateReadbackBuffer(nvrhi::IDevice* device, uint64_t byteSize, const char* name, void** mapped);

void ClearDepth(nvrhi::ICommandList* commandList, nvrhi::ITexture* texture, float depth);

// Vulkan only: a keepInitialState texture comes out of UNDEFINED in the first command list that
// moves it, so one that is only ever sampled (set 0 names it before anything writes it) would never
// leave UNDEFINED. Texture creation registers each (D3D12 makes them in their initial state); the
// frame's command list moves every registered one to its initial state first thing.
void RegisterInitialTransition(nvrhi::ITexture* texture);
void RecordInitialTransitions(nvrhi::ICommandList* commandList);
// Forgets the registered textures, before the device goes.
void DropInitialTransitions();

// shader with its specialization constants set (Vulkan's): name only says which shader failed.
nvrhi::ShaderHandle SpecializeShader(
    nvrhi::IDevice* device, nvrhi::IShader* shader, std::span<const nvrhi::ShaderSpecialization> constants, const char* name);

// A binding layout holding push constants alone, at registerSpace (the space a shader declares them
// in with D3D_PUSH_CONSTANTS), and the one binding set every draw binds for it.
struct PushConstantLayout
{
    nvrhi::BindingLayoutHandle layout;
    nvrhi::BindingSetHandle set;
};
PushConstantLayout CreatePushConstantLayout(nvrhi::IDevice* device, uint32_t registerSpace, uint32_t size, nvrhi::ShaderType visibility);

// A framebuffer over colour targets and an optional depth target; throws when NVRHI cannot make it.
nvrhi::FramebufferHandle CreateNvrhiFramebuffer(
    nvrhi::IDevice* device, std::initializer_list<nvrhi::ITexture*> colors, nvrhi::ITexture* depth = nullptr);

// The full-screen pipelines' shape: fullscreen.vert (or another vertex shader with no vertex input),
// a triangle list, no culling, no depth test unless the options ask for it, all four channels of
// every colour target written without blending.
struct FullscreenNvrhiOptions
{
    const char* vertexShader = "fullscreen.vert.spv";
    // Test against the pass's depth target with nearer-or-equal (reverse-Z: GreaterOrEqual), writing
    // nothing: with sky.vert, which places the triangle on the far plane (depth 0), that draws only
    // where no geometry did.
    bool depthTestAtFarPlane = false;
    // Adds the fragment's rgb to what the target holds (One, One) and leaves alpha alone.
    bool additiveBlend = false;
    // Test with nearer (reverse-Z: Greater) and write the depth the fragment shader gives, as a
    // surface drawn among the scene's geometry does. Overrides depthTestAtFarPlane.
    bool depthTestAndWrite = false;
    // The framebuffer's colour targets, each written like the first.
    uint32_t colorAttachmentCount = 1;
};
nvrhi::GraphicsPipelineHandle CreateFullscreenNvrhiPipeline(
    nvrhi::IDevice* device,
    const nvrhi::FramebufferInfo& framebuffer,
    std::initializer_list<nvrhi::IBindingLayout*> layouts,
    const char* fragmentShader,
    const FullscreenNvrhiOptions& options = {});

// A SPIR-V shader from the shader folder (EnginePaths::ShaderRoot), entry point main. Throws when
// the file is missing or NVRHI rejects it.
nvrhi::ShaderHandle CreateNvrhiShader(nvrhi::IDevice* device, nvrhi::ShaderType type, const char* shaderName);

// A compute pipeline running shaderName over layouts. Every engine binding layout names its
// descriptor set as its registerSpace, with registerSpaceIsDescriptorSet (frame set 0, G-buffer set 2,
// a pass's own set): NVRHI places each layout at that set, and its validation tells the layouts'
// bindings apart by it.
nvrhi::ComputePipelineHandle CreateNvrhiComputePipeline(
    nvrhi::IDevice* device,
    const char* shaderName,
    std::initializer_list<nvrhi::IBindingLayout*> layouts);

// A viewport and scissor over extent the way the engine's shaders are written for: Vulkan's own, y
// down. NVRHI turns every viewport into Direct3D's (a negative height from maxY); handing it the
// rectangle upside down (minY at the bottom edge) cancels that, and the scissor is given apart.
nvrhi::ViewportState NativeViewportState(VkExtent2D extent);
// The same for a rectangle of the target (x, y its top-left texel), its scissor that rectangle.
nvrhi::ViewportState NativeViewportRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

// NVRHI's VulkanBindingOffsets all zero: a binding layout item's slot is the binding the shader
// declares, as the engine's shaders number their sets themselves.
nvrhi::VulkanBindingOffsets ShaderBindingOffsets();

// Throws failureMessage when the device cannot make the binding layout or set.
nvrhi::BindingLayoutHandle CreateNvrhiBindingLayout(nvrhi::IDevice* device, const nvrhi::BindingLayoutDesc& desc, const char* failureMessage);
nvrhi::BindingSetHandle CreateNvrhiBindingSet(
    nvrhi::IDevice* device,
    const nvrhi::BindingSetDesc& desc,
    nvrhi::IBindingLayout* layout,
    const char* failureMessage);
}
