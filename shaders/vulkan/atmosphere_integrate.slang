// Hillaire 2020's ray-marched in-scattering, shared by the multiple-scattering, sky-view and
// aerial perspective LUT shaders. Include after atmosphere_common.slang.
#ifndef ATMOSPHERE_INTEGRATE_SLANG
#define ATMOSPHERE_INTEGRATE_SLANG

struct ScatteringResult
{
    float3 luminance;
    float3 transmittance;
    // The multiple-scattering LUT's f_ms: light scattered once more along the ray, for a unit
    // illuminance and an isotropic phase.
    float3 multiScatteringAs1;
};

// Marches from origin along direction to the nearest of the atmosphere's edge, the ground and
// tMaxLimit, in sampleCount uniform steps, integrating each step's in-scattering analytically.
// anisotropicPhase uses the Rayleigh and Cornette-Shanks phases; otherwise the uniform one.
// useMultiScattering adds the multiple-scattering LUT's contribution; includeGround adds the
// sunlit ground's Lambertian reflection where the ray ends on it.
ScatteringResult IntegrateScatteredLuminance<T : IAtmosphereLut, M : IAtmosphereLut>(
    float3 origin,
    float3 direction,
    float3 sunDirection,
    float3 illuminance,
    T transmittanceLut,
    M multiScatteringLut,
    bool includeGround,
    float sampleCount,
    float tMaxLimit,
    bool anisotropicPhase,
    bool useMultiScattering)
{
    ScatteringResult result;
    result.luminance = float3(0.0);
    result.transmittance = float3(1.0);
    result.multiScatteringAs1 = float3(0.0);

    float tBottom = RaySphereIntersectNearest(origin, direction, float3(0.0), BottomRadius());
    float tTop = RaySphereIntersectNearest(origin, direction, float3(0.0), TopRadius());
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
    float3 throughput = float3(1.0);
    for (float step = 0.0; step < sampleCount; step += 1.0)
    {
        float t = (step + 0.5) * dt;
        float3 position = origin + t * direction;
        MediumSample medium = SampleMedium(position);
        float3 extinction = max(medium.extinction, float3(1e-6));
        float3 stepTransmittance = exp(-medium.extinction * dt);

        float height = length(position);
        float3 up = position / height;
        float cosSunZenith = dot(sunDirection, up);
        float3 transmittanceToSun = SampleTransmittance(transmittanceLut, height, cosSunZenith);
        float3 phaseTimesScattering = anisotropicPhase
                                        ? medium.mieScattering * miePhase + medium.rayleighScattering * rayleighPhase
                                        : medium.scattering * uniformPhase;
        float tEarth = RaySphereIntersectNearest(position, sunDirection, PLANET_RADIUS_OFFSET_KM * up, BottomRadius());
        float earthShadow = tEarth >= 0.0 ? 0.0 : 1.0;
        float3 multiScattered = useMultiScattering
                                  ? SampleMultipleScattering(multiScatteringLut, height, cosSunZenith)
                                  : float3(0.0);

        float3 inScattering = illuminance * (earthShadow * transmittanceToSun * phaseTimesScattering + multiScattered * medium.scattering);
        // Energy-conserving analytic integration of S * T over the step (Hillaire 2015).
        result.luminance += throughput * (inScattering - inScattering * stepTransmittance) / extinction;
        result.multiScatteringAs1 += throughput * (medium.scattering - medium.scattering * stepTransmittance) / extinction;
        throughput *= stepTransmittance;
    }

    if (includeGround && endsOnGround)
    {
        float3 position = origin + tBottom * direction;
        float height = length(position);
        float3 up = position / height;
        float cosSunZenith = dot(sunDirection, up);
        float3 transmittanceToSun = SampleTransmittance(transmittanceLut, height, cosSunZenith);
        float nDotL = clamp(dot(up, sunDirection), 0.0, 1.0);
        result.luminance += illuminance * transmittanceToSun * throughput * nDotL * ubo.groundAlbedo.rgb / ATMOSPHERE_PI;
    }

    result.transmittance = throughput;
    return result;
}

#endif
