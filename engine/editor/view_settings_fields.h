#pragma once

#include <engine/renderer/exposure.h>
#include <engine/renderer/render_types.h>
#include <engine/renderer/white_balance.h>

namespace me
{

// The camera and renderer settings as (group, key, field) triples, one list each for every file that
// writes and reads them (the engine settings and the capture state), so a file's writer and reader
// cannot drift apart and a new field is added in one place. An empty group is a top-level field.

// Every RenderDebugSettings field but the G-buffer view and the tone mapper, which are enums the
// files write as numbers.
template <typename Visitor>
void VisitRenderDebugFields(RenderDebugSettings& settings, Visitor&& visit)
{
    visit("", "forward_only", settings.forwardOnly);
    visit("", "clustered_lighting", settings.clusteredLighting);
    visit("", "local_light_shadows", settings.localLightShadows);
    visit("", "hardware_ray_tracing", settings.hardwareRayTracing);
    visit("ray_tracing", "sun_shadows", settings.rayTracing.sunShadows);
    visit("ray_tracing", "ambient_occlusion", settings.rayTracing.ambientOcclusion);
    visit("ray_tracing", "probe_occlusion", settings.rayTracing.probeOcclusion);
    visit("ray_tracing", "occlusion_rays", settings.rayTracing.occlusionRays);
    visit("ray_tracing", "reflections", settings.rayTracing.reflections);
    visit("ray_tracing", "local_shadows", settings.rayTracing.localShadows);
    visit("ray_tracing", "denoise", settings.rayTracing.denoise);
    visit("path_tracing", "enabled", settings.pathTracing.enabled);
    visit("path_tracing", "max_bounces", settings.pathTracing.maxBounces);
    visit("path_tracing", "firefly_clamp", settings.pathTracing.fireflyClamp);
    visit("path_tracing", "light_candidates", settings.pathTracing.lightCandidates);
    visit("path_tracing", "accumulate", settings.pathTracing.accumulate);
    visit("path_tracing", "motion_frames", settings.pathTracing.motionFrames);
    visit("path_tracing", "max_frames", settings.pathTracing.maxFrames);
    visit("path_tracing", "denoise", settings.pathTracing.denoise);
    visit("path_tracing", "forward_surfaces", settings.pathTracing.forwardSurfaces);
    visit("path_tracing", "ray_media", settings.pathTracing.rayMedia);
    visit("path_tracing", "forward_surfaces_half_resolution", settings.pathTracing.forwardSurfacesHalfResolution);
    visit("path_tracing", "emissive_lights", settings.pathTracing.emissiveLights);
    visit("path_tracing", "light_grid", settings.pathTracing.lightGrid);
    visit("path_tracing", "reflection_guides", settings.pathTracing.reflectionGuides);
    visit("path_tracing", "restir", settings.pathTracing.restir);
    RestirPtSettings& restirPt = settings.pathTracing.restirPt;
    visit("restir_pt", "temporal_reuse", restirPt.temporalReuse);
    visit("restir_pt", "spatial_reuse", restirPt.spatialReuse);
    visit("restir_pt", "footprint_reconnection", restirPt.footprintReconnection);
    visit("restir_pt", "footprint_scale", restirPt.footprintScale);
    visit("restir_pt", "roughness_threshold", restirPt.roughnessThreshold);
    visit("restir_pt", "legacy_distance", restirPt.legacyDistance);
    visit("restir_pt", "decorrelation", restirPt.decorrelation);
    visit("restir_pt", "cap", restirPt.cap);
    visit("restir_pt", "cap_min", restirPt.capMin);
    visit("restir_pt", "cap_gamma", restirPt.capGamma);
    visit("restir_pt", "color_noise_reduction", restirPt.colorNoiseReduction);
    visit("restir_pt", "dual_motion_vectors", restirPt.dualMotionVectors);
    visit("restir_pt", "permutation_sampling", restirPt.permutationSampling);
    visit("restir_pt", "russian_roulette", restirPt.russianRoulette);
    visit("restir_pt", "accumulate", restirPt.accumulate);
    visit("restir_pt", "debug_view", restirPt.debugView);
    visit("", "shadow_distance", settings.shadowDistance);
    visit("", "taa", settings.taa);
    visit("", "specular_anti_aliasing", settings.specularAntiAliasing);
    visit("", "hdr_output", settings.hdrOutput);
    visit("", "hdr_peak_nits", settings.hdrPeakNits);
    visit("", "toon_exposure_ev", settings.toonExposureEv);
    visit("", "khronos_reference", settings.khronosReference);
    visit("", "render_scale", settings.renderScale);
    visit("viewport_resolution", "fixed", settings.viewportResolution.fixed);
    visit("viewport_resolution", "width", settings.viewportResolution.width);
    visit("viewport_resolution", "height", settings.viewportResolution.height);
    visit("", "dlss_ray_reconstruction", settings.dlssRayReconstruction);
    visit("bloom", "enabled", settings.bloom.enabled);
    visit("bloom", "strength", settings.bloom.strength);
    visit("ssr", "enabled", settings.ssr.enabled);
    visit("ssr", "max_roughness", settings.ssr.maxRoughness);
    visit("ssr", "max_distance", settings.ssr.maxDistance);
    visit("ao", "enabled", settings.ao.enabled);
    visit("ao", "radius", settings.ao.radius);
    visit("ao", "thickness", settings.ao.thickness);
    visit("ao", "slice_count", settings.ao.sliceCount);
    visit("ao", "step_count", settings.ao.stepCount);
    visit("ao", "spatial_filter", settings.ao.spatialFilter);
    visit("ao", "temporal_filter", settings.ao.temporalFilter);
    visit("gi", "enabled", settings.gi.enabled);
    visit("gi", "radius", settings.gi.radius);
    visit("gi", "thickness", settings.gi.thickness);
    visit("gi", "slice_count", settings.gi.sliceCount);
    visit("gi", "step_count", settings.gi.stepCount);
    visit("gi", "strength", settings.gi.strength);
    visit("gi", "spatial_filter", settings.gi.spatialFilter);
    visit("gi", "temporal_filter", settings.gi.temporalFilter);
    visit("ddgi", "enabled", settings.ddgi.enabled);
    visit("ddgi", "levels", settings.ddgi.levels);
    visit("ddgi", "base_spacing", settings.ddgi.baseSpacing);
    visit("ddgi", "probes_per_frame", settings.ddgi.probesPerFrame);
    visit("ddgi", "hysteresis", settings.ddgi.hysteresis);
    visit("ddgi", "normal_bias", settings.ddgi.normalBias);
    visit("ddgi", "view_bias", settings.ddgi.viewBias);
    visit("ddgi", "probe_view_level", settings.ddgi.probeViewLevel);
}

// The camera's auto exposure and auto white balance.
template <typename Visitor>
void VisitCameraAdaptationFields(AutoExposureSettings& exposure, AutoWhiteBalanceSettings& whiteBalance, Visitor&& visit)
{
    visit("auto_exposure", "enabled", exposure.enabled);
    visit("auto_exposure", "compensation_ev", exposure.compensationEv);
    visit("auto_exposure", "min_ev100", exposure.minEv100);
    visit("auto_exposure", "max_ev100", exposure.maxEv100);
    visit("auto_exposure", "low_percentile", exposure.lowPercentile);
    visit("auto_exposure", "high_percentile", exposure.highPercentile);
    visit("auto_exposure", "adapt_to_brighter_per_second", exposure.adaptToBrighterPerSecond);
    visit("auto_exposure", "adapt_to_darker_per_second", exposure.adaptToDarkerPerSecond);
    visit("auto_exposure", "short_term_range_ev", exposure.shortTermRangeEv);
    visit("auto_exposure", "long_term_to_brighter_per_second", exposure.longTermToBrighterPerSecond);
    visit("auto_exposure", "long_term_to_darker_per_second", exposure.longTermToDarkerPerSecond);
    visit("auto_exposure", "frame_reference_weight", exposure.frameReferenceWeight);
    visit("auto_exposure", "sun_reference_weight", exposure.sunReferenceWeight);
    visit("auto_exposure", "sky_reference_weight", exposure.skyReferenceWeight);
    visit("auto_white_balance", "enabled", whiteBalance.enabled);
    visit("auto_white_balance", "degree", whiteBalance.degree);
    visit("auto_white_balance", "adapt_per_second", whiteBalance.adaptPerSecond);
    visit("auto_white_balance", "target_kelvin", whiteBalance.targetKelvin);
}
}
