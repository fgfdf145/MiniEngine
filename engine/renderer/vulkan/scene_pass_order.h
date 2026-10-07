#pragma once

// Deliberately includes no Vulkan header: this unit is pure logic compiled into a test that must
// not pull in SDL, GLM or the editor. See render_target_layout.h for the same reasoning.
#include <span>

namespace me
{

// Every scene pass the frame can record. The pass objects are owned in one list and looked up by
// this id, which keeps ownership and record order as two separate concerns: a viewport resize
// walks the ownership list, and recording walks the order.
enum class ScenePassId
{
    Geometry,
    // The ray traced sun shadow (rt_shadow_pass.h), which the lighting pass reads.
    RtShadow,
    // ReSTIR PT (restir_pt_pass.h): the path traced lighting the lighting pass adds while it runs.
    RestirPt,
    AoTrace,
    AoResolve,
    SsrTrace,
    SsrResolve,
    // The path traced indirect light (path_trace_pass.h), which the lighting pass reads in path
    // tracing mode in place of the ambient terms.
    PathTrace,
    Lighting,
    // One-bounce indirect diffuse (gi_pass.h): the trace over the lit image, its filter, and the
    // composite that adds it back to the lit image.
    GiTrace,
    GiResolve,
    GiComposite,
    // The DDGI debug views (ddgi_debug_pass.h), written over SceneGi once the composite has used it.
    DdgiDebug,
    // The scatter pre-pass of KHR_materials_volume_scatter (VulkanScatterPass), ahead of the forward
    // pass that samples it.
    Scatter,
    Forward,
    // The anime characters (toon_pass.h): their depth texture and eye mask, then their cel shading,
    // outlines and see-through hair over the opaque scene, before the transmission copy takes it.
    ToonPrepass,
    Toon,
    TransmissionCopy,
    ForwardTranslucent,
    Taa,
    Bloom,
    ExposureHistogram,
    Tonemap,
    // The editor's selection outline (selection_outline_pass.h): the selected entity's depth, then
    // the outline around it, into an image of its own that ImGui draws over the viewport.
    SelectionMask,
    SelectionOutline
};

// The passes to record, in order. Both orders end in the same exposure histogram, tone mapping and
// selection outline passes, so flipping the comparison switch isolates shading differences and never confounds them
// with metering or tone mapping. The directional shadow pass is not a scene pass: the renderer
// records it before whichever order this returns.
//
// The returned span points at storage with static lifetime, so it is valid for as long as the
// program and costs no allocation per frame.
std::span<const ScenePassId> BuildScenePassOrder(bool forwardOnly);

// The enumerator's name, for logs and timings. Static storage.
const char* ScenePassName(ScenePassId id);
}
