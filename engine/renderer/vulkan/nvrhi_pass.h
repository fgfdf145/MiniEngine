#pragma once

#include "nvrhi_native.h"

#include <initializer_list>
#include <vector>

namespace me
{

// An image an NVRHI pass shares with the passes that still record native Vulkan, and the state
// native code holds it in: before the pass, and again after it. exitState, when set, is where the
// pass leaves it instead (an image the pass brings out of UNDEFINED, Common to NVRHI).
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

// One pass's NVRHI commands in the frame's command list, between passes that record native Vulkan
// into the same command buffer (docs/design/2026-10-08-nvrhi-backend-design.md, stage 3). The list
// runs without automatic barriers (VulkanCommandContext): native code moves images between layouts
// where NVRHI cannot see it, so an NVRHI pass sets the states its commands need itself
// (setTextureState, commitBarriers). The scope forgets what NVRHI last bound, since native commands
// rebound the command buffer since; tells NVRHI the state of each shared image as the pass begins;
// and as it ends, leaves each in that state again, its writes made visible to the native passes after
// it. The pass's own images NVRHI tracks for itself (keepInitialState).
class NvrhiPassScope
{
  public:
    NvrhiPassScope(
        nvrhi::ICommandList* commandList,
        std::initializer_list<NvrhiSharedTexture> shared,
        std::initializer_list<NvrhiSharedBuffer> sharedBuffers = {});
    ~NvrhiPassScope();

    NvrhiPassScope(const NvrhiPassScope&) = delete;
    NvrhiPassScope& operator=(const NvrhiPassScope&) = delete;

  private:
    nvrhi::ICommandList* m_commandList = nullptr;
    std::vector<NvrhiSharedTexture> m_shared;
    std::vector<NvrhiSharedBuffer> m_sharedBuffers;
};

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
