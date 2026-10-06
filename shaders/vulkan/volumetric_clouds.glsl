// Volumetric clouds (docs/design/2026-09-28-volumetric-clouds-design.md): a cumulus layer in a
// shell around the planet, ray marched through plumes over a flat base cut with billows
// (docs/design/2026-10-06-cumulus-generation-design.md; cloud_weather.comp and cloud_noise.comp), lit by
// the atmosphere's sun (dual-lobe Henyey-Greenstein with Wrenninge's multiple-scattering octaves)
// and sky. The C++ mirror of the scalar functions is engine/renderer/volumetric_clouds.cpp.
// Units are kilometres, planet centre at the origin. Include after atmosphere_sampling.glsl.
#ifndef VOLUMETRIC_CLOUDS_GLSL
#define VOLUMETRIC_CLOUDS_GLSL

// Set 0 bindings 24, 25 and 27: the large and small billows and the plume map (VulkanAtmosphere),
// REPEAT and linear.
layout(set = 0, binding = 24) uniform sampler3D cloudShapeNoise;
layout(set = 0, binding = 25) uniform sampler3D cloudDetailNoise;
layout(set = 0, binding = 27) uniform sampler2D cloudWeatherMap;

// Must match kCloudScatteringOctaves and kCloudOctave* in engine/renderer/volumetric_clouds.h.
const int CLOUD_SCATTERING_OCTAVES = 5;
const float CLOUD_OCTAVE_SCATTERING = 0.8;
const float CLOUD_OCTAVE_EXTINCTION = 0.8;
const float CLOUD_OCTAVE_ANISOTROPY = 0.5;
// The march never reaches further than this; beyond it the haze has taken the clouds anyway.
const float CLOUD_MAX_DISTANCE_KM = 120.0;
// The small billows fade out by this distance: further away a step is longer than they are, and
// they would only alias.
const float CLOUD_DETAIL_DISTANCE_KM = 30.0;
// Toward the sun: steps from 25 m doubling, so the light finds the creases between billows and
// still crosses the whole layer (3.2 km). The first two read the small billows, the next two the
// large ones, the rest (400 m and longer) the plumes alone.
const int CLOUD_LIGHT_STEPS = 7;
const float CLOUD_LIGHT_FIRST_STEP_KM = 0.025;
const int CLOUD_LIGHT_DETAIL_STEPS = 2;
const int CLOUD_LIGHT_BILLOW_STEPS = 4;
// The view march: coarse steps through clear air read the plume map alone; where a billow could
// reach, fine steps, 30 m near the camera and longer with distance as a pixel grows, so the sharp
// surfaces are not sliced into bands. Never more iterations than this many times the coarse count.
const float CLOUD_FINE_STEP_KM = 0.015;
const float CLOUD_FINE_STEP_PER_KM = 0.002;
const int CLOUD_MARCH_BUDGET = 4;
const float CLOUD_DISTANCE_STEP_SHARE = 0.5;
// Away from the sun, for how much cloud the diffused light still has to cross: 100 m doubling.
const int CLOUD_AWAY_STEPS = 4;
const float CLOUD_AWAY_FIRST_STEP_KM = 0.1;
// Must match kCloudAmbientSteps and kCloudMaxDiffusionDecay in engine/renderer/volumetric_clouds.h.
const int CLOUD_AMBIENT_STEPS = 3;
const float CLOUD_MAX_DIFFUSION_DECAY = 0.95;
// Must match the plume map and billow constants in engine/renderer/volumetric_clouds.h.
const float CLOUD_WEATHER_FLOOR = -0.25;
const float CLOUD_WEATHER_SLOPE_SCALE = 256.0;
const float CLOUD_BILLOW_MEAN = 0.69;
const vec3 CLOUD_SHAPE_BILLOW_PER_TILE = vec3(0.05, 0.025, 0.0125);
const vec4 CLOUD_DETAIL_BILLOW_PER_TILE = vec4(0.2, 0.1, 0.05, 0.025);
const float CLOUD_EDGE_KM = 0.015;
const float CLOUD_WATER_FULL_HEIGHT_KM = 1.0;
const float CLOUD_WATER_AT_BASE = 0.25;
const float CLOUD_BILLOW_RISE_KM = 0.12;
const float CLOUD_BASE_RAGGEDNESS = 0.4;
// Must match kCloudBaseLargeBillows and kCloudBaseFadeKm.
const float CLOUD_BASE_LARGE_BILLOWS = 0.25;
const float CLOUD_BASE_FADE_KM = 0.1;
// Must match kCloudMinSunCosine: below it the sun's path down a column is too long to matter.
const float CLOUD_MIN_SUN_COSINE = 0.1;

