#include <engine/renderer/vulkan/format_support.h>
#include <engine/renderer/vulkan/render_target_layout.h>
#include <engine/renderer/vulkan/scene_pass_order.h>

#include <array>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

RenderPassIo MakeIo(
    std::span<const RenderTargetId> reads,
    std::span<const RenderTargetId> writes)
{
    RenderPassIo io{};
    io.reads = reads;
    io.writes = writes;
    return io;
}

void UndefinedColorWriteBecomesColorAttachment()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> writes = {RenderTargetId::SceneHdr};

    const std::vector<TargetTransition> transitions = tracker.Transition(MakeIo({}, writes));

    Require(transitions.size() == 1, "a first write must produce one transition");
    Require(transitions[0].target == RenderTargetId::SceneHdr, "wrong target in transition");
    Require(transitions[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED, "a fresh target must start undefined");
    Require(
        transitions[0].newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        "a color write must target the color attachment layout");
    Require(
        tracker.GetLayout(RenderTargetId::SceneHdr) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        "Transition must record the new layout");
}

void DepthWriteBecomesDepthAttachment()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> writes = {RenderTargetId::SceneDepth};

    const std::vector<TargetTransition> transitions = tracker.Transition(MakeIo({}, writes));

    Require(transitions.size() == 1, "a first depth write must produce one transition");
    Require(
        transitions[0].newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        "a depth write must not use the color attachment layout");
}

void WrittenThenReadBecomesShaderRead()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> hdr = {RenderTargetId::SceneHdr};
    tracker.Transition(MakeIo({}, hdr));

    const std::vector<TargetTransition> transitions = tracker.Transition(MakeIo(hdr, {}));

    Require(transitions.size() == 1, "a read after a write must produce one transition");
    Require(
        transitions[0].oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        "the old layout must be what the previous pass left behind");
    Require(
        transitions[0].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        "a read must target the shader read only layout");
}

void ReadAfterReadProducesNothing()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> hdr = {RenderTargetId::SceneHdr};
    tracker.Transition(MakeIo({}, hdr));
    tracker.Transition(MakeIo(hdr, {}));

    const std::vector<TargetTransition> transitions = tracker.Transition(MakeIo(hdr, {}));

    Require(transitions.empty(), "a second read of a target already in the read layout needs no barrier");
}

void WriteAfterWriteProducesAMemoryBarrier()
{
    // Two passes writing the same attachment back to back (the geometry pass then the forward pass
    // on depth, the lighting pass then the forward blend pass on HDR) must still be ordered. The
    // layout does not change, so this is a barrier with oldLayout == newLayout: memory only.
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> hdr = {RenderTargetId::SceneHdr};
    tracker.Transition(MakeIo({}, hdr));

    const std::vector<TargetTransition> transitions = tracker.Transition(MakeIo({}, hdr));

    Require(transitions.size() == 1, "a write after a write must produce one barrier");
    Require(
        transitions[0].oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
            transitions[0].newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        "a write after a write keeps the layout and only orders memory");
}

void RepeatedDeclarationIsIdempotent()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> reads = {RenderTargetId::SceneHdr};
    const std::array<RenderTargetId, 1> writes = {RenderTargetId::SceneLdr};

    const std::vector<TargetTransition> first = tracker.Transition(MakeIo(reads, writes));
    const std::vector<TargetTransition> second = tracker.Transition(MakeIo(reads, writes));

    Require(first.size() == 2, "the first application must transition both targets");
    // The read target needs nothing the second time; the written one needs a memory-only barrier
    // so the second write is ordered after the first.
    Require(second.size() == 1, "applying the same declaration twice must order only the write");
    Require(
        second[0].target == RenderTargetId::SceneLdr && second[0].oldLayout == second[0].newLayout,
        "the repeated write must get a memory-only barrier");
}

void ResetReturnsEveryTargetToUndefined()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> hdr = {RenderTargetId::SceneHdr};
    tracker.Transition(MakeIo({}, hdr));

    tracker.Reset();

    Require(
        tracker.GetLayout(RenderTargetId::SceneHdr) == VK_IMAGE_LAYOUT_UNDEFINED,
        "Reset must return targets to undefined so recreated images are re-transitioned");
}

void TargetInBothSpansIsRejected()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> both = {RenderTargetId::SceneHdr};

    bool threw = false;
    try
    {
        tracker.Transition(MakeIo(both, both));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    Require(threw, "a target read and written by one pass must be rejected");
}

