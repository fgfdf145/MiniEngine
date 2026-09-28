// Volumetric clouds (docs/design/2026-09-28-volumetric-clouds-design.md): a cumulus layer in a
// shell around the planet, ray marched through the tiling noise cloud_noise.comp builds, lit by
// the atmosphere's sun (dual-lobe Henyey-Greenstein with Wrenninge's multiple-scattering octaves)
// and sky. The C++ mirror of the scalar functions is engine/renderer/volumetric_clouds.cpp.
// Units are kilometres, planet centre at the origin. Include after atmosphere_sampling.glsl.
#ifndef VOLUMETRIC_CLOUDS_GLSL
#define VOLUMETRIC_CLOUDS_GLSL

// Set 0 bindings 24 and 25: the shape and detail noise (VulkanAtmosphere), REPEAT and linear.
layout(set = 0, binding = 24) uniform sampler3D cloudShapeNoise;
layout(set = 0, binding = 25) uniform sampler3D cloudDetailNoise;

// Must match kCloudScatteringOctaves and kCloudOctave* in engine/renderer/volumetric_clouds.h.
const int CLOUD_SCATTERING_OCTAVES = 5;
const float CLOUD_OCTAVE_SCATTERING = 0.8;
const float CLOUD_OCTAVE_EXTINCTION = 0.8;
const float CLOUD_OCTAVE_ANISOTROPY = 0.5;
// The march never reaches further than this; beyond it the haze has taken the clouds anyway.
const float CLOUD_MAX_DISTANCE_KM = 120.0;
// The detail erosion fades out by this distance: further away a step is longer than the detail
// noise's features, which would only alias.
const float CLOUD_DETAIL_DISTANCE_KM = 30.0;
const int CLOUD_LIGHT_STEPS = 6;
// Must match kCloudEdgeWidth and kCloudWeatherShare in engine/renderer/volumetric_clouds.h.
const float CLOUD_EDGE_WIDTH = 0.2;
const float CLOUD_WEATHER_SHARE = 0.55;

bool CloudsEnabled()
{
    return EnvironmentMode() == ENVIRONMENT_ATMOSPHERE && ubo.cloudLayer.w > 0.0;
}

float CloudRemap(float x, float a, float b, float c, float d)
{
    float span = b - a;
    return span == 0.0 ? c : c + (x - a) / span * (d - c);
}

float CloudHeightGradient(float heightFraction)
{
    float base = clamp(CloudRemap(heightFraction, 0.0, 0.1, 0.0, 1.0), 0.0, 1.0);
    float top = clamp(CloudRemap(heightFraction, 0.3, 1.0, 1.0, 0.0), 0.0, 1.0);
    return base * top;
}