bool CloudsEnabled()
{
    return EnvironmentMode() == ENVIRONMENT_ATMOSPHERE && ubo.cloudLayer.w > 0.0;
}

float CloudRemap(float x, float a, float b, float c, float d)
{
    float span = b - a;
    return span == 0.0 ? c : c + (x - a) / span * (d - c);
}

float CloudHenyeyGreenstein(float g, float cosTheta)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * ATMOSPHERE_PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-6), 1.5));
}

float CloudPhase(float forwardG, float backG, float backWeight, float cosTheta)
{
    return CloudHenyeyGreenstein(forwardG, cosTheta) * (1.0 - backWeight) + CloudHenyeyGreenstein(backG, cosTheta) * backWeight;
}

// Wrenninge's octaves: the first, single scattering, through lightOpticalDepth toward the sun; the
// rest, light already scattered, through scatteredOpticalDepth (CloudScatteredOpticalDepth).
float CloudSunScattering(float lightOpticalDepth, float scatteredOpticalDepth, float forwardG, float backG, float backWeight, float cosTheta)
{
    float scattering = 0.0;
    float a = 1.0;
    float b = 1.0;
    float c = 1.0;
    for (int octave = 0; octave < CLOUD_SCATTERING_OCTAVES; ++octave)
    {
        float depth = octave == 0 ? lightOpticalDepth : scatteredOpticalDepth;
        scattering += a * CloudPhase(forwardG * c, backG * c, backWeight, cosTheta) * exp(-b * depth);
        a *= CLOUD_OCTAVE_SCATTERING;
        b *= CLOUD_OCTAVE_EXTINCTION;
        c *= CLOUD_OCTAVE_ANISOTROPY;
    }
    return scattering;
}

// The optical depth scattered sunlight crosses to reach a point. In a closed deck (deckWeight 1,
// CloudDeckWeight) it need not take the slanted path toward the sun, which runs through the
// neighbouring towers: it can come down through the point's own column from its sunlit top
// (upOpticalDepth above the point, entered at sunCosine), whichever is less. Towers then cast their
// hard shadows in single scattering only, not as streaks across the deck. In a field of separate
// cumulus (deckWeight 0) the column is too narrow for that, and the slanted path stands.
float CloudScatteredOpticalDepth(float lightOpticalDepth, float upOpticalDepth, float sunCosine, float deckWeight)
{
    if (sunCosine <= 0.0)
    {
        return lightOpticalDepth;
    }
    float column = upOpticalDepth / max(sunCosine, CLOUD_MIN_SUN_COSINE);
    return mix(lightOpticalDepth, min(lightOpticalDepth, column), deckWeight);
}

// Lossless Eddington fluence per unit of beam illuminance, Marshak at both faces of a slab of
// scaled optical thickness total, scaled optical depth in from the lit face, the beam entering at
// cosine mu to the face's normal.
float CloudDiffuseFluence(float scaled, float total, float mu)
{
    float deep = mu * (3.0 * mu + 2.0);
    float beam = exp(-scaled / mu);
    float leaving = deep + (2.0 * mu - 3.0 * mu * mu) * exp(-total / mu);
    return max(deep - 3.0 * mu * mu * beam - leaving * (scaled + 2.0 / 3.0) / (total + 4.0 / 3.0), 0.0);
}

// The diffusion field (docs/design/2026-10-06-cloud-diffusion-and-ambient-occlusion-design.md,
// 2026-10-06-cumulus-generation-design.md): lossless Eddington across a slab lit along the ray,
// Marshak at both faces, awayOpticalDepth the cloud still ahead before the light leaves; per
// steradian per unit of scattering coefficient and of sun illuminance.
float CloudDiffuseScattering(float lightOpticalDepth, float awayOpticalDepth, float kappa, float similarity)
{
    float scaled = similarity * lightOpticalDepth;
    float total = similarity * (lightOpticalDepth + max(awayOpticalDepth, 0.0));
    return CloudDiffuseFluence(scaled, total, 1.0) * exp(-kappa * scaled) / (4.0 * ATMOSPHERE_PI);
}

