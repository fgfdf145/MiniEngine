// Exponential height fog (docs/design/2026-09-28-height-fog-design.md): a grey medium with
// extinction density * exp(-(y - fogHeight) * falloff) per metre, lit by the atmosphere's own light.
// Mirrors engine/renderer/height_fog.cpp line for line. Included by atmosphere_sampling.glsl after
// the sky SH is declared.
#ifndef HEIGHT_FOG_GLSL
#define HEIGHT_FOG_GLSL

// Must match kHeightFogMaxExponent in engine/renderer/height_fog.h.
const float HEIGHT_FOG_MAX_EXPONENT = 80.0;

// (1 - exp(-x)) / x for x >= 0; the series below 1e-2 keeps the precision the quotient loses.
float HeightFogSegmentAverage(float x)
{
    return x < 1e-2 ? 1.0 - x * 0.5 + x * x / 6.0 : (1.0 - exp(-x)) / x;
}

// Optical depth from the camera along the unit direction, from the start distance to distance,
// factored at the segment's denser end so neither end overflows or underflows.
float HeightFogOpticalDepth(vec3 direction, float distance)
{
    float density = ubo.heightFogDensity.x;
    float falloff = ubo.heightFogDensity.y;
    float startDistance = ubo.heightFogDensity.w;
    float segmentLength = distance - startDistance;
    if (segmentLength <= 0.0)
    {
        return 0.0;
    }
    float startHeight = ubo.cameraWorldPosition.y + startDistance * direction.y;
    float startExponent = -(startHeight - ubo.heightFogDensity.z) * falloff;
    float kL = falloff * direction.y * segmentLength;
    float denserExponent = min(startExponent + max(-kL, 0.0), HEIGHT_FOG_MAX_EXPONENT);
    return density * exp(denserExponent) * segmentLength * HeightFogSegmentAverage(abs(kL));
}

float HenyeyGreensteinPhase(float g, float cosTheta)
{
    float denominator = max(1.0 + g * g - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g * g) / (4.0 * ATMOSPHERE_PI * denominator * sqrt(denominator));
}

// The light the fog scatters toward the camera along direction, per unit opacity: the sky's average
// radiance (its radiance SH's L0 times Y00, the isotropic part) plus the sun at the camera through
// the Henyey-Greenstein lobe, tinted by the albedo. The sun's share (heightFogParams.yzw) is
// built once per frame by BuildEnvironmentUniformData. No shadowing inside the fog.
vec3 HeightFogInscatter(vec3 direction)
{
    vec3 skyAverage = max(skyIrradiance.coefficients[0].rgb * 0.282095, vec3(0.0));
    float phase = HenyeyGreensteinPhase(ubo.heightFogParams.x, dot(direction, ubo.sunDirectionAndMode.xyz));
    return ubo.heightFogColor.rgb * skyAverage + ubo.heightFogParams.yzw * phase;
}

vec3 BlendHeightFog(vec3 color, vec3 direction, float opticalDepth)
{
    float opacity = min(1.0 - exp(-opticalDepth), ubo.heightFogColor.w);
    return mix(color, HeightFogInscatter(direction), opacity);
}

// Fogs a surface's radiance by the fog between it and the camera.
vec3 ApplyHeightFog(vec3 color, vec3 worldPosition)
{
    if (ubo.heightFogDensity.x <= 0.0)
    {
        return color;
    }
    vec3 toSurface = worldPosition - ubo.cameraWorldPosition.xyz;
    float distance = length(toSurface);
    if (distance <= 0.0)
    {
        return color;
    }
    vec3 direction = toSurface / distance;
    return BlendHeightFog(color, direction, HeightFogOpticalDepth(direction, distance));
}

// Fogs the sky: the ray runs to infinity, so the depth is finite only while it climbs and the fog
// is opaque at and below the horizon, which is where surfaces far out meet it without a seam.
vec3 ApplyHeightFogToSky(vec3 color, vec3 direction)
{
    if (ubo.heightFogDensity.x <= 0.0)
    {
        return color;
    }
    if (direction.y <= 0.0)
    {
        return BlendHeightFog(color, direction, 1e30);
    }
    float falloff = ubo.heightFogDensity.y;
    float startHeight = ubo.cameraWorldPosition.y + ubo.heightFogDensity.w * direction.y;
    float startExponent = min(-(startHeight - ubo.heightFogDensity.z) * falloff, HEIGHT_FOG_MAX_EXPONENT);
    return BlendHeightFog(color, direction, ubo.heightFogDensity.x * exp(startExponent) / (falloff * direction.y));
}

#endif
