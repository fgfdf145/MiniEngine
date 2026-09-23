// Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique": the
// medium, ray-sphere intersection, phase functions and LUT parameterisations, shared by the LUT
// compute shaders and the fragment shaders that sample the LUTs. Every coefficient comes from the
// environment block of CameraBuffer, filled by BuildEnvironmentUniformData
// (engine/renderer/atmosphere.h); only the ozone profile and the LUT sizes are constants here.
//
// Units: kilometres, planet centre at the origin, +Y up. Include after scene_common.glsl.
#ifndef ATMOSPHERE_COMMON_GLSL
#define ATMOSPHERE_COMMON_GLSL

// Must match EnvironmentMode in engine/scene/scene_environment.h.
#define ENVIRONMENT_NONE 0u
#define ENVIRONMENT_ATMOSPHERE 1u
#define ENVIRONMENT_HDRI 2u

const float ATMOSPHERE_PI = 3.14159265358979;
// Must match the image sizes in engine/renderer/vulkan/atmosphere.cpp.
const vec2 TRANSMITTANCE_LUT_SIZE = vec2(256.0, 64.0);
const vec2 MULTI_SCATTERING_LUT_SIZE = vec2(32.0, 32.0);
const vec2 SKY_VIEW_LUT_SIZE = vec2(192.0, 108.0);
// Slices are spread quadratically: slice s (texel centre (s + 0.5) / 32) sits at
// ((s + 0.5) / 32)^2 * 32 km, so near slices are fine and the far ones reach 32 km.
const float AERIAL_PERSPECTIVE_SLICE_COUNT = 32.0;
const float AERIAL_PERSPECTIVE_KM_PER_SLICE = 1.0;
// Must match kOzoneCenterAltitudeKm and kOzoneHalfWidthKm in engine/renderer/atmosphere.h.
const float OZONE_CENTER_ALTITUDE_KM = 25.0;
const float OZONE_HALF_WIDTH_KM = 15.0;
// Lifts points off the ground so a ray that starts on it does not hit it again.
const float PLANET_RADIUS_OFFSET_KM = 0.01;

uint EnvironmentMode()
{
    return uint(ubo.sunDirectionAndMode.w + 0.5);
}

float BottomRadius()
{
    return ubo.atmosphereRadii.x;
}

float TopRadius()
{
    return ubo.atmosphereRadii.y;
}

struct MediumSample
{
    vec3 rayleighScattering;
    vec3 mieScattering;
    vec3 scattering;
    vec3 extinction;
};

MediumSample SampleMedium(vec3 positionKm)
{
    float altitude = max(length(positionKm) - BottomRadius(), 0.0);
    float rayleighDensity = exp(-altitude / ubo.rayleighScattering.w);
    float mieDensity = exp(-altitude / ubo.mieParameters.z);
    float ozoneDensity = max(0.0, 1.0 - abs(altitude - OZONE_CENTER_ALTITUDE_KM) / OZONE_HALF_WIDTH_KM);

    MediumSample medium;
    medium.rayleighScattering = ubo.rayleighScattering.rgb * rayleighDensity;
    medium.mieScattering = vec3(ubo.mieParameters.x * mieDensity);
    medium.scattering = medium.rayleighScattering + medium.mieScattering;
    medium.extinction =
        medium.rayleighScattering +
        vec3(ubo.mieParameters.y * mieDensity) +
        ubo.ozoneAbsorption.rgb * ozoneDensity;
    return medium;
}

// Distance along the ray to the nearest intersection in front of the origin, or -1 for none.
float RaySphereIntersectNearest(vec3 origin, vec3 direction, vec3 center, float radius)
{
    vec3 offset = origin - center;
    float b = dot(direction, offset);
    float c = dot(offset, offset) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0)
    {
        return -1.0;
    }
    float root = sqrt(discriminant);
    float near = -b - root;
    float far = -b + root;
    if (near < 0.0 && far < 0.0)
    {
        return -1.0;
    }
    if (near < 0.0)
    {
        return max(0.0, far);
    }
    return max(0.0, near);
}

float RayleighPhase(float cosTheta)
{
    return 3.0 / (16.0 * ATMOSPHERE_PI) * (1.0 + cosTheta * cosTheta);
}

// cosTheta between the view direction and the direction toward the sun: 1 looks into the sun,
// where forward scattering peaks.
float CornetteShanksPhase(float g, float cosTheta)
{
    float k = 3.0 / (8.0 * ATMOSPHERE_PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + cosTheta * cosTheta) / pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1e-4), 1.5);
}

// Keep lookups at texel centres at the LUT edges (Hillaire's sub-UV mapping).
vec2 FromUnitToSubUvs(vec2 uv, vec2 size)
{
    return (uv + 0.5 / size) * (size / (size + 1.0));
}