// The same field across the cloud's own column, a horizontal slab lit from above at sunCosine:
// upOpticalDepth of it above the point, downOpticalDepth below. Deep in a deck the light has
// forgotten the sun's direction, so its column, not the slanted path to the sun, sets how much
// reaches the base.
float CloudSlabDiffuseScattering(float upOpticalDepth, float downOpticalDepth, float sunCosine, float kappa, float similarity)
{
    if (sunCosine <= 0.0)
    {
        return 0.0;
    }
    float scaled = similarity * max(upOpticalDepth, 0.0);
    float total = scaled + similarity * max(downOpticalDepth, 0.0);
    return CloudDiffuseFluence(scaled, total, sunCosine) * exp(-kappa * scaled) / (4.0 * ATMOSPHERE_PI);
}

// The octaves, raised by diffusion toward single scattering plus the diffusion field wherever
// that is brighter: deep in a thick cloud the octaves die out, the diffusion field does not.
// scatteredOpticalDepth is CloudScatteredOpticalDepth's; upOpticalDepth and downOpticalDepth are the
// point's own column above and below it, sunCosine the sun's height over its horizon: the
// diffusion field is the brighter of the field along the ray and across the column.
float CloudSunScatteringWithDiffusion(float lightOpticalDepth, float scatteredOpticalDepth, float awayOpticalDepth, float upOpticalDepth,
                                      float downOpticalDepth, float sunCosine, float forwardG, float backG, float backWeight,
                                      float cosTheta, float diffusion, float kappa, float similarity)
{
    float octaves = CloudSunScattering(lightOpticalDepth, scatteredOpticalDepth, forwardG, backG, backWeight, cosTheta);
    if (diffusion <= 0.0)
    {
        return octaves;
    }
    float single = CloudPhase(forwardG, backG, backWeight, cosTheta) * exp(-lightOpticalDepth);
    float field = max(CloudDiffuseScattering(lightOpticalDepth, awayOpticalDepth, kappa, similarity),
                      CloudSlabDiffuseScattering(upOpticalDepth, downOpticalDepth, sunCosine, kappa, similarity));
    float diffused = single + field;
    return octaves + diffusion * max(diffused - octaves, 0.0);
}

// Diffuse light through opticalDepth of conservatively scattering cloud (two-stream, Bohren 1987).
float CloudDiffuseTransmittance(float opticalDepth, float meanCosine)
{
    return 1.0 / (1.0 + 0.75 * (1.0 - meanCosine) * max(opticalDepth, 0.0));
}

bool CloudSphereRoots(vec3 origin, vec3 direction, float radius, out float nearT, out float farT)
{
    float b = dot(origin, direction);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - c;
    nearT = 0.0;
    farT = 0.0;
    if (discriminant < 0.0)
    {
        return false;
    }
    float root = sqrt(discriminant);
    nearT = -b - root;
    farT = -b + root;
    return true;
}

vec2 CloudShellInterval(vec3 origin, vec3 direction, float planet, float inner, float outer, float maxDistance)
{
    const vec2 none = vec2(0.0, -1.0);
    float radius = length(origin);
    float outerNear;
    float outerFar;
    if (!CloudSphereRoots(origin, direction, outer, outerNear, outerFar) || outerFar <= 0.0)
    {
        return none;
    }
    float innerNear;
    float innerFar;
    bool innerHit = CloudSphereRoots(origin, direction, inner, innerNear, innerFar);

    float start = 0.0;
    float end = outerFar;
    if (radius < inner)
    {
        float planetNear;
        float planetFar;
        if (CloudSphereRoots(origin, direction, planet, planetNear, planetFar) && planetNear > 0.0)
        {
            return none;
        }
        start = innerFar;
    }
    else
    {
        start = max(outerNear, 0.0);
        if (innerHit && innerNear > 0.0)
        {
            end = min(end, innerNear);
        }
    }
    start = max(start, 0.0);
    end = min(end, maxDistance);
    return end > start ? vec2(start, end) : none;
}

