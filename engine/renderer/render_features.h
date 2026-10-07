#pragma once

#include <engine/renderer/render_types.h>

namespace me
{

// What the device and the backend can run. The renderer fills it from what it has this frame; the
// editor from what the backend reported, taking the ray scene as ready (its readiness comes and goes
// with streaming, and the settings should not flicker with it).
struct RenderCapabilities
{
    // The GPU has ray queries (hardware ray tracing).
    bool rayQueries = false;
    // The ray scene can be traced: its instances and textures are in place.
    bool raySceneReady = true;
    bool pathTracer = false;
    bool restirPt = false;
    // DLSS resolves the frame in place of TAA.
    bool dlss = false;
    bool dlssRayReconstruction = false;
};

// Which parts of the pipeline the settings leave a place for: what each of RenderDebugSettings'
// switches would change if it were on. The renderer turns its effects on as switch && feature, and the
// Graphics Debug panel greys out every switch whose feature is false, so the two cannot disagree on
// what runs. The path tracing flags are the ones that follow from the switches themselves.
struct RenderFeatures
{
    bool operator==(const RenderFeatures&) const = default;

    // The deferred order (not the forward-only comparison).
    bool deferred = true;
    // DDGI's probe rays and the effects below use ray queries.
    bool hardwareRays = false;
    // The ray traced effects can run: hardware rays, a ready ray scene, the deferred order and not the
    // Khronos reference view.
    bool rayTracedEffects = false;
    // Path tracing can run where it is switched on, in its plain or ReSTIR PT form as the settings pick.
    bool pathTracingAvailable = false;
    // Path tracing runs: its light replaces every ambient term (AO, GI, probe occlusion, reflections).
    bool pathTracing = false;
    // ReSTIR PT runs: it shades the deferred pixels' direct light too, so the traced shadows stand aside.
    bool restirPt = false;
    bool plainPathTracing = false;

    // Each effect's place, its own switch aside.
    bool taa = false;
    bool ao = false;
    bool gi = false;
    bool ssr = false;
    bool bloom = false;
    bool ddgi = false;
    bool sunShadowMap = false;
    bool localLightShadows = false;
    bool specularAntiAliasing = false;
    // The tone mapper the settings name (the Khronos reference view always uses PBR Neutral).
    bool toneMapperChoice = false;
    bool gbufferViews = false;
    bool renderScale = false;
    bool dlssPreset = false;

    bool rayTracedSunShadows = false;
    bool rayTracedLocalShadows = false;
    // Like SSR, only with TAA's history (TAA or DLSS resolving).
    bool rayTracedReflections = false;
    bool rayTracedAmbientOcclusion = false;
    bool probeOcclusion = false;
    // RayTracingSettings::occlusionRays: some traced occlusion runs.
    bool occlusionRays = false;
    // The traced sun shadow's filters (DLSS ray reconstruction denoises it itself).
    bool rayTracedShadowDenoise = false;
    // The plain path tracer's accumulation and filters (likewise left to ray reconstruction).
    bool pathTraceAccumulate = false;
    bool pathTraceDenoise = false;
};

RenderFeatures ResolveRenderFeatures(const RenderDebugSettings& settings, const RenderCapabilities& capabilities);
}
