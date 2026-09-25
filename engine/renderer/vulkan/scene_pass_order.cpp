#include "scene_pass_order.h"

#include <array>

namespace me
{

namespace
{
// The forward pass, the transmission copy and the translucent forward pass appear in both orders: the
// copy takes the scene once everything opaque and the sky are in it, and the translucent pass draws
// transmissive surfaces over it, then Blend. The copy does nothing on a frame without transmissive
// draws. The forward pass appears in both orders. In the deferred order it receives only Blend draw
// items, which cannot be deferred; in the forward-only order it receives every item. That filter
// travels in ScenePassFrameContext, not here; this function decides order, not content. The same
// holds for the two AO passes: they are always in the deferred order, and whether they do any work
// is the frame context's ao.enabled; the two SSR passes likewise follow ssr.enabled. TAA is in both orders, so both meter and tone map its output;
// the forward-only order has no motion vectors, and there the pass only copies the image through.
constexpr std::array<ScenePassId, 13> kDeferredOrder = {
    ScenePassId::Geometry,
    ScenePassId::AoTrace,
    ScenePassId::AoResolve,
    ScenePassId::SsrTrace,
    ScenePassId::SsrResolve,
    ScenePassId::Lighting,
    ScenePassId::Forward,
    ScenePassId::TransmissionCopy,
    ScenePassId::ForwardTranslucent,
    ScenePassId::Taa,
    ScenePassId::Bloom,
    ScenePassId::ExposureHistogram,
    ScenePassId::Tonemap};

constexpr std::array<ScenePassId, 7> kForwardOnlyOrder = {
    ScenePassId::Forward,
    ScenePassId::TransmissionCopy,
    ScenePassId::ForwardTranslucent,
    ScenePassId::Taa,
    ScenePassId::Bloom,
    ScenePassId::ExposureHistogram,
    ScenePassId::Tonemap};
}

std::span<const ScenePassId> BuildScenePassOrder(bool forwardOnly)
{
    return forwardOnly
               ? std::span<const ScenePassId>(kForwardOnlyOrder)
               : std::span<const ScenePassId>(kDeferredOrder);
}
}