// Kilometres inside the surface before the billows: below the plume's top across its slope, and
// above the flat base.
float CloudSurfaceDistance(float top, float slope, float coverageOffset, float thicknessKm, float weatherFrequency, float heightKm)
{
    float topKm = (top - coverageOffset) * thicknessKm;
    float slopeKm = slope * thicknessKm * weatherFrequency;
    return min((topKm - heightKm) * inversesqrt(1.0 + slopeKm * slopeKm), heightKm);
}

// How far the billows push the surface out at heightKm above the base: none at the flat base.
float CloudBillows(vec4 shape, vec4 fine, float shapeTileKm, float detailTileKm, float strength, float detail, float heightKm)
{
    float large = dot(shape.rgb - CLOUD_BILLOW_MEAN, CLOUD_SHAPE_BILLOW_PER_TILE);
    float small = dot(fine - CLOUD_BILLOW_MEAN, CLOUD_DETAIL_BILLOW_PER_TILE);
    float rise = clamp(heightKm / CLOUD_BILLOW_RISE_KM, 0.0, 1.0);
    return strength * (rise * large * shapeTileKm + max(rise, CLOUD_BASE_RAGGEDNESS) * detail * small * detailTileKm);
}

// The furthest the billows lift the base: their reach there, with all the detail octaves.
float CloudBaseReachKm(float shapeTileKm, float detailTileKm, float strength)
{
    float large = CLOUD_BASE_LARGE_BILLOWS * dot(CLOUD_SHAPE_BILLOW_PER_TILE, vec3(1.0)) * shapeTileKm;
    float small = CLOUD_BASE_RAGGEDNESS * dot(CLOUD_DETAIL_BILLOW_PER_TILE, vec4(1.0)) * detailTileKm;
    return (1.0 - CLOUD_BILLOW_MEAN) * strength * (large + small);
}

// Density in [0, 1] at heightKm above the layer's floor from the base: the billows move it up and
// down about their reach above the floor (never below it, where the shell would cut it flat), and
// it fades in over CLOUD_BASE_FADE_KM rather than the sides' sharp edge.
float CloudBaseDensity(vec4 shape, vec4 fine, float shapeTileKm, float detailTileKm, float strength, float detail, float heightKm)
{
    float large = dot(shape.rgb - CLOUD_BILLOW_MEAN, CLOUD_SHAPE_BILLOW_PER_TILE);
    float small = dot(fine - CLOUD_BILLOW_MEAN, CLOUD_DETAIL_BILLOW_PER_TILE);
    float push = strength * (CLOUD_BASE_LARGE_BILLOWS * large * shapeTileKm + CLOUD_BASE_RAGGEDNESS * detail * small * detailTileKm);
    float baseKm = CloudBaseReachKm(shapeTileKm, detailTileKm, strength) - push;
    return clamp((heightKm - baseKm) / CLOUD_BASE_FADE_KM, 0.0, 1.0);
}

// The base without the billows, at its mean height: for the long light steps.
float CloudMeanBaseDensity(float heightKm)
{
    float baseKm = CloudBaseReachKm(1.0 / ubo.cloudScales.x, 1.0 / ubo.cloudScales.y, ubo.cloudScales.w);
    return clamp((heightKm - baseKm) / CLOUD_BASE_FADE_KM, 0.0, 1.0);
}

float CloudWaterProfile(float heightKm)
{
    return max(CLOUD_WATER_AT_BASE, pow(clamp(heightKm / CLOUD_WATER_FULL_HEIGHT_KM, 0.0, 1.0), 2.0 / 3.0));
}

float CloudEdgeDensity(float distanceKm)
{
    return clamp(distanceKm / CLOUD_EDGE_KM, 0.0, 1.0);
}

// CloudWaterProfile integrated from the base up to heightKm (km).
float CloudWaterColumn(float heightKm)
{
    float h = max(heightKm, 0.0);
    float baseTop = CLOUD_WATER_FULL_HEIGHT_KM * pow(CLOUD_WATER_AT_BASE, 1.5);
    if (h <= baseTop)
    {
        return CLOUD_WATER_AT_BASE * h;
    }
    float rising = 0.6 * pow(CLOUD_WATER_FULL_HEIGHT_KM, -2.0 / 3.0);
    float full = min(h, CLOUD_WATER_FULL_HEIGHT_KM);
    float column = CLOUD_WATER_AT_BASE * baseTop + rising * (pow(full, 5.0 / 3.0) - pow(baseTop, 5.0 / 3.0));
    return column + max(h - CLOUD_WATER_FULL_HEIGHT_KM, 0.0);
}

