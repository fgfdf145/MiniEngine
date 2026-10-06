#pragma once

#include <engine/scene/scene_environment.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace me
{

// Volumetric clouds (docs/design/2026-09-28-volumetric-clouds-design.md): a cumulus layer in a
// shell around the planet, ray marched; the clouds themselves are plumes over a flat base, cut
// with billows (docs/design/2026-10-06-cumulus-generation-design.md). The shaders mirror
// these functions line for line in shaders/vulkan/volumetric_clouds.glsl; units are kilometres,
// planet centre at the origin, as in the atmosphere.

// Multiple scattering (Wrenninge 2013, as Hillaire 2016 uses it): octave i scatters a^i of the
// light, sees b^i of the optical depth and c^i of the anisotropy. a <= b keeps it energy
// conserving. Hillaire's 0.5 and 3 octaves left the sunlit faces at half the ~30 000 cd/m^2 GT7
// measured (docs/references/gt7-rendering-notes.md, 1.1); 0.8 and 5 reach two thirds of it.
inline constexpr int kCloudScatteringOctaves = 5;
inline constexpr float kCloudOctaveScattering = 0.8f;
inline constexpr float kCloudOctaveExtinction = 0.8f;
inline constexpr float kCloudOctaveAnisotropy = 0.5f;

// The weather map (shaders/vulkan/cloud_weather.comp): per texel of one tile, the top of the
// tallest plume above the layer's base as a share of the thickness (r, stored as
// (top - kCloudWeatherFloor) / (1 - kCloudWeatherFloor)), and how steeply it falls, |d top / d uv|
// over kCloudWeatherSlopeScale (g). Outside every plume the top runs below the base, down to the
// floor, so the surface distance keeps falling away from a cloud's side.
inline constexpr uint32_t kCloudWeatherSize = 1024;
inline constexpr float kCloudWeatherFloor = -0.25f;
inline constexpr float kCloudWeatherSlopeScale = 256.0f;
// A plume's top over its footprint: t (1 - (d / r)^p), a paraboloid: a rounded top, sides at 63
// degrees where it meets the base, and on below the base outside the footprint.
inline constexpr float kCloudDomeExponent = 2.0f;
// Overlapping domes join with a polynomial smooth maximum this wide (share of the thickness,
// 150 m at 2.5 km): a plain maximum leaves a ledge where a wide low turret meets a tall narrow one,
// and the plumes read as stacked plates.
inline constexpr float kCloudDomeBlend = 0.06f;
// A plume's height (share of the thickness) per unit of its diameter in uv: the 40 km tile over
// the 2.5 km layer the plume sizes were drawn for, so an aspect of 1 is as tall as wide there.
inline constexpr float kCloudPlumeHeightPerUv = 16.0f;

// One scale of plumes on a jittered grid: each cell holds one with the probability (raised where
// the clustering field is high), of radius and aspect (height over diameter) drawn from the
// ranges, in cells. The larger scales are clusters of turrets: three to five sub-domes around a
// core. Four doublings, 2.5 km cells down to 300 m, so there are four times as many plumes per
// halving of size, as fair-weather cumulus fields show (a power law near D^-2 to D^-3).
struct CloudPlumeLevel
{
    float cells;
    float probability;
    float radiusMin;
    float radiusMax;
    float aspectMin;
    float aspectMax;
    bool turrets;
};
inline constexpr std::array<CloudPlumeLevel, 4> kCloudPlumeLevels = {{
    {16.0f, 0.55f, 0.30f, 0.45f, 0.45f, 0.80f, true},
    {32.0f, 0.55f, 0.28f, 0.45f, 0.40f, 0.75f, true},
    {64.0f, 0.55f, 0.25f, 0.45f, 0.35f, 0.70f, false},
    {128.0f, 0.55f, 0.25f, 0.45f, 0.30f, 0.60f, false},
}};

// The billows (shaders/vulkan/cloud_noise.comp): each channel of both volumes is a union of
// spheres around jittered points, sqrt(1 - d^2), stretched from its 0.45 floor onto [0, 1]; its
// mean is then kCloudBillowMean, so a billow pushes the surface out as often as in.
inline constexpr float kCloudBillowMean = 0.69f;
// Billow heights per tile size, kilometres per kilometre, octave by octave (spheres per tile: shape
// r g b 4, 8, 16; detail r g b a 2, 4, 8, 16): a fifth of each octave's sphere spacing in the
// shape, the large lobes, and two fifths in the detail, the cauliflower on them.
inline constexpr std::array<float, 3> kCloudShapeBillowPerTile = {0.05f, 0.025f, 0.0125f};
inline constexpr std::array<float, 4> kCloudDetailBillowPerTile = {0.2f, 0.1f, 0.05f, 0.025f};
// Where the density rises from nothing to full inside the surface: a cumulus's edge is sharp.
inline constexpr float kCloudEdgeKm = 0.015f;
// Liquid water rises with height above the base (adiabatic), and extinction with it as its 2/3
// power; at the base a fraction of the full value, which is reached this far up.
inline constexpr float kCloudWaterFullHeightKm = 1.0f;
inline constexpr float kCloudWaterAtBase = 0.25f;
// Billows grow in from the flat base: the large ones from none at it to all of them this far above;
// the small ones keep a share at the base, which leaves it ragged but level.
inline constexpr float kCloudBillowRiseKm = 0.12f;
inline constexpr float kCloudBaseRaggedness = 0.4f;

// Scattered sunlight may come down a point's own column instead of along the slanted path through
// its neighbours once the layer closes into a deck: from none at this coverage to all of it at
// full coverage (docs/design/2026-10-06-cumulus-generation-design.md, overcast).
inline constexpr float kCloudDeckCoverageStart = 0.6f;
// The sun's height over the horizon a column's path is measured at, at least: lower, the path
// down the column is so long the slanted one is shorter anyway.
inline constexpr float kCloudMinSunCosine = 0.1f;

// The vertical marches (detail-free) that measure the cloud above and below a point for its ambient
// light (docs/design/2026-10-06-cloud-diffusion-and-ambient-occlusion-design.md).
inline constexpr int kCloudAmbientSteps = 3;
// The diffusion field's decay per scaled optical depth never reaches 1 (single-scattering albedo
// 2/3), where the half-space solution's particular term diverges.
inline constexpr float kCloudMaxDiffusionDecay = 0.95f;

// The cloud shadow map (shaders/vulkan/cloud_shadow.glsl): the clouds' transmittance toward the sun
// per point of the ground, over a square centred on the camera. 31 m texels: the sun's disk seen
// from 1.5 km already blurs a shadow edge over 14 m.
inline constexpr uint32_t kCloudShadowMapSize = 512;
inline constexpr float kCloudShadowExtentMeters = 16000.0f;
// The sun's height (direction y) the projection to the ground never goes below, so a sun at the
// horizon does not throw every lookup off the map.
inline constexpr float kCloudShadowMinSunHeight = 0.05f;
// The map's outer share over which the shadow fades to none.
inline constexpr float kCloudShadowEdgeFade = 0.1f;

// The settings held to the ranges the editor offers, as BuildEnvironmentUniformData uploads them.
CloudSettings ClampCloudSettings(const CloudSettings& settings);

// Maps x from [a, b] to [c, d], unclamped; a == b maps everything to c.
float CloudRemap(float x, float a, float b, float c, float d);

// The weather map's texel at uv in [0, 1)^2, as cloud_weather.comp computes it: x the top
// (share of the thickness, kCloudWeatherFloor outside every plume), y |d top / d uv|.
glm::vec2 CloudWeatherTexel(const glm::vec2& uv);

// How far the plume tops are lowered so a share coverage of the ground lies under a cloud: the
// weather map's measured cover, inverted. Lowering them shrinks every plume and drops the
// smallest; at full coverage they sink below the floor and the layer closes into a deck.
float CloudCoverageOffset(float coverage);

// Kilometres inside the cloud's surface (negative outside) at heightKm above the base, before the
// billows: the lower of the distance below the plume's top, measured across its slope, and the
// height above the flat base. top and slope are the weather texel's; weatherFrequency is uv per km.
float CloudSurfaceDistance(float top, float slope, float coverageOffset, float thicknessKm, float weatherFrequency, float heightKm);

// How far the billows move the surface out (positive) or in at heightKm above the base: the shape
// volume's three octaves and the detail volume's four, each about its mean, scaled to the
// tiles (km) and by strength; detail in [0, 1] fades the detail octaves.
float CloudBillows(const glm::vec4& shape, const glm::vec4& fine, float shapeTileKm, float detailTileKm, float strength, float detail, float heightKm);

// The share of the full extinction at heightKm above the base: kCloudWaterAtBase rising as
// (h / kCloudWaterFullHeightKm)^(2/3) to 1.
float CloudWaterProfile(float heightKm);

// Density in [0, 1] distanceKm inside the surface: none outside, full kCloudEdgeKm in.
float CloudEdgeDensity(float distanceKm);

// CloudWaterProfile integrated from the base up to heightKm (km of full-density cloud).
float CloudWaterColumn(float heightKm);

// Optical depth of a column of the plumes' smooth body from the base up to heightKm, at
// extinctionPerKm where full (no billows, no edge).
float CloudPlumeColumnDepth(float heightKm, float extinctionPerKm);

// How far scattered sunlight may come down a point's own column rather than along the slanted
// path toward the sun: smoothstep from kCloudDeckCoverageStart to 1 of the coverage.
float CloudDeckWeight(float coverage);

// Dual-lobe Henyey-Greenstein per steradian; cosTheta is 1 looking into the sun.
float CloudPhase(float forwardG, float backG, float backWeight, float cosTheta);

// The sun's light scattered toward the camera per unit of scattering coefficient, after
// lightOpticalDepth of cloud toward the sun: the octave sum above.
float CloudSunScattering(float lightOpticalDepth, float forwardG, float backG, float backWeight, float cosTheta);

// The same with the first octave, single scattering, through lightOpticalDepth and the rest, light
// already scattered, through scatteredOpticalDepth (CloudScatteredOpticalDepth).
float CloudSunScattering(float lightOpticalDepth, float scatteredOpticalDepth, float forwardG, float backG, float backWeight, float cosTheta);

// The optical depth scattered sunlight crosses to reach a point: the slanted lightOpticalDepth,
// moved by deckWeight (CloudDeckWeight) toward the lesser of it and the path down the point's own
// column from its sunlit top, upOpticalDepth / sunCosine. In a deck the slanted path runs through
// the neighbouring towers and would draw their shadows as streaks across it.
float CloudScatteredOpticalDepth(float lightOpticalDepth, float upOpticalDepth, float sunCosine, float deckWeight);

// The dual lobe's mean cosine, held to [0, 0.95]: the anisotropy diffusion theory scales away.
float CloudMeanCosine(float forwardG, float backG, float backWeight);

// The similarity-scaled medium (f = g): x the diffusion field's decay kappa per scaled optical
// depth, sqrt(3 (1 - albedo')) held to kCloudMaxDiffusionDecay; y the scale 1 - albedo g from
// optical depth to scaled optical depth.
glm::vec2 CloudDiffusionParameters(float albedo, float meanCosine);

// The diffusion field (Eddington across a slab lit along the ray, Marshak at both faces) as light
// scattered per steradian per unit of scattering coefficient and of sun illuminance, after
// lightOpticalDepth of cloud toward the sun with awayOpticalDepth still ahead before the light
// leaves: isotropic, fluence / 4 pi, never negative. Deep in a thick cloud it fills toward 5 E;
// near the face the light leaves by, and in thin clouds, it escapes and falls toward nothing.
float CloudDiffuseScattering(float lightOpticalDepth, float awayOpticalDepth, float kappa, float similarity);

// Lossless Eddington fluence per unit of beam illuminance in a slab of scaled optical thickness
// total, scaled optical depth in from the lit face, the beam entering at cosine mu to the face's
// normal, Marshak at both faces: mu (3 mu + 2) deep in a thick slab, 2 mu at its lit face.
float CloudDiffuseFluence(float scaled, float total, float mu);

// The diffusion field across the point's own column, a horizontal slab lit from above at
// sunCosine with upOpticalDepth above the point and downOpticalDepth below; as
// CloudDiffuseScattering, and 0 with the sun below the horizon. Deep in a deck the light has
// forgotten the sun's direction, so the column, not the slanted path, sets what reaches the base.
float CloudSlabDiffuseScattering(float upOpticalDepth, float downOpticalDepth, float sunCosine, float kappa, float similarity);

// The sun's light scattered toward the camera: the octaves (single scattering through
// lightOpticalDepth, the rest through scatteredOpticalDepth), raised by diffusion in [0, 1] toward
// single scattering plus the diffusion field wherever that is brighter, so nothing is counted
// twice and the octaves alone stand at diffusion 0. The field is the brighter of the one along the
// ray (awayOpticalDepth ahead) and the one across the point's column (upOpticalDepth above,
// downOpticalDepth below, lit at sunCosine).
float CloudSunScatteringWithDiffusion(
    float lightOpticalDepth,
    float scatteredOpticalDepth,
    float awayOpticalDepth,
    float upOpticalDepth,
    float downOpticalDepth,
    float sunCosine,
    float forwardG,
    float backG,
    float backWeight,
    float cosTheta,
    float diffusion,
    float kappa,
    float similarity);

// Diffuse light through opticalDepth of conservatively scattering cloud (two-stream, Bohren 1987):
// 1 / (1 + 3/4 (1 - g) tau).
float CloudDiffuseTransmittance(float opticalDepth, float meanCosine);

// Where origin + t * direction (unit) runs inside the shell between the spheres of radius inner
// and outer around the planet centre, before it meets the planet of radius planet, clipped to
// [0, maxDistance]: x the entry, y the exit. y <= x when the ray never enters the shell in range.
// A ray that crosses the inner sphere downward and back (a camera above the layer looking through
// it at a slant) keeps only the first span.
// The map's centre in world x, z: the camera snapped to whole texels.
glm::vec2 CloudShadowMapCenter(const glm::vec3& cameraPosition);

// Where the ray from worldPosition toward the sun (directionToSun, unit) crosses world y = 0, as
// the map's uv around the camera.
glm::vec2 CloudShadowUv(const glm::vec3& worldPosition, const glm::vec3& directionToSun, const glm::vec3& cameraPosition);

// How much of the map's shadow applies at uv: 1 inside, falling to 0 over the outer edge fade.
float CloudShadowEdgeWeight(const glm::vec2& uv);

glm::vec2 CloudShellInterval(
    const glm::vec3& origin,
    const glm::vec3& direction,
    float planet,
    float inner,
    float outer,
    float maxDistance);
}