float CloudCoverageRamp(float field, float coverage)
{
    return coverage <= 0.0 ? 0.0 : clamp((field - (1.0 - coverage)) / CLOUD_EDGE_WIDTH, 0.0, 1.0);
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

float CloudSunScattering(float lightOpticalDepth, float forwardG, float backG, float backWeight, float cosTheta)
{
    float scattering = 0.0;
    float a = 1.0;
    float b = 1.0;
    float c = 1.0;
    for (int octave = 0; octave < CLOUD_SCATTERING_OCTAVES; ++octave)
    {
        scattering += a * CloudPhase(forwardG * c, backG * c, backWeight, cosTheta) * exp(-b * lightOpticalDepth);
        a *= CLOUD_OCTAVE_SCATTERING;
        b *= CLOUD_OCTAVE_EXTINCTION;
        c *= CLOUD_OCTAVE_ANISOTROPY;
    }
    return scattering;
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

float CloudWeather(float first, float second)
{
    return clamp(CloudRemap(first * 0.65 + second * 0.35, 0.25, 0.75, 0.0, 1.0), 0.0, 1.0);
}

// The base shape: the Perlin-Worley lobes, eroded at their edges by the Worley octaves.
float CloudShape(vec4 shape)
{
    float fbm = shape.g * 0.625 + shape.b * 0.25 + shape.a * 0.125;
    float erosion = (1.0 - fbm) * 0.6;
    return clamp(CloudRemap(shape.r, erosion, 1.0, 0.0, 1.0), 0.0, 1.0);
}

// The field the coverage thresholds: mostly weather, some shape, shaped by the height profile.
float CloudField(float weather, float shape, float gradient)
{
    return (weather * CLOUD_WEATHER_SHARE + shape * (1.0 - CLOUD_WEATHER_SHARE)) * gradient;
}

// Extinction per km at a point of the layer, heightFraction its height in it. detail in [0, 1]
// scales the detail erosion; at 0 it is the cheaper shape the far march and the light march read.
float CloudExtinction(vec3 positionKm, float heightFraction, float detail)
{
    float gradient = CloudHeightGradient(heightFraction);
    if (gradient <= 0.0)
    {
        return 0.0;
    }
    // The weather: two slices of the shape noise, read as a 2D map over the ground, where the
    // clouds gather.
    vec2 weatherUv = positionKm.xz * ubo.cloudScales.z;
    float weather = CloudWeather(
        textureLod(cloudShapeNoise, vec3(weatherUv, 0.37), 0.0).r,
        textureLod(cloudShapeNoise, vec3(weatherUv * 2.63 + 0.19, 0.71), 0.0).g);
    // Its lowest possible field still misses the coverage threshold: nothing here at any height.
    if (weather * CLOUD_WEATHER_SHARE + (1.0 - CLOUD_WEATHER_SHARE) <= 1.0 - ubo.cloudLayer.z)
    {
        return 0.0;
    }
    vec4 shape = textureLod(cloudShapeNoise, positionKm * ubo.cloudScales.x, 0.0);
    float field = CloudField(weather, CloudShape(shape), gradient);
    float cloud = CloudCoverageRamp(field, ubo.cloudLayer.z);
    if (detail > 0.0 && cloud > 0.0)
    {
        vec3 fine = textureLod(cloudDetailNoise, positionKm * ubo.cloudScales.y, 0.0).rgb;
        float fineFbm = fine.r * 0.625 + fine.g * 0.25 + fine.b * 0.125;
        // Wispy at the base, billowy toward the tops.
        float modifier = mix(fineFbm, 1.0 - fineFbm, clamp(heightFraction * 5.0, 0.0, 1.0));
        cloud = clamp(CloudRemap(cloud, modifier * ubo.cloudScales.w * detail, 1.0, 0.0, 1.0), 0.0, 1.0);
    }
    return cloud * ubo.cloudLayer.w;
}

// Optical depth from a point toward the sun, over steps that double in length.
float CloudLightOpticalDepth(vec3 positionKm, vec3 sunDirection, float inner, float thickness, float detail)
{
    float stepLength = thickness / 32.0;
    float t = 0.0;
    float depth = 0.0;
    for (int step = 0; step < CLOUD_LIGHT_STEPS; ++step)
    {
        vec3 p = positionKm + sunDirection * (t + stepLength * 0.5);
        float heightFraction = (length(p) - inner) / thickness;
        if (heightFraction > 1.0)
        {
            break;
        }
        depth += CloudExtinction(p, heightFraction, step < 2 ? detail : 0.0) * stepLength;
        t += stepLength;
        stepLength *= 2.0;
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
    vec3 groundBelow = ubo.groundAlbedo.rgb * groundIrradiance / ATMOSPHERE_PI;
    float albedo = ubo.cloudPhase.w;

    vec3 luminance = vec3(0.0);
    float transmittance = 1.0;
    float weightedDistance = 0.0;
    float weightSum = 0.0;
    float t = span.x + dt * jitter;
    for (int step = 0; step < steps; ++step)
    {
        vec3 p = camera + direction * t;
        float heightFraction = (length(p) - inner) / thickness;
        float detail = clamp(1.0 - t / CLOUD_DETAIL_DISTANCE_KM, 0.0, 1.0);
        float extinction = CloudExtinction(p, heightFraction, detail);
        if (extinction > 0.0)
        {
            float lightDepth = CloudLightOpticalDepth(p, sunDirection, inner, thickness, detail);
            float sunScattering = CloudSunScattering(lightDepth, ubo.cloudPhase.x, ubo.cloudPhase.y, ubo.cloudPhase.z, cosTheta);
            vec3 ambient = mix(groundBelow, skyAbove, clamp(heightFraction, 0.0, 1.0)) * ubo.cloudParams.x;
            float stepTransmittance = exp(-extinction * dt);
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
        t += dt;
    }
    distanceKm = weightSum > 0.0 ? weightedDistance / weightSum : span.x;
    return vec4(luminance, transmittance);
}

// The sky along direction with the clouds in front of it. skyLuminance is what lies behind them
// (with the sun's disk, which they hide), skyHaze the air's own light without the disk: distant
// clouds fade into it over the haze distance, which stands for the air between them and the camera.
vec3 ApplyClouds(vec3 skyLuminance, vec3 skyHaze, vec3 direction, float jitter, int minSteps, int maxSteps)
{
    if (!CloudsEnabled())
    {
        return skyLuminance;
    }
    float distanceKm;
    vec4 clouds = MarchClouds(direction, jitter, minSteps, maxSteps, distanceKm);
    float fade = exp(-distanceKm / max(ubo.cloudParams.y, 1e-3));
    return skyLuminance * clouds.a + clouds.rgb * fade + skyHaze * (1.0 - clouds.a) * (1.0 - fade);
}

// Interleaved gradient noise (Jimenez 2014), stepped per frame so TAA averages the march's jitter.
float CloudJitter(vec2 pixel)
{
    vec2 p = pixel + 5.588238 * mod(ubo.cloudParams.w, 64.0);
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

#endif