// Optical depth of a column of cloud from the base up to heightKm, extinctionPerKm where full:
// the plumes' smooth body, without the billows or the 15 m edge.
float CloudPlumeColumnDepth(float heightKm, float extinctionPerKm)
{
    return CloudWaterColumn(heightKm) * extinctionPerKm;
}

// The furthest the billows can push a surface out: the octaves' highest values above their mean.
float CloudBillowReachKm(float detail)
{
    float shapeTileKm = 1.0 / ubo.cloudScales.x;
    float detailTileKm = 1.0 / ubo.cloudScales.y;
    float reach = dot(CLOUD_SHAPE_BILLOW_PER_TILE, vec3(1.0)) * shapeTileKm + detail * dot(CLOUD_DETAIL_BILLOW_PER_TILE, vec4(1.0)) * detailTileKm;
    return (1.0 - CLOUD_BILLOW_MEAN) * ubo.cloudScales.w * reach;
}

// Kilometres inside the plumes' smooth surface (before the billows), from the plume map alone;
// far negative outside the layer.
float CloudPlumeDistance(vec3 positionKm, float heightFraction, out float columnTopKm)
{
    columnTopKm = 0.0;
    if (heightFraction <= 0.0 || heightFraction >= 1.0)
    {
        return -1e3;
    }
    float thicknessKm = ubo.cloudLayer.y;
    vec2 weather = textureLod(cloudWeatherMap, positionKm.xz * ubo.cloudScales.z, 0.0).rg;
    float top = weather.r * (1.0 - CLOUD_WEATHER_FLOOR) + CLOUD_WEATHER_FLOOR;
    columnTopKm = clamp((top - ubo.cloudLayer.z) * thicknessKm, 0.0, thicknessKm);
    return CloudSurfaceDistance(top, weather.g * CLOUD_WEATHER_SLOPE_SCALE, ubo.cloudLayer.z, thicknessKm, ubo.cloudScales.z, heightFraction * thicknessKm);
}

float CloudPlumeDistance(vec3 positionKm, float heightFraction)
{
    float columnTopKm;
    return CloudPlumeDistance(positionKm, heightFraction, columnTopKm);
}

// Extinction per km at a point of the layer, heightFraction its height in it, and how far inside
// the billowed surface it lies (km, negative outside). detail in [0, 1] fades the small billows;
// at 0 it is the cheaper cloud the far march and the long light steps read.
float CloudExtinction(vec3 positionKm, float heightFraction, float detail, out float distanceKm)
{
    distanceKm = CloudPlumeDistance(positionKm, heightFraction);
    // Further outside than any billow reaches: clear, without the volume taps.
    float reach = CloudBillowReachKm(detail);
    if (distanceKm < -reach)
    {
        distanceKm += reach;
        return 0.0;
    }
    float heightKm = heightFraction * ubo.cloudLayer.y;
    vec4 shape = textureLod(cloudShapeNoise, positionKm * ubo.cloudScales.x, 0.0);
    vec4 fine = detail > 0.0 ? textureLod(cloudDetailNoise, positionKm * ubo.cloudScales.y, 0.0) : vec4(CLOUD_BILLOW_MEAN);
    float shapeTileKm = 1.0 / ubo.cloudScales.x;
    float detailTileKm = 1.0 / ubo.cloudScales.y;
    distanceKm += CloudBillows(shape, fine, shapeTileKm, detailTileKm, ubo.cloudScales.w, detail, heightKm);
    float base = CloudBaseDensity(shape, fine, shapeTileKm, detailTileKm, ubo.cloudScales.w, detail, heightKm);
    return CloudEdgeDensity(distanceKm) * base * CloudWaterProfile(heightKm) * ubo.cloudLayer.w;
}

float CloudExtinction(vec3 positionKm, float heightFraction, float detail)
{
    float distanceKm;
    return CloudExtinction(positionKm, heightFraction, detail, distanceKm);
}