vec2 FromSubUvsToUnit(vec2 uv, vec2 size)
{
    return (uv - 0.5 / size) * (size / (size - 1.0));
}

// Bruneton's transmittance parameterisation: x the distance to the top boundary between its
// minimum and maximum for this height, y the height as a fraction of the horizon distance.
vec2 TransmittanceLutParamsToUv(float viewHeight, float cosZenith)
{
    float bottom = BottomRadius();
    float top = TopRadius();
    float H = sqrt(max(0.0, top * top - bottom * bottom));
    float rho = sqrt(max(0.0, viewHeight * viewHeight - bottom * bottom));
    float discriminant = viewHeight * viewHeight * (cosZenith * cosZenith - 1.0) + top * top;
    float d = max(0.0, -viewHeight * cosZenith + sqrt(max(discriminant, 0.0)));
    float dMin = top - viewHeight;
    float dMax = rho + H;
    return vec2((d - dMin) / max(dMax - dMin, 1e-6), rho / H);
}

void UvToTransmittanceLutParams(vec2 uv, out float viewHeight, out float cosZenith)
{
    float bottom = BottomRadius();
    float top = TopRadius();
    float H = sqrt(max(0.0, top * top - bottom * bottom));
    float rho = H * uv.y;
    viewHeight = sqrt(rho * rho + bottom * bottom);
    float dMin = top - viewHeight;
    float dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    cosZenith = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * viewHeight * d);
    cosZenith = clamp(cosZenith, -1.0, 1.0);
}

vec3 SampleTransmittance(sampler2D lut, float viewHeight, float cosZenith)
{
    return textureLod(lut, TransmittanceLutParamsToUv(viewHeight, cosZenith), 0.0).rgb;
}

vec3 SampleMultipleScattering(sampler2D lut, float viewHeight, float cosSunZenith)
{
    vec2 uv = clamp(
        vec2(cosSunZenith * 0.5 + 0.5, (viewHeight - BottomRadius()) / (TopRadius() - BottomRadius())),
        vec2(0.0),
        vec2(1.0));
    return textureLod(lut, FromUnitToSubUvs(uv, MULTI_SCATTERING_LUT_SIZE), 0.0).rgb;
}

// Sky-view parameterisation: y concentrates texels at the horizon (above it in [0, 0.5), below in
// [0.5, 1]), x is the azimuth relative to the sun as sqrt of (1 - cos) / 2.
void UvToSkyViewLutParams(vec2 uv, float viewHeight, out float viewZenithCos, out float lightViewCos)
{
    uv = FromSubUvsToUnit(uv, SKY_VIEW_LUT_SIZE);
    float horizonDistance = sqrt(max(0.0, viewHeight * viewHeight - BottomRadius() * BottomRadius()));
    float beta = acos(clamp(horizonDistance / viewHeight, -1.0, 1.0));
    float zenithHorizonAngle = ATMOSPHERE_PI - beta;
    if (uv.y < 0.5)
    {
        float coord = 1.0 - 2.0 * uv.y;
        coord = 1.0 - coord * coord;
        viewZenithCos = cos(zenithHorizonAngle * coord);
    }
    else
    {
        float coord = uv.y * 2.0 - 1.0;
        coord *= coord;
        viewZenithCos = cos(zenithHorizonAngle + beta * coord);
    }
    float coord = uv.x * uv.x;
    lightViewCos = -(coord * 2.0 - 1.0);
}

vec2 SkyViewLutParamsToUv(bool intersectGround, float viewZenithCos, float lightViewCos, float viewHeight)
{
    float horizonDistance = sqrt(max(0.0, viewHeight * viewHeight - BottomRadius() * BottomRadius()));
    float beta = acos(clamp(horizonDistance / viewHeight, -1.0, 1.0));
    float zenithHorizonAngle = ATMOSPHERE_PI - beta;
    float viewZenithAngle = acos(clamp(viewZenithCos, -1.0, 1.0));
    vec2 uv;
    if (!intersectGround)
    {
        float coord = clamp(viewZenithAngle / zenithHorizonAngle, 0.0, 1.0);
        coord = 1.0 - sqrt(max(0.0, 1.0 - coord));
        uv.y = coord * 0.5;
    }
    else
    {
        float coord = clamp((viewZenithAngle - zenithHorizonAngle) / beta, 0.0, 1.0);
        uv.y = sqrt(coord) * 0.5 + 0.5;
    }
    uv.x = sqrt(clamp(-lightViewCos * 0.5 + 0.5, 0.0, 1.0));
    return FromUnitToSubUvs(uv, SKY_VIEW_LUT_SIZE);
}

#endif
