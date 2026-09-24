#include "scene_pass_order.h"

#include <array>

namespace me
{

namespace
{
// The forward pass appears in both orders. In the deferred order it receives only Blend draw
// items, which cannot be deferred; in the forward-only order it receives every item. That filter
// travels in ScenePassFrameContext, not here; this function decides order, not content. The same
// holds for the two AO passes: they are always in the deferred order, and whether they do any work
// is the frame context's ao.enabled. TAA is in both orders, so both meter and tone map its output;
// the forward-only order has no motion vectors, and there the pass only copies the image through.
constexpr std::array<ScenePassId, 9> kDeferredOrder = {
    ScenePassId::Geometry,
    ScenePassId::AoTrace,
    ScenePassId::AoResolve,
    ScenePassId::Lighting,
    ScenePassId::Forward,
    ScenePassId::Taa,
    ScenePassId::Bloom,
    ScenePassId::ExposureHistogram,
    ScenePassId::Tonemap};

constexpr std::array<ScenePassId, 5> kForwardOnlyOrder = {
    ScenePassId::Forward,
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
