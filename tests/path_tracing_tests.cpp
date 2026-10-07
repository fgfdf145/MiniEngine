#include <engine/renderer/path_tracing.h>
#include <engine/renderer/render_features.h>

#include <array>
#include <iostream>
#include <stdexcept>

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

PathTraceView TestView()
{
    PathTraceView view;
    view.view = glm::mat4(1.0f);
    view.view[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
    view.projection = glm::mat4(2.0f);
    view.width = 1280;
    view.height = 720;
    return view;
}

const std::array<glm::vec4, 2> kLighting = {glm::vec4(0.0f, -1.0f, 0.0f, 0.0f), glm::vec4(1.0f, 1.0f, 1.0f, 100000.0f)};

void FirstFrameIsNotStill()
{
    PathTraceAccumulation accumulation;
    Require(accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false) == 0, "the first frame has nothing to average with");
}

void StillFramesCount()
{
    PathTraceAccumulation accumulation;
    accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false);
    Require(accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false) == 1, "a repeated frame is still");
    Require(accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false) == 2, "still frames count up");
}

void AnyChangeStartsOver()
{
    PathTraceAccumulation accumulation;
    accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false);
    accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false);

    PathTraceView moved = TestView();
    moved.view[3].x += 1e-4f;
    Require(accumulation.Advance(moved, kLighting, RenderDebugSettings{}, false) == 0, "a moved camera starts over");
    Require(accumulation.Advance(moved, kLighting, RenderDebugSettings{}, false) == 1, "and stands still again");

    PathTraceView resized = moved;
    resized.width = 640;
    Require(accumulation.Advance(resized, kLighting, RenderDebugSettings{}, false) == 0, "a new render size starts over");

    std::array<glm::vec4, 2> sunMoved = kLighting;
    sunMoved[0].x = 1e-5f;
    accumulation.Advance(resized, kLighting, RenderDebugSettings{}, false);
    Require(accumulation.Advance(resized, sunMoved, RenderDebugSettings{}, false) == 0, "any change of the light starts over");
    Require(accumulation.Advance(resized, std::span<const glm::vec4>(sunMoved).first(1), RenderDebugSettings{}, false) == 0,
            "a light fewer starts over");

    accumulation.Advance(resized, kLighting, RenderDebugSettings{}, false);
    RenderDebugSettings settings;
    settings.pathTracing.maxBounces = 1;
    Require(accumulation.Advance(resized, kLighting, settings, false) == 0, "a settings change starts over");
    Require(accumulation.Advance(resized, kLighting, settings, true) == 0, "a scene change starts over");
}

void ResetStartsOver()
{
    PathTraceAccumulation accumulation;
    accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false);
    accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false);
    accumulation.Reset();
    Require(accumulation.Advance(TestView(), kLighting, RenderDebugSettings{}, false) == 0, "the frame after Reset is not still");
}

void HistoryCapGrowsWhileStill()
{
    PathTracingSettings settings;
    settings.motionFrames = 32;
    settings.maxFrames = 1000;
    Require(PathTraceHistoryCap(settings, 0) == 32, "moving: the motion cap");
    Require(PathTraceHistoryCap(settings, 10) == 42, "one more frame per still frame");
    Require(PathTraceHistoryCap(settings, 5000) == 1000, "no more than maxFrames");

    settings.maxFrames = 100000;
    Require(PathTraceHistoryCap(settings, 100000) == kPathTraceMaxFrames, "no more than a half float counts");
    settings.motionFrames = 0;
    settings.maxFrames = 0;
    Require(PathTraceHistoryCap(settings, 0) == 1, "at least the frame itself");
    settings.motionFrames = 64;
    settings.maxFrames = 16;
    Require(PathTraceHistoryCap(settings, 0) == 16, "the motion cap never exceeds maxFrames");
}

