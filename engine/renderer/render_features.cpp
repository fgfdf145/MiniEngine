#include "render_features.h"

#include "path_tracing.h"

namespace me
{

RenderFeatures ResolveRenderFeatures(const RenderDebugSettings& settings, const RenderCapabilities& capabilities)
{
    RenderFeatures features;
    features.deferred = !settings.forwardOnly;
    // The Khronos reference view renders what the Sample Viewer does: no shadows, AO, GI, SSR, glare,
    // DDGI or specular filtering, and PBR Neutral.
    const bool khronos = settings.khronosReference;
    const bool deferredOwn = features.deferred && !khronos;

    const PathTracingSettings pathTracing = EffectivePathTracing(settings.pathTracing);
    features.hardwareRays = capabilities.rayQueries && settings.hardwareRayTracing;
    features.rayTracedEffects = features.hardwareRays && capabilities.raySceneReady && deferredOwn;
    features.pathTracingAvailable =
        features.rayTracedEffects && (pathTracing.restir ? capabilities.restirPt : capabilities.pathTracer);
    features.pathTracing = pathTracing.enabled && features.pathTracingAvailable;
    features.restirPt = features.pathTracing && pathTracing.restir;
    features.plainPathTracing = features.pathTracing && !pathTracing.restir;
    features.offlinePathTracing = features.plainPathTracing && pathTracing.offline.enabled;

    const bool dlssRayReconstruction = capabilities.dlss && capabilities.dlssRayReconstruction;
    // The forward-only order has no motion vectors; DLSS takes TAA's place while it resolves.
    features.taa = features.deferred && !capabilities.dlss;
    features.renderScale = !capabilities.dlss;
    features.dlssPreset = capabilities.dlss;
    // Path tracing's light replaces every ambient term, so the passes that make them stand aside.
    features.ao = deferredOwn && !features.pathTracing;
    features.gi = deferredOwn && !features.pathTracing;
    // Reflections, marched or traced, take their colour from TAA's history.
    const bool taaHistory = capabilities.dlss || (settings.taa && features.taa);
    features.ssr = deferredOwn && !features.pathTracing && taaHistory;
    features.bloom = !khronos;
    // DDGI keeps updating under path tracing: the forward-shaded surfaces still take their ambient from it.
    features.ddgi = !khronos;
    features.sunShadowMap = !khronos;
    features.localLightShadows = !khronos;
    features.specularAntiAliasing = !khronos;
    features.toneMapperChoice = !khronos;
    features.gbufferViews = features.deferred;

    features.rayTracedSunShadows = features.rayTracedEffects && !features.restirPt && !features.offlinePathTracing;
    features.rayTracedLocalShadows = features.rayTracedSunShadows && settings.localLightShadows;
    features.rayTracedReflections = features.rayTracedEffects && !features.pathTracing && taaHistory;
    features.rayTracedAmbientOcclusion = features.rayTracedEffects && features.ao && settings.ao.enabled;
    features.probeOcclusion = features.rayTracedEffects && !features.pathTracing && features.ddgi && settings.ddgi.enabled;
    features.occlusionRays = (features.rayTracedAmbientOcclusion && settings.rayTracing.ambientOcclusion) ||
                             (features.probeOcclusion && settings.rayTracing.probeOcclusion);
    features.rayTracedShadowDenoise = features.rayTracedSunShadows && settings.rayTracing.sunShadows && !dlssRayReconstruction;
    features.pathTraceAccumulate = features.plainPathTracing && (!dlssRayReconstruction || features.offlinePathTracing);
    features.pathTraceDenoise = features.plainPathTracing && !dlssRayReconstruction;
    return features;
}
}