void ChooseFormatTakesTheFirstSupportedCandidate()
{
    const std::array<VkFormat, 2> candidates = {
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_FORMAT_R16G16B16A16_SFLOAT};

    const VkFormat chosen = ChooseFormat(
        "HDR",
        candidates,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
        [](VkFormat format)
        {
            return format == VK_FORMAT_B10G11R11_UFLOAT_PACK32
                       ? static_cast<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
                       : static_cast<VkFormatFeatureFlags>(0);
        });

    Require(chosen == VK_FORMAT_B10G11R11_UFLOAT_PACK32, "the first supported candidate must win");
}

void ChooseFormatFallsThroughToALaterCandidate()
{
    const std::array<VkFormat, 2> candidates = {
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_FORMAT_R16G16B16A16_SFLOAT};

    const VkFormat chosen = ChooseFormat(
        "HDR",
        candidates,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
        [](VkFormat format)
        {
            return format == VK_FORMAT_R16G16B16A16_SFLOAT
                       ? static_cast<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
                       : static_cast<VkFormatFeatureFlags>(0);
        });

    Require(chosen == VK_FORMAT_R16G16B16A16_SFLOAT, "an unsupported first candidate must be skipped");
}

void ChooseFormatRequiresEveryRequestedFeature()
{
    // The depth target must be both a depth attachment and sampleable. A candidate offering only
    // one of the two must be skipped, which is the constraint this design newly imposes.
    const std::array<VkFormat, 2> candidates = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT};
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

    const VkFormat chosen = ChooseFormat(
        "depth",
        candidates,
        required,
        [](VkFormat format)
        {
            if (format == VK_FORMAT_D32_SFLOAT)
            {
                return static_cast<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
            }
            return static_cast<VkFormatFeatureFlags>(
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        });

    Require(chosen == VK_FORMAT_D24_UNORM_S8_UINT, "a partially supported candidate must be skipped");
}

void ChooseFormatThrowsWhenNothingQualifies()
{
    const std::array<VkFormat, 1> candidates = {VK_FORMAT_D32_SFLOAT};

    bool threw = false;
    try
    {
        ChooseFormat(
            "depth",
            candidates,
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
            [](VkFormat)
            {
                return static_cast<VkFormatFeatureFlags>(0);
            });
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    Require(threw, "no qualifying candidate must throw rather than return an undefined format");
}

void DeferredOrderRunsGeometryAoLightingForwardExposureThenTonemap()
{
    const std::span<const ScenePassId> order = BuildScenePassOrder(false);

    Require(order.size() == 13, "the deferred order must contain thirteen passes");
    Require(order[0] == ScenePassId::Geometry, "the deferred order must start with the geometry pass");
    Require(order[1] == ScenePassId::AoTrace, "the AO trace reads the finished G-buffer");
    Require(order[2] == ScenePassId::AoResolve, "the AO resolve filters the trace");
    Require(order[3] == ScenePassId::SsrTrace, "the reflection trace reads the finished G-buffer");
    Require(order[4] == ScenePassId::SsrResolve, "the reflection resolve filters the trace");
    Require(order[5] == ScenePassId::Lighting, "lighting reads the resolved AO and reflections");
    Require(order[6] == ScenePassId::Forward, "the forward pass (forward-shaded opaque, sky) must follow lighting");
    Require(order[7] == ScenePassId::TransmissionCopy, "the scene behind transmissive surfaces is copied once it is complete");
    Require(order[8] == ScenePassId::ForwardTranslucent, "transmissive and Blend surfaces draw over the copied scene");
    Require(order[9] == ScenePassId::Taa, "TAA resolves the finished HDR image, blend surfaces included");
    Require(order[10] == ScenePassId::Bloom, "bloom spreads the resolved, stable image");
    Require(order[11] == ScenePassId::ExposureHistogram, "the histogram must meter the finished image");
    Require(order[12] == ScenePassId::Tonemap, "the deferred order must end in tone mapping");
}

void ForwardOnlyOrderSkipsTheDeferredPasses()
{
    const std::span<const ScenePassId> order = BuildScenePassOrder(true);

    Require(order.size() == 7, "the forward-only order must contain seven passes");
    Require(order[0] == ScenePassId::Forward, "the forward-only order must start with the forward pass");
    Require(order[1] == ScenePassId::TransmissionCopy, "both orders copy the scene for transmission");
    Require(order[2] == ScenePassId::ForwardTranslucent, "both orders draw transmissive and Blend surfaces last");
    Require(order[3] == ScenePassId::Taa, "both orders hand the image to TAA, which passes it through here");
    Require(order[4] == ScenePassId::Bloom, "bloom needs no motion vectors, so both orders have it");
    Require(order[5] == ScenePassId::ExposureHistogram, "both orders must meter the same way");
    Require(order[6] == ScenePassId::Tonemap, "both orders must end in the same tone mapping pass");
}

void GBufferTargetsAreColorTargets()
{
    constexpr std::array<RenderTargetId, 8> gbuffer = {
        RenderTargetId::GBufferAlbedo,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferEmissive,
        RenderTargetId::GBufferVelocity,
        RenderTargetId::GBufferSpecular,
        RenderTargetId::GBufferCoat,
        RenderTargetId::GBufferSheen};

    for (const RenderTargetId target : gbuffer)
    {
        Require(GetRenderTargetKind(target) == RenderTargetKind::Color, "every G-buffer target must be a color target");
        Require(
            GetWriteLayout(target) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            "a G-buffer write must use the color attachment layout");
    }
}

void AoTargetsAreStorageTargets()
{
    for (const RenderTargetId target :
         {RenderTargetId::AoRaw, RenderTargetId::SceneAo, RenderTargetId::SceneTaa, RenderTargetId::SsrRaw, RenderTargetId::SceneReflections})
    {
        Require(GetRenderTargetKind(target) == RenderTargetKind::Storage, "AO targets are written by compute");
        Require(GetWriteLayout(target) == VK_IMAGE_LAYOUT_GENERAL, "a storage write needs the general layout");
    }
}

void StorageWriteThenReadBecomesShaderRead()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> ao = {RenderTargetId::SceneAo};
    const std::vector<TargetTransition> write = tracker.Transition(MakeIo({}, ao));
    Require(write.size() == 1 && write[0].newLayout == VK_IMAGE_LAYOUT_GENERAL, "a storage write targets general");

    const std::vector<TargetTransition> read = tracker.Transition(MakeIo(ao, {}));
    Require(read.size() == 1, "a read after a storage write needs one barrier");
    Require(read[0].oldLayout == VK_IMAGE_LAYOUT_GENERAL, "the read starts from what the write left");
    Require(read[0].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "a read targets shader read only");
}