// The plumes alone, without the billows (which sit about the plumes' surface on average): for the
// long light steps, where a billow is far smaller than the step.
float CloudPlumeExtinction(vec3 positionKm, float heightFraction)
{
    float distanceKm = CloudPlumeDistance(positionKm, heightFraction);
    float heightKm = heightFraction * ubo.cloudLayer.y;
    return CloudEdgeDensity(distanceKm) * CloudMeanBaseDensity(heightKm) * CloudWaterProfile(heightKm) * ubo.cloudLayer.w;
}

// Optical depth from a point toward the sun, over steps that double in length. jitter in [0, 1)
// places the sample within each step: fixed at the middle, the steps would cut the shading of a
// smooth surface into contour bands; varied per pixel and frame, TAA averages them away.
float CloudLightOpticalDepth(vec3 positionKm, vec3 sunDirection, float inner, float thickness, float detail, float jitter)
{
    float stepLength = CLOUD_LIGHT_FIRST_STEP_KM;
    float t = 0.0;
    float depth = 0.0;
    for (int step = 0; step < CLOUD_LIGHT_STEPS; ++step)
    {
        vec3 p = positionKm + sunDirection * (t + stepLength * jitter);
        float heightFraction = (length(p) - inner) / thickness;
        if (heightFraction > 1.0)
        {
            break;
        }
        float extinction = step < CLOUD_LIGHT_BILLOW_STEPS ? CloudExtinction(p, heightFraction, step < CLOUD_LIGHT_DETAIL_STEPS ? detail : 0.0)
                                                           : CloudPlumeExtinction(p, heightFraction);
        depth += extinction * stepLength;
        t += stepLength;
        stepLength *= 2.0;
    }
    return depth;
}

// Optical depth (the plumes alone) from a point away from the sun until the light would leave the
// cloud: steps from 100 m doubling over 1.5 km, stopping at the layer's base or top; jitter as for
// the light march.
float CloudAwayOpticalDepth(vec3 positionKm, vec3 awayDirection, float inner, float thickness, float jitter)
{
    float stepLength = CLOUD_AWAY_FIRST_STEP_KM;
    float t = 0.0;
    float depth = 0.0;
    for (int step = 0; step < CLOUD_AWAY_STEPS; ++step)
    {
        vec3 p = positionKm + awayDirection * (t + stepLength * jitter);
        float heightFraction = (length(p) - inner) / thickness;
        if (heightFraction <= 0.0 || heightFraction >= 1.0)
        {
            break;
        }
        depth += CloudPlumeExtinction(p, heightFraction) * stepLength;
        t += stepLength;
        stepLength *= 2.0;
    }
    return depth;
}

// Optical depth (detail-free) along a straight run of length from a point: the column of cloud
// above or below it, for the sky and ground light it lets through.
float CloudColumnOpticalDepth(vec3 positionKm, vec3 direction, float runLength, float inner, float thickness)
{
    float stepLength = runLength / float(CLOUD_AMBIENT_STEPS);
    float depth = 0.0;
    for (int step = 0; step < CLOUD_AMBIENT_STEPS; ++step)
    {
        vec3 p = positionKm + direction * ((float(step) + 0.5) * stepLength);
        float heightFraction = (length(p) - inner) / thickness;
        depth += CloudExtinction(p, heightFraction, 0.0) * stepLength;
    }
    return depth;
}

