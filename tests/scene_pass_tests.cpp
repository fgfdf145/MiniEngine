#include <engine/renderer/vulkan/format_support.h>
#include <engine/renderer/vulkan/render_target_layout.h>

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

void AlreadyCorrectLayoutProducesNothing()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> hdr = {RenderTargetId::SceneHdr};
    tracker.Transition(MakeIo({}, hdr));

    const std::vector<TargetTransition> transitions = tracker.Transition(MakeIo({}, hdr));

    Require(transitions.empty(), "a target already in the required layout must not be transitioned");
}

void RepeatedDeclarationIsIdempotent()
{
    RenderTargetLayoutTracker tracker;
    const std::array<RenderTargetId, 1> reads = {RenderTargetId::SceneHdr};
    const std::array<RenderTargetId, 1> writes = {RenderTargetId::SceneLdr};

    const std::vector<TargetTransition> first = tracker.Transition(MakeIo(reads, writes));
    const std::vector<TargetTransition> second = tracker.Transition(MakeIo(reads, writes));

    Require(first.size() == 2, "the first application must transition both targets");
    Require(second.empty(), "applying the same declaration twice must transition once");
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
}

int main()
{
    try
    {
        UndefinedColorWriteBecomesColorAttachment();
        DepthWriteBecomesDepthAttachment();
        WrittenThenReadBecomesShaderRead();
        AlreadyCorrectLayoutProducesNothing();
        RepeatedDeclarationIsIdempotent();
        ResetReturnsEveryTargetToUndefined();
        TargetInBothSpansIsRejected();
        ChooseFormatTakesTheFirstSupportedCandidate();
        ChooseFormatFallsThroughToALaterCandidate();
        ChooseFormatRequiresEveryRequestedFeature();
        ChooseFormatThrowsWhenNothingQualifies();

        std::cout << "scene pass tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "scene pass tests failed: " << error.what() << '\n';
        return 1;
    }
}
