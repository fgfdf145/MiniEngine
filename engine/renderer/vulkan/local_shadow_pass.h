#pragma once

#include "common.h"
#include "nvrhi_native.h"
#include "shadow_pass.h"
#include "uniform_buffer.h"

#include <engine/renderer/local_shadows.h>

#include <span>

namespace me
{

// Renders the local lights' shadow atlas (see local_shadows.h): one 2D depth image, one viewport
// per tile, which the material pass samples through set 0, binding 13. The casters, their push
// constants and the shaders are the cascade pass's (ShadowCasterRenderer).
//
// Like VulkanShadowPass it is not an IScenePass: the atlas has a fixed size and is one image shared by
// every frame in flight. It rests as a shader resource; every frame clears the whole atlas, tiles or
// none, because the material pass binds it either way.
class VulkanLocalShadowPass
{
  public:
    VulkanLocalShadowPass(nvrhi::IDevice* nvrhiDevice, nvrhi::IBindingLayout* frameSetLayout, nvrhi::IBindingLayout* materialSetLayout);
    ~VulkanLocalShadowPass();

    VulkanLocalShadowPass(const VulkanLocalShadowPass&) = delete;
    VulkanLocalShadowPass& operator=(const VulkanLocalShadowPass&) = delete;

    // The atlas and comparison sampler the material pass binds.
    TextureDescriptorBinding GetSampledBinding() const;
    // Clears the atlas and renders into each tile every caster whose bounds reach its frustum. frameSet:
    // a frame set, for the alpha test's materials.
    void Record(
        nvrhi::ICommandList* commandList,
        nvrhi::IBindingSet* frameSet,
        std::span<const ShadowDrawItem> drawItems,
        std::span<const LocalShadowTile> tiles) const;

  private:
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::TextureHandle m_texture;
    nvrhi::FramebufferHandle m_framebuffer;
    nvrhi::SamplerHandle m_sampler;
    std::unique_ptr<ShadowCasterRenderer> m_casters;
};
}
