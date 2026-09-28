#pragma once

#include <engine/scene/scene_environment.h>

#include <glm/glm.hpp>

namespace me
{

// Volumetric clouds (docs/design/2026-09-28-volumetric-clouds-design.md): a cumulus layer in a
// shell around the planet, ray marched through tiling Perlin-Worley noise. The shaders mirror
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
// The field's rise from the coverage threshold to full density: narrow, for cumulus's sharp edges.
inline constexpr float kCloudEdgeWidth = 0.2f;
// The weather map's share of the field; the base shape has the rest.
inline constexpr float kCloudWeatherShare = 0.55f;

// The settings held to the ranges the editor offers, as BuildEnvironmentUniformData uploads them.
CloudSettings ClampCloudSettings(const CloudSettings& settings);

// Maps x from [a, b] to [c, d], unclamped; a == b maps everything to c.
float CloudRemap(float x, float a, float b, float c, float d);

// The cumulus profile over the height fraction h in [0, 1] of the layer: a flat base that fills in
// over the lowest tenth and tops that thin out from 30 % up. Zero outside [0, 1].
float CloudHeightGradient(float heightFraction);

// The weather map from its two noise taps (the shape volume's r and g, normalised to [0, 1]),
// stretched over [0, 1]: where the clouds gather.
float CloudWeather(float first, float second);

// The base shape from a shape volume texel (r Perlin-Worley, g b a Worley octaves): the lobes
// eroded at their edges by the Worley fBm, in [0, 1].
float CloudShape(const glm::vec4& shape);

// The field the coverage thresholds: weather and shape blended, times the height profile.
float CloudField(float weather, float shape, float gradient);

// Density in [0, 1] where the field reads field: 0 below 1 - coverage, full kCloudEdgeWidth above
// it, and 0 everywhere at coverage 0.
float CloudCoverageRamp(float field, float coverage);

// Dual-lobe Henyey-Greenstein per steradian; cosTheta is 1 looking into the sun.
float CloudPhase(float forwardG, float backG, float backWeight, float cosTheta);

// The sun's light scattered toward the camera per unit of scattering coefficient, after
// lightOpticalDepth of cloud toward the sun: the octave sum above.
float CloudSunScattering(float lightOpticalDepth, float forwardG, float backG, float backWeight, float cosTheta);

// Where origin + t * direction (unit) runs inside the shell between the spheres of radius inner
// and outer around the planet centre, before it meets the planet of radius planet, clipped to
// [0, maxDistance]: x the entry, y the exit. y <= x when the ray never enters the shell in range.
// A ray that crosses the inner sphere downward and back (a camera above the layer looking through
// it at a slant) keeps only the first span.
glm::vec2 CloudShellInterval(
    const glm::vec3& origin,
    const glm::vec3& direction,
    float planet,
    float inner,
    float outer,
    float maxDistance);
}