RenderCapabilities RayTracingGpu()
{
    RenderCapabilities capabilities;
    capabilities.rayQueries = true;
    capabilities.pathTracer = true;
    capabilities.restirPt = true;
    return capabilities;
}

void HybridRunsEveryEffect()
{
    const RenderFeatures features = ResolveRenderFeatures(RenderDebugSettings{}, RayTracingGpu());
    Require(features.rayTracedEffects && features.pathTracingAvailable && !features.pathTracing, "hybrid: traced effects, no paths");
    Require(features.ao && features.gi && features.ssr && features.taa && features.bloom && features.ddgi, "hybrid: every pass has its place");
    Require(features.rayTracedSunShadows && features.rayTracedLocalShadows && features.rayTracedReflections, "hybrid: traced shadows and reflections");
    Require(features.rayTracedAmbientOcclusion && features.probeOcclusion && features.occlusionRays, "hybrid: traced occlusion");
    Require(features.rayTracedShadowDenoise && !features.pathTraceAccumulate, "hybrid: shadow filters, no path filters");
}

void PathTracingReplacesTheAmbientTerms()
{
    RenderDebugSettings settings;
    settings.pathTracing.enabled = true;
    const RenderFeatures features = ResolveRenderFeatures(settings, RayTracingGpu());
    Require(features.pathTracing && features.plainPathTracing && !features.restirPt, "the plain path tracer runs");
    Require(!features.ao && !features.gi && !features.ssr, "no AO, GI or SSR under path tracing");
    Require(!features.rayTracedReflections && !features.rayTracedAmbientOcclusion && !features.probeOcclusion && !features.occlusionRays,
            "no traced reflections or occlusion under path tracing");
    Require(features.rayTracedSunShadows && features.rayTracedLocalShadows, "the direct light keeps its traced shadows");
    Require(features.ddgi && features.bloom && features.taa, "DDGI, bloom and TAA stay");
    Require(features.pathTraceAccumulate && features.pathTraceDenoise, "the paths accumulate and filter");

    settings.pathTracing.restir = true;
    const RenderFeatures restir = ResolveRenderFeatures(settings, RayTracingGpu());
    Require(restir.restirPt && !restir.plainPathTracing, "ReSTIR PT runs in the plain path tracer's place");
    Require(!restir.rayTracedSunShadows && !restir.rayTracedLocalShadows && !restir.rayTracedShadowDenoise, "ReSTIR PT shades the direct light too");
    Require(!restir.pathTraceAccumulate && !restir.pathTraceDenoise, "the plain path tracer's filters stand aside");
}

void PathTracingNeedsTracedEffects()
{
    RenderDebugSettings settings;
    settings.pathTracing.enabled = true;
    const RenderCapabilities noRays;
    Require(!ResolveRenderFeatures(settings, noRays).pathTracing, "no ray queries, no paths");
    RenderCapabilities notReady = RayTracingGpu();
    notReady.raySceneReady = false;
    Require(!ResolveRenderFeatures(settings, notReady).pathTracing, "not until the ray scene is ready");
    Require(ResolveRenderFeatures(settings, notReady).ao, "the hybrid passes cover until then");

    settings.hardwareRayTracing = false;
    const RenderFeatures switchedOff = ResolveRenderFeatures(settings, RayTracingGpu());
    Require(!switchedOff.hardwareRays && !switchedOff.rayTracedEffects && !switchedOff.pathTracing, "hardware rays switched off");
    Require(switchedOff.ao && switchedOff.ssr, "the screen-space passes come back");
}

