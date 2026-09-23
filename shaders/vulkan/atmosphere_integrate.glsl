// Hillaire 2020's ray-marched in-scattering, shared by the multiple-scattering, sky-view and
// aerial perspective LUT shaders. Include after atmosphere_common.glsl.
#ifndef ATMOSPHERE_INTEGRATE_GLSL
#define ATMOSPHERE_INTEGRATE_GLSL

struct ScatteringResult
{
    vec3 luminance;
    vec3 transmittance;
    // The multiple-scattering LUT's f_ms: light scattered once more along the ray, for a unit
    // illuminance and an isotropic phase.
    vec3 multiScatteringAs1;
};

// Marches from origin along direction to the nearest of the atmosphere's edge, the ground and
// tMaxLimit, in sampleCount uniform steps, integrating each step's in-scattering analytically.
// anisotropicPhase uses the Rayleigh and Cornette-Shanks phases; otherwise the uniform one.
// useMultiScattering adds the multiple-scattering LUT's contribution; includeGround adds the
// sunlit ground's Lambertian reflection where the ray ends on it.
ScatteringResult IntegrateScatteredLuminance(
    vec3 origin,
    vec3 direction,
    vec3 sunDirection,
    vec3 illuminance,
    sampler2D transmittanceLut,
    sampler2D multiScatteringLut,
    bool includeGround,
    float sampleCount,
    float tMaxLimit,
    bool anisotropicPhase,
    bool useMultiScattering)
{
    ScatteringResult result;
    result.luminance = vec3(0.0);
    result.transmittance = vec3(1.0);
    result.multiScatteringAs1 = vec3(0.0);

    float tBottom = RaySphereIntersectNearest(origin, direction, vec3(0.0), BottomRadius());
    float tTop = RaySphereIntersectNearest(origin, direction, vec3(0.0), TopRadius());
    float tMax;
    if (tBottom < 0.0)
    {
        if (tTop < 0.0)
        {
            return result;
        }
        tMax = tTop;
    }
    else
    {
        tMax = tTop > 0.0 ? min(tTop, tBottom) : tBottom;
    }
    bool endsOnGround = tBottom >= 0.0 && tMax == tBottom && tBottom <= tMaxLimit;
    tMax = min(tMax, tMaxLimit);

    float cosTheta = dot(direction, sunDirection);
    float miePhase = CornetteShanksPhase(ubo.mieParameters.w, cosTheta);
    float rayleighPhase = RayleighPhase(cosTheta);
    const float uniformPhase = 1.0 / (4.0 * ATMOSPHERE_PI);

    float dt = tMax / sampleCount;
    vec3 throughput = vec3(1.0);
    for (float step = 0.0; step < sampleCount; step += 1.0)
    {
        float t = (step + 0.5) * dt;
        vec3 position = origin + t * direction;
        MediumSample medium = SampleMedium(position);
        vec3 extinction = max(medium.extinction, vec3(1e-6));
        vec3 stepTransmittance = exp(-medium.extinction * dt);

        float height = length(position);
        vec3 up = position / height;
        float cosSunZenith = dot(sunDirection, up);
        vec3 transmittanceToSun = SampleTransmittance(transmittanceLut, height, cosSunZenith);
        vec3 phaseTimesScattering = anisotropicPhase
                                        ? medium.mieScattering * miePhase + medium.rayleighScattering * rayleighPhase
                                        : medium.scattering * uniformPhase;
        float tEarth = RaySphereIntersectNearest(position, sunDirection, PLANET_RADIUS_OFFSET_KM * up, BottomRadius());
        float earthShadow = tEarth >= 0.0 ? 0.0 : 1.0;
        vec3 multiScattered = useMultiScattering
                                  ? SampleMultipleScattering(multiScatteringLut, height, cosSunZenith)
                                  : vec3(0.0);

        vec3 inScattering = illuminance * (earthShadow * transmittanceToSun * phaseTimesScattering + multiScattered * medium.scattering);
        // Energy-conserving analytic integration of S * T over the step (Hillaire 2015).
        result.luminance += throughput * (inScattering - inScattering * stepTransmittance) / extinction;
        result.multiScatteringAs1 += throughput * (medium.scattering - medium.scattering * stepTransmittance) / extinction;
        throughput *= stepTransmittance;
    }

    if (includeGround && endsOnGround)
    {
        vec3 position = origin + tBottom * direction;
        float height = length(position);
        vec3 up = position / height;
        float cosSunZenith = dot(sunDirection, up);
        vec3 transmittanceToSun = SampleTransmittance(transmittanceLut, height, cosSunZenith);
        float nDotL = clamp(dot(up, sunDirection), 0.0, 1.0);
        result.luminance += illuminance * transmittanceToSun * throughput * nDotL * ubo.groundAlbedo.rgb / ATMOSPHERE_PI;
    }

    result.transmittance = throughput;
    return result;
}

#endif