// The clouds along direction from the camera: rgb the light they send toward it, a the
// transmittance through them; distanceKm is where they sit, weighted by what each step hides.
// jitter in [0, 1) offsets the first step; steps grow from minSteps overhead to maxSteps toward
// the horizon.
vec4 MarchClouds(vec3 direction, float jitter, int minSteps, int maxSteps, out float distanceKm)
{
    distanceKm = 0.0;
    vec3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float inner = BottomRadius() + ubo.cloudLayer.x;
    float thickness = ubo.cloudLayer.y;
    vec2 span = CloudShellInterval(camera, direction, BottomRadius(), inner, inner + thickness, CLOUD_MAX_DISTANCE_KM);
    if (span.y <= span.x)
    {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    float spanLength = span.y - span.x;
    int steps = int(mix(float(minSteps), float(maxSteps), clamp(spanLength / (4.0 * thickness), 0.0, 1.0)));
    float dt = spanLength / float(steps);

    vec3 sunDirection = ubo.sunDirectionAndMode.xyz;
    float cosTheta = dot(direction, sunDirection);
    // The sun and the sky as the layer sees them, once per ray, where it enters the layer.
    vec3 entry = camera + direction * (span.x + min(spanLength, thickness) * 0.5);
    float entryRadius = length(entry);
    vec3 entryUp = entry / entryRadius;
    vec3 sunLight = vec3(0.0);
    if (RaySphereIntersectNearest(entry, sunDirection, vec3(0.0), BottomRadius()) < 0.0)
    {
        sunLight = ubo.sunIlluminance.rgb * SampleTransmittance(atmosphereTransmittanceLut, entryRadius, dot(sunDirection, entryUp));
    }
    // The sky above the layer as a uniform upper hemisphere, and the lit ground below it.
    vec3 skyAbove = GroundSkyIrradiance() / ATMOSPHERE_PI;
    vec3 cameraUp = camera / length(camera);
    vec3 groundIrradiance = GroundSkyIrradiance() + GroundSunIrradiance(cameraUp) * max(dot(cameraUp, sunDirection), 0.0);
    // A seamless horizon has no ground to bounce light: below is the sky too.
    vec3 groundBelow = SeamlessHorizon() ? skyAbove : ubo.groundAlbedo.rgb * groundIrradiance / ATMOSPHERE_PI;
    float albedo = ubo.cloudPhase.w;
    float diffusion = ubo.cloudLighting.x;
    float ambientOcclusion = ubo.cloudLighting.y;
    float kappa = ubo.cloudLighting.z;
    float meanCosine = ubo.cloudLighting.w;
    float similarity = 1.0 - albedo * meanCosine;

    vec3 luminance = vec3(0.0);
    float transmittance = 1.0;
    float weightedDistance = 0.0;
    float weightSum = 0.0;
    float t = span.x + dt * jitter;
    float billowReach = CloudBillowReachKm(1.0);
    for (int iteration = 0; iteration < steps * CLOUD_MARCH_BUDGET && t < span.y; ++iteration)
    {
        vec3 p = camera + direction * t;
        float heightFraction = (length(p) - inner) / thickness;
        float columnTopKm;
        if (CloudPlumeDistance(p, heightFraction, columnTopKm) < -billowReach)
        {
            t += dt;
            continue;
        }
        float stepLength = min(dt, CLOUD_FINE_STEP_KM + t * CLOUD_FINE_STEP_PER_KM);
        float detail = clamp(1.0 - t / CLOUD_DETAIL_DISTANCE_KM, 0.0, 1.0);
        float surfaceDistance;
        float extinction = CloudExtinction(p, heightFraction, detail, surfaceDistance);
        // Outside the billowed surface, step by its distance: the field changes at most about
        // twice as fast as a true distance, so half of it cannot overshoot.
        if (extinction <= 0.0)
        {
            t += max(stepLength, -surfaceDistance * CLOUD_DISTANCE_STEP_SHARE);
            continue;
        }
        if (extinction > 0.0)
        {
            // A different place in the light steps for every view sample (golden-ratio sequence
            // from the pixel's jitter).
            float lightJitter = fract(jitter + float(iteration) * 0.618034);
            float lightDepth = CloudLightOpticalDepth(p, sunDirection, inner, thickness, detail, lightJitter);
            float awayDepth = diffusion > 0.0 ? CloudAwayOpticalDepth(p, -sunDirection, inner, thickness, lightJitter) : 0.0;
            // The point's own column, from the plume map alone: the cloud above it and below it.
            float heightKm = clamp(heightFraction, 0.0, 1.0) * thickness;
            float upDepth = CloudPlumeColumnDepth(max(columnTopKm, heightKm), ubo.cloudLayer.w) - CloudPlumeColumnDepth(heightKm, ubo.cloudLayer.w);
            float downDepth = CloudPlumeColumnDepth(heightKm, ubo.cloudLayer.w);
            float sunCosine = dot(sunDirection, p / length(p));
            float scatteredDepth = CloudScatteredOpticalDepth(lightDepth, upDepth, sunCosine, ubo.cloudParams.z);
            float sunScattering = CloudSunScatteringWithDiffusion(
                lightDepth, scatteredDepth, awayDepth, upDepth, downDepth, sunCosine, ubo.cloudPhase.x, ubo.cloudPhase.y, ubo.cloudPhase.z,
                cosTheta, diffusion, kappa, similarity);
            // The sky reaches the point through the cloud above it, the ground's light through
            // the cloud below.
            float skySeen = 1.0;
            float groundSeen = 1.0;
            if (ambientOcclusion > 0.0)
            {
                float h = clamp(heightFraction, 0.0, 1.0);
                vec3 up = p / length(p);
                float above = CloudColumnOpticalDepth(p, up, (1.0 - h) * thickness, inner, thickness);
                float below = CloudColumnOpticalDepth(p, -up, h * thickness, inner, thickness);
                skySeen = mix(1.0, CloudDiffuseTransmittance(above, meanCosine), ambientOcclusion);
                groundSeen = mix(1.0, CloudDiffuseTransmittance(below, meanCosine), ambientOcclusion);
            }
            vec3 ambient = mix(groundBelow * groundSeen, skyAbove * skySeen, clamp(heightFraction, 0.0, 1.0)) * ubo.cloudParams.x;
            float stepTransmittance = exp(-extinction * stepLength);
            // Hillaire 2016's energy-conserving step: the in-scattering integrated analytically
            // over the step's own extinction, which is sigma_s / sigma_t = albedo.
            vec3 scattered = albedo * (sunLight * sunScattering + ambient) * (1.0 - stepTransmittance);
            luminance += transmittance * scattered;
            float hidden = transmittance * (1.0 - stepTransmittance);
            weightedDistance += hidden * t;
            weightSum += hidden;
            transmittance *= stepTransmittance;
            if (transmittance < 0.003)
            {
                transmittance = 0.0;
                break;
            }
        }
        t += stepLength;
    }
    distanceKm = weightSum > 0.0 ? weightedDistance / weightSum : span.x;
    return vec4(luminance, transmittance);
}

// The clouds along direction as a layer over the sky: rgb the light they and the haze in front of
// them send toward the camera, a the transmittance, so the sky behind becomes sky * a + rgb.
// skyHaze is the air's own light without the sun's disk: distant clouds fade into it over the haze
// distance, which stands for the air between them and the camera.
vec4 CloudLayer(vec3 direction, vec3 skyHaze, float jitter, int minSteps, int maxSteps)
{
    float distanceKm;
    vec4 clouds = MarchClouds(direction, jitter, minSteps, maxSteps, distanceKm);
    float fade = exp(-distanceKm / max(ubo.cloudParams.y, 1e-3));
    return vec4(clouds.rgb * fade + skyHaze * (1.0 - clouds.a) * (1.0 - fade), clouds.a);
}

// The sky along direction with the clouds in front of it. skyLuminance is what lies behind them
// (with the sun's disk, which they hide).
vec3 ApplyClouds(vec3 skyLuminance, vec3 skyHaze, vec3 direction, float jitter, int minSteps, int maxSteps)
{
    if (!CloudsEnabled())
    {
        return skyLuminance;
    }
    vec4 layer = CloudLayer(direction, skyHaze, jitter, minSteps, maxSteps);
    return skyLuminance * layer.a + layer.rgb;
}

// The cloud targets (cloud_march.comp, cloud_resolve.comp) hold luminance times this: a fixed scale
// rather than the frame's exposure, so the history stays valid while the exposure adapts, and cloud
// lit toward the sun (~1e5 cd/m^2) still fits in half float.
const float CLOUD_TARGET_SCALE = 1.0 / 64.0;

// The TAA jitter as a shift in texture coordinates: ViewDirectionFromTexCoord(uv + CloudJitterUv())
// is the direction through the unjittered uv, the grid the cloud targets are laid out on. The
// jitter moves every point by the same amount in normalised device coordinates, so one point
// measures it.
vec2 CloudJitterUv()
{
    vec4 point = vec4(ubo.cameraWorldPosition.xyz + ViewDirectionFromTexCoord(vec2(0.5)) * 1000.0, 1.0);
    vec4 jittered = ubo.proj * ubo.view * point;
    vec4 plain = ubo.viewProjNoJitter * point;
    return (jittered.xy / jittered.w - plain.xy / plain.w) * 0.5;
}

// Interleaved gradient noise (Jimenez 2014), stepped per frame so TAA averages the march's jitter.
float CloudJitter(vec2 pixel)
{
    vec2 p = pixel + 5.588238 * mod(ubo.cloudParams.w, 64.0);
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

#endif
