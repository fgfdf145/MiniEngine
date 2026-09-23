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
// is the frame context's ao.enabled.
constexpr std::array<ScenePassId, 7> kDeferredOrder = {
    ScenePassId::Geometry,
    ScenePassId::AoTrace,
    ScenePassId::AoResolve,
    ScenePassId::Lighting,
    ScenePassId::Forward,
    ScenePassId::ExposureHistogram,
    ScenePassId::Tonemap};

constexpr std::array<ScenePassId, 3> kForwardOnlyOrder = {
    ScenePassId::Forward,
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
