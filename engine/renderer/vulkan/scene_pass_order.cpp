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
// is the frame context's ao.enabled; the two SSR passes likewise follow ssr.enabled. The scatter pre-pass
// is in both, right before the forward pass whose scattering materials sample it; it draws nothing on
// a frame without them. TAA is in both orders, so both meter and tone map its output;
// the forward-only order has no motion vectors, and there the pass only copies the image through.
// The three GI passes follow lighting, whose image is the light they bounce, and come before the
// forward pass, whose surfaces they do not light; like AO they are in the deferred order only. The
// DDGI debug views follow the composite, whose input they overwrite for the tone mapping pass to show.
// The ray traced sun shadow follows the geometry pass, whose depth and normals it traces from, and is in
// the deferred order only; it writes a neutral result while it does not trace. ReSTIR PT follows it, as
// it reads the same G-buffer, and precedes the lighting pass that adds its result; it records nothing
// while it is off.
// The path tracer follows the reflection resolve, whose target it overwrites in path tracing mode (as
// it does the indirect diffuse's, which the GI resolve writes only after lighting has read it), and
// precedes the lighting that remodulates it; deferred order only, and it records nothing while off.
// Right before it, the forward-shaded surfaces' layer it traces as well, over the finished depth.
// The toon passes follow the forward pass in both: the anime characters are cel shaded over the opaque
// scene (their opaque surfaces are in the G-buffer, which gives them depth, normals and motion) and
// come before the transmission copy, so glass shows them.
// The selection outline comes last in both: it reads the finished scene depth and writes an image of
// its own, which nothing in the scene reads.
constexpr std::array<ScenePassId, 26> kDeferredOrder = {
    ScenePassId::Geometry,
    ScenePassId::RtShadow,
    ScenePassId::RestirPt,
    ScenePassId::AoTrace,
    ScenePassId::AoResolve,
    ScenePassId::SsrTrace,
    ScenePassId::SsrResolve,
    ScenePassId::PathTraceLayer,
    ScenePassId::PathTrace,
    ScenePassId::Lighting,
    ScenePassId::GiTrace,
    ScenePassId::GiResolve,
    ScenePassId::GiComposite,
    ScenePassId::DdgiDebug,
    ScenePassId::Scatter,
    ScenePassId::Forward,
    ScenePassId::ToonPrepass,
    ScenePassId::Toon,
    ScenePassId::TransmissionCopy,
    ScenePassId::ForwardTranslucent,
    ScenePassId::Taa,
    ScenePassId::Bloom,
    ScenePassId::ExposureHistogram,
    ScenePassId::Tonemap,
    ScenePassId::SelectionMask,
    ScenePassId::SelectionOutline};

constexpr std::array<ScenePassId, 12> kForwardOnlyOrder = {
    ScenePassId::Scatter,
    ScenePassId::Forward,
    ScenePassId::ToonPrepass,
    ScenePassId::Toon,
    ScenePassId::TransmissionCopy,
    ScenePassId::ForwardTranslucent,
    ScenePassId::Taa,
    ScenePassId::Bloom,
    ScenePassId::ExposureHistogram,
    ScenePassId::Tonemap,
    ScenePassId::SelectionMask,
    ScenePassId::SelectionOutline};
}

const char* ScenePassName(ScenePassId id)
{
    switch (id)
    {
    case ScenePassId::Geometry:
        return "Geometry";
    case ScenePassId::RtShadow:
        return "RtShadow";
    case ScenePassId::RestirPt:
        return "RestirPt";
    case ScenePassId::AoTrace:
        return "AoTrace";
    case ScenePassId::AoResolve:
        return "AoResolve";
    case ScenePassId::SsrTrace:
        return "SsrTrace";
    case ScenePassId::SsrResolve:
        return "SsrResolve";
    case ScenePassId::PathTraceLayer:
        return "PathTraceLayer";
    case ScenePassId::PathTrace:
        return "PathTrace";
    case ScenePassId::Lighting:
        return "Lighting";
    case ScenePassId::GiTrace:
        return "GiTrace";
    case ScenePassId::GiResolve:
        return "GiResolve";
    case ScenePassId::GiComposite:
        return "GiComposite";
    case ScenePassId::DdgiDebug:
        return "DdgiDebug";
    case ScenePassId::Scatter:
        return "Scatter";
    case ScenePassId::Forward:
        return "Forward";
    case ScenePassId::ToonPrepass:
        return "ToonPrepass";
    case ScenePassId::Toon:
        return "Toon";
    case ScenePassId::TransmissionCopy:
        return "TransmissionCopy";
    case ScenePassId::ForwardTranslucent:
        return "ForwardTranslucent";
    case ScenePassId::Taa:
        return "Taa";
    case ScenePassId::Bloom:
        return "Bloom";
    case ScenePassId::ExposureHistogram:
        return "ExposureHistogram";
    case ScenePassId::Tonemap:
        return "Tonemap";
    case ScenePassId::SelectionMask:
        return "SelectionMask";
    case ScenePassId::SelectionOutline:
        return "SelectionOutline";
    }
    return "Unknown";
}

std::span<const ScenePassId> BuildScenePassOrder(bool forwardOnly)
{
    return forwardOnly
               ? std::span<const ScenePassId>(kForwardOnlyOrder)
               : std::span<const ScenePassId>(kDeferredOrder);
}
}
