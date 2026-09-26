// KHR_materials_transmission and KHR_materials_volume, as the Khronos glTF Sample Viewer samples
// them: what a transmissive surface shows is the scene behind it, copied once everything opaque and
// the sky are drawn (VulkanTransmissionCopyPass), read where the refracted ray leaves the volume.
//
// Compiled twice, like gt7_tonemap.glsl: by glslc from triangle.frag and by
// tests/transmission_tests.cpp inside a namespace with `using namespace glm`, so it keeps to the
// subset both accept (f-suffixed literals, no out parameters).
#ifndef TRANSMISSION_COMMON_GLSL
#define TRANSMISSION_COMMON_GLSL

// The transmission copy's size, fixed as the viewer's is: the LOD rule below depends on it.
const float kTransmissionCopySize = 1024.0f;

// Where the view ray leaves the medium: straight through a thin wall (thickness 0), otherwise along
// the ray refracted into the surface for thickness mesh units, scaled per axis by the node.
vec3 TransmissionExitPoint(vec3 position, vec3 N, vec3 V, float ior, float thickness, vec3 modelScale)
{
    if (thickness <= 0.0f)
    {
        return position;
    }
    vec3 refracted = refract(-V, N, 1.0f / max(ior, 1.0f));
    if (dot(refracted, refracted) < 1e-8f)
    {
        return position;
    }
    return position + normalize(refracted) * thickness * modelScale;
}

// Roughness blurs transmitted light less the closer the IOR is to 1 (applyIorToRoughness).
float TransmissionRoughness(float roughness, float ior)
{
    return roughness * clamp(ior * 2.0f - 2.0f, 0.0f, 1.0f);
}

// The copy's mip level for a surface's perceptual roughness: log2(size) at full blur.
float TransmissionLod(float perceptualRoughness, float ior)
{
    return log2(kTransmissionCopySize) * TransmissionRoughness(perceptualRoughness, ior);
}

// KHR_materials_dispersion: the IOR for red, green and blue, spread (ior - 1) * 0.025 * dispersion
// either side of the material's, as the Khronos sample viewer spreads it. Red bends least.
vec3 DispersedIors(float ior, float dispersion)
{
    float halfSpread = (ior - 1.0f) * 0.025f * dispersion;
    return vec3(ior - halfSpread, ior, ior + halfSpread);
}

// Beer-Lambert through distance: white light turns attenuationColor after attenuationDistance. An
// attenuation distance of 0 stands for infinity, no absorption.
vec3 ApplyVolumeAttenuation(vec3 radiance, float distance, vec3 attenuationColor, float attenuationDistance)
{
    if (attenuationDistance <= 0.0f)
    {
        return radiance;
    }
    return pow(attenuationColor, vec3(distance / attenuationDistance)) * radiance;
}

#endif