void DepthFollowsTheDeferredWriteReadWriteSequence()
{
    // The geometry pass writes depth, the lighting pass samples it, then the forward blend pass
    // depth-tests against it again. Phase one never read depth, so this sequence is new.
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> depth = {RenderTargetId::SceneDepth};

    tracker.Transition(MakeIo({}, depth));
    const std::vector<TargetTransition> toRead = tracker.Transition(MakeIo(depth, {}));
    const std::vector<TargetTransition> backToWrite = tracker.Transition(MakeIo({}, depth));

    Require(toRead.size() == 1, "sampling written depth must issue exactly one transition");
    Require(
        toRead[0].oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
            toRead[0].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        "sampling depth must move it from the attachment layout to shader read");
    Require(backToWrite.size() == 1, "depth-testing sampled depth must issue exactly one transition");
    Require(
        backToWrite[0].oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            backToWrite[0].newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        "writing sampled depth again must return it to the attachment layout");
}
}

int main()
{
    try
    {
        UndefinedColorWriteBecomesColorAttachment();
        DepthWriteBecomesDepthAttachment();
        WrittenThenReadBecomesShaderRead();
        ReadAfterReadProducesNothing();
        WriteAfterWriteProducesAMemoryBarrier();
        RepeatedDeclarationIsIdempotent();
        ResetReturnsEveryTargetToUndefined();
        TargetInBothSpansIsRejected();
        ChooseFormatTakesTheFirstSupportedCandidate();
        ChooseFormatFallsThroughToALaterCandidate();
        ChooseFormatRequiresEveryRequestedFeature();
        ChooseFormatThrowsWhenNothingQualifies();
        DeferredOrderRunsGeometryAoLightingForwardExposureThenTonemap();
        ForwardOnlyOrderSkipsTheDeferredPasses();
        GBufferTargetsAreColorTargets();
        AoTargetsAreStorageTargets();
        StorageWriteThenReadBecomesShaderRead();
        DepthFollowsTheDeferredWriteReadWriteSequence();

        std::cout << "scene pass tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "scene pass tests failed: " << error.what() << '\n';
        return 1;
    }
}