void ComparisonViewsLeaveOutTheirPasses()
{
    RenderDebugSettings forward;
    forward.forwardOnly = true;
    forward.pathTracing.enabled = true;
    const RenderFeatures forwardOnly = ResolveRenderFeatures(forward, RayTracingGpu());
    Require(!forwardOnly.deferred && !forwardOnly.gbufferViews && !forwardOnly.taa, "forward only: no G-buffer, no TAA");
    Require(!forwardOnly.ao && !forwardOnly.gi && !forwardOnly.ssr && !forwardOnly.rayTracedEffects && !forwardOnly.pathTracingAvailable,
            "forward only: no deferred passes");
    Require(forwardOnly.bloom && forwardOnly.ddgi && forwardOnly.hardwareRays, "forward only keeps bloom and DDGI's rays");

    RenderDebugSettings khronos;
    khronos.khronosReference = true;
    const RenderFeatures reference = ResolveRenderFeatures(khronos, RayTracingGpu());
    Require(!reference.bloom && !reference.ddgi && !reference.ao && !reference.gi && !reference.ssr, "Khronos: no glare, GI, AO or SSR");
    Require(!reference.sunShadowMap && !reference.localLightShadows && !reference.specularAntiAliasing, "Khronos: no shadows or specular filter");
    Require(!reference.toneMapperChoice && !reference.rayTracedEffects, "Khronos: PBR Neutral, no traced effects");
    Require(reference.taa && reference.gbufferViews, "Khronos keeps the deferred order's TAA and views");
}

void ReflectionsNeedTaaHistory()
{
    RenderDebugSettings settings;
    settings.taa = false;
    const RenderFeatures noTaa = ResolveRenderFeatures(settings, RayTracingGpu());
    Require(!noTaa.ssr && !noTaa.rayTracedReflections, "no TAA history, no reflections");

    RenderCapabilities dlss = RayTracingGpu();
    dlss.dlss = true;
    const RenderFeatures upscaled = ResolveRenderFeatures(settings, dlss);
    Require(!upscaled.taa && !upscaled.renderScale && upscaled.dlssPreset, "DLSS takes TAA's place and the render size");
    Require(upscaled.ssr && upscaled.rayTracedReflections, "DLSS keeps the history reflections read");
}

void RayReconstructionDenoisesItself()
{
    RenderDebugSettings settings;
    settings.pathTracing.enabled = true;
    RenderCapabilities capabilities = RayTracingGpu();
    capabilities.dlss = true;
    capabilities.dlssRayReconstruction = true;
    const RenderFeatures features = ResolveRenderFeatures(settings, capabilities);
    Require(features.pathTracing && !features.pathTraceAccumulate && !features.pathTraceDenoise, "RR takes the raw paths");
    Require(!features.rayTracedShadowDenoise, "RR takes the raw traced shadow");
    capabilities.dlss = false;
    Require(ResolveRenderFeatures(settings, capabilities).pathTraceDenoise, "ray reconstruction only while DLSS resolves");
}

void SwitchesGateTheirTracedEffects()
{
    RenderDebugSettings settings;
    settings.localLightShadows = false;
    settings.ao.enabled = false;
    settings.ddgi.enabled = false;
    const RenderFeatures features = ResolveRenderFeatures(settings, RayTracingGpu());
    Require(!features.rayTracedLocalShadows && !features.rayTracedAmbientOcclusion && !features.probeOcclusion, "traced effects follow their passes");
    Require(!features.occlusionRays, "no traced occlusion, no occlusion rays");
    settings.rayTracing.sunShadows = false;
    Require(!ResolveRenderFeatures(settings, RayTracingGpu()).rayTracedShadowDenoise, "no traced sun shadow to filter");
}
}

int main()
{
    try
    {
        FirstFrameIsNotStill();
        StillFramesCount();
        AnyChangeStartsOver();
        ResetStartsOver();
        HistoryCapGrowsWhileStill();
        HybridRunsEveryEffect();
        PathTracingReplacesTheAmbientTerms();
        PathTracingNeedsTracedEffects();
        ComparisonViewsLeaveOutTheirPasses();
        ReflectionsNeedTaaHistory();
        RayReconstructionDenoisesItself();
        SwitchesGateTheirTracedEffects();
    }
    catch (const std::exception& error)
    {
        std::cerr << "path_tracing_tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "path_tracing_tests passed\n";
    return 0;
}
