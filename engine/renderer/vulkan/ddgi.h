#pragma once

#include "common.h"
#include "nvrhi_native.h"
#include "uniform_buffer.h"

#include <engine/renderer/ddgi_volume.h>

#include <cstdint>
#include <span>
#include <vector>

namespace me
{

// The cascaded DDGI probes on the GPU (docs/design/2026-09-27-ddgi-design.md): the irradiance and
// visibility atlases (one array layer per level), the probe states, the ray buffer and each frame
// slot's schedule. Each frame the probes the CPU scheduled trace their rays (ddgi_trace.comp) and
// blend them into their tiles (ddgi_update.comp), both NVRHI dispatches. Device lifetime like
// VulkanEnvironmentProbe: set 0 binds the atlases for every draw (bindings 21 to 23), so they rest as
// shader resources and are unordered access only for the update. It records after the ray scene and
// before the scene passes.
class VulkanDdgi
{
  public:
    // The most probes one frame can update (DdgiSettings::probesPerFrame is clamped to it).
    static constexpr uint32_t kMaxProbesPerFrame = 4096;

    // raySetLayout is the ray scene's set (VulkanRayScene::GetNvrhiSetLayout), which the trace binds
    // at set 1; rayQuery makes ddgi_trace_ray_query.comp's pipeline as well.
    VulkanDdgi(
        nvrhi::IDevice* nvrhiDevice,
        nvrhi::IBindingLayout* frameSetLayout,
        nvrhi::IBindingLayout* raySetLayout,
        uint32_t frameCount,
        bool rayQuery);
    ~VulkanDdgi();

    VulkanDdgi(const VulkanDdgi&) = delete;
    VulkanDdgi& operator=(const VulkanDdgi&) = delete;

    // This frame's schedule (PackDdgiProbe values, at most kMaxProbesPerFrame), into the slot's buffer.
    void SetSchedule(uint32_t frameSlot, std::span<const uint32_t> probes);

    // What the update reported (kDdgiFeedback* values, one per probe) for the schedule this frame
    // slot recorded last, once its fence has signalled: scheduled gets that schedule, feedback the
    // reports. Both come back empty when the slot recorded none since the last call. Call before
    // SetSchedule replaces the slot's schedule.
    void TakeFeedback(uint32_t frameSlot, std::vector<uint32_t>& scheduled, std::vector<uint32_t>& feedback);

    // Clears every probe on the next Record: new content, whose light the old probes do not hold.
    void Invalidate();

    void Record(
        nvrhi::ICommandList* commandList,
        nvrhi::IBindingSet* frameSet,
        nvrhi::IBindingSet* raySet,
        uint32_t frameSlot,
        uint32_t frameIndex,
        float hysteresis,
        uint32_t lightingEpoch,
        uint32_t geometryEpoch,
        bool rayQuery);

    TextureDescriptorBinding GetIrradianceBinding() const;
    TextureDescriptorBinding GetVisibilityBinding() const;
    VkBuffer GetProbeStateBuffer() const;
    nvrhi::IBuffer* GetProbeStateHandle() const;
    // The atlases (RGBA16F and RG16F, resting as shader resources), for the reference comparison's
    // readback.
    nvrhi::ITexture* GetIrradianceTexture() const;
    nvrhi::ITexture* GetVisibilityTexture() const;
    VkImage GetIrradianceImage() const;
    VkImage GetVisibilityImage() const;

  private:
    nvrhi::TextureHandle CreateAtlas(uint32_t texelsPerProbe, nvrhi::Format format, const char* name) const;

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    uint32_t m_frameCount = 0;
    nvrhi::TextureHandle m_irradiance;
    nvrhi::TextureHandle m_visibility;
    nvrhi::SamplerHandle m_sampler;
    nvrhi::BufferHandle m_states;
    nvrhi::BufferHandle m_rays;
    std::vector<nvrhi::BufferHandle> m_schedules;
    std::vector<void*> m_scheduleMapped;
    std::vector<uint32_t> m_scheduleCounts;
    // Per frame slot: the update's reports (host visible), a copy of the schedule they answer, and
    // whether that schedule was recorded since the last TakeFeedback.
    std::vector<nvrhi::BufferHandle> m_feedback;
    std::vector<nvrhi::BufferHandle> m_feedbackReadback;
    std::vector<void*> m_feedbackMapped;
    std::vector<std::vector<uint32_t>> m_recordedSchedules;
    std::vector<uint8_t> m_feedbackPending;
    // Set 2: the trace's (schedule, rays written) and the update's (schedule, rays read, atlases,
    // states and feedback written), one binding set of each per frame slot.
    nvrhi::BindingLayoutHandle m_traceLayout;
    nvrhi::BindingLayoutHandle m_updateLayout;
    std::vector<nvrhi::BindingSetHandle> m_traceSets;
    std::vector<nvrhi::BindingSetHandle> m_updateSets;
    nvrhi::ComputePipelineHandle m_tracePipeline;
    // ddgi_trace_ray_query.comp, made when the ray set has hardware ray tracing (the constructor's
    // rayQuery); Record's rayQuery picks it per frame.
    nvrhi::ComputePipelineHandle m_rayQueryTracePipeline;
    nvrhi::ComputePipelineHandle m_updatePipeline;
    bool m_cleared = false;
    // The atlases have been cleared once: until then they hold nothing (Common), afterwards they rest
    // as shader resources.
    bool m_atlasesInitialized = false;
};
}
