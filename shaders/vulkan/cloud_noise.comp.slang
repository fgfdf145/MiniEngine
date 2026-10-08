// The volumetric clouds' two tiling billow volumes, built once (VulkanAtmosphere::Record)
// (docs/design/2026-10-06-cumulus-generation-design.md). Every channel is one octave of sphere
// packing: around one jittered point per cell, sqrt(1 - d^2) of the distance d to the nearest in
// cells, so a channel is a union of round lobes with creases where they meet - the cauliflower of
// a cumulus. Each is stretched from its 0.45 floor onto [0, 1], where its mean is 0.69
// (kCloudBillowMean); the clouds add every octave about that mean, scaled to its sphere size.
//   shape  128^3: r g b 4, 8 and 16 spheres per tile, a 1;
//   detail 128^3: r g b a 2, 4, 8 and 16 spheres per tile.
// Every octave wraps at the tile edge, so the REPEAT sampler shows no seam.
// Set 1 bindings 7 and 8 of the atmosphere's compute set.
[[vk::binding(7, 1)]] [[vk::image_format("rgba8")]] RWTexture3D<float4> shapeNoiseOut;
[[vk::binding(8, 1)]] [[vk::image_format("rgba8")]] RWTexture3D<float4> detailNoiseOut;

// Must match kCloudNoiseSizes in engine/renderer/vulkan/atmosphere.cpp.
static const int SHAPE_SIZE = 128;
static const int DETAIL_SIZE = 128;

uint3 Hash3(uint3 v)
{
    // Jarzynski and Olano 2020, pcg3d.
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

float3 Random3(int3 cell, uint seed)
{
    return float3(Hash3(uint3(cell) + uint3(seed, seed * 7u, seed * 13u)) & 0xffffu) / 65535.0;
}

// One octave of sphere packing over period cells per unit of p in [0, 1), in [0, 1].
float SphereBillow(float3 p, float period, uint seed)
{
    float3 scaled = p * period;
    int3 cell = int3(floor(scaled));
    float3 local = frac(scaled);
    int wrap = int(period);
    float nearest = 1.0;
    for (int z = -1; z <= 1; ++z)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                int3 offset = int3(x, y, z);
                int3 wrapped = (cell + offset + wrap) % wrap;
                float3 feature = float3(offset) + Random3(wrapped, seed);
                nearest = min(nearest, length(feature - local));
            }
        }
    }
    float sphere = sqrt(max(1.0 - nearest * nearest, 0.0));
    return clamp((sphere - 0.45) / 0.55, 0.0, 1.0);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int3 texel = int3(dispatchThreadId);
    if (any(texel >= int3(SHAPE_SIZE)))
    {
        return;
    }
    float3 p = (float3(texel) + 0.5) / float(SHAPE_SIZE);
    shapeNoiseOut[texel] = float4(SphereBillow(p, 4.0, 11u), SphereBillow(p, 8.0, 23u), SphereBillow(p, 16.0, 37u), 1.0);
    if (all(texel < int3(DETAIL_SIZE)))
    {
        float3 q = (float3(texel) + 0.5) / float(DETAIL_SIZE);
        detailNoiseOut[texel] = float4(SphereBillow(q, 2.0, 51u), SphereBillow(q, 4.0, 67u), SphereBillow(q, 8.0, 79u), SphereBillow(q, 16.0, 97u));
    }
}
