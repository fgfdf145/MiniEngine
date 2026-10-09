#ifndef SPECULAR_AA_SLANG
#define SPECULAR_AA_SLANG

// Geometric specular anti-aliasing (Tokuyoshi & Kaplanyan 2019, as Filament ships it): widen a lobe
// by how much its normal varies across the pixel, so a highlight narrower than the pixel spreads
// over the pixels that should share it instead of being hit or missed. FilterRoughnessForSpecularAA
// in engine/renderer/specular_aa.cpp is the same arithmetic and carries the tests. Include
// scene_common.slang first: the switch and constants are ubo.specularAntiAliasing.

// |dN/dx|^2 + |dN/dy|^2 across one pixel. Call it in uniform control flow, where derivatives are
// defined. Fine derivatives: HLSL's ddx is coarse (one per 2x2 quad) on Direct3D 12, where Vulkan's
// on NVIDIA is fine, and the coarse one rings on gently curved surfaces.
float NormalVariation(float3 N)
{
    float3 dx = ddx_fine(N);
    float3 dy = ddy_fine(N);
    return dot(dx, dx) + dot(dy, dy);
}

float FilterRoughnessForSpecularAA(float perceptualRoughness, float normalVariation)
{
    if (ubo.specularAntiAliasing.x < 0.5)
        return perceptualRoughness;
    float kernel = min(2.0 * ubo.specularAntiAliasing.y * normalVariation, ubo.specularAntiAliasing.z);
    // Untouched rather than round-tripped through alpha^2: a flat surface shades exactly as before.
    if (kernel <= 0.0)
        return perceptualRoughness;
    float alpha = perceptualRoughness * perceptualRoughness;
    return sqrt(sqrt(min(alpha * alpha + kernel, 1.0)));
}

#endif
