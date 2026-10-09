// Screen Space Indirect Lighting with Visibility Bitmask (Therrien, Levesque, Gilet
// 2023), ambient occlusion half only. Per pixel: a few slices through the view vector, each a
// 32-sector bitmask over the hemisphere around the normal. Every depth sample is treated as a slab
// of constant thickness, so it occludes only the sectors between its front and back, and thin
// geometry no longer shadows everything behind it the way a GTAO horizon does.

#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "vbao_common.slang"

[[vk::binding(0, 1)]] Texture2D depthTexture : register(t0, space1);
[[vk::binding(1, 1)]] Texture2D normalTexture : register(t1, space1);
// r the AO; g the DDGI probe occlusion, 1 here (rt_occlusion.comp writes it after this pass).
[[vk::binding(2, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> aoRawImage : register(u2, space1);

static const float PI = 3.14159265359;
static const float HALF_PI = 1.57079632679;
static const uint SECTOR_COUNT = 32u;

// Jimenez 2014. Offsetting the pixel by a per-frame amount decorrelates frames for the temporal
// filter while keeping the spatial pattern low-discrepancy for the spatial one.
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Bits [floor(minT * 32), floor(minT * 32) + ceil((maxT - minT) * 32)), the sectors a slab spanning
// minT..maxT covers. The paper's formulation, including the conservative ceil.
uint OccludedSectors(float minT, float maxT)
{
    uint startBit = min(uint(minT * float(SECTOR_COUNT)), SECTOR_COUNT - 1u);
    uint count = uint(ceil((maxT - minT) * float(SECTOR_COUNT)));
    uint mask = count == 0u ? 0u : (0xFFFFFFFFu >> (SECTOR_COUNT - min(count, SECTOR_COUNT)));
    return mask << startBit;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // Half resolution: this invocation traces one pixel of its 2x2 block (HalfResSourcePixel).
    int2 halfPixel = int2(dispatchThreadId.xy);
    if (any(halfPixel >= HalfResExtent()))
    {
        return;
    }
    int2 pixel = HalfResSourcePixel(halfPixel);

    float depth = depthTexture.Load(int3(pixel, 0)).r;
    if (IsFarDepth(depth))
    {
        aoRawImage[halfPixel] = float4(1.0);
        return;
    }

    float2 uv = (float2(pixel) + 0.5) * aoConstants.invExtent;
    float3 position = ViewPositionFromDepth(uv, depth);
    // The geometric normal: the depth buffer does not follow the normal map, so the shading normal
    // would self-occlude.
    float3 worldNormal = DecodeNormalOctahedral(normalTexture.Load(int3(pixel, 0)).ba);
    float3 normal = normalize(mul((float3x3)ubo.view, worldNormal));
    float3 V = normalize(-position);

    // The world radius projected at this depth, in pixels. |proj[1][1]| is 1 / tan(fovY / 2).
    float pixelRadius = aoConstants.radius * abs(ubo.proj[1][1]) * 0.5 * aoConstants.extent.y / -position.z;
    pixelRadius = min(pixelRadius, aoConstants.maxPixelRadius);
    if (pixelRadius < 1.0)
    {
        aoRawImage[halfPixel] = float4(1.0);
        return;
    }

    float2 noisePixel = float2(pixel) + 5.588238 * float(aoConstants.frameIndex % 64u);
    float sliceNoise = InterleavedGradientNoise(noisePixel);
    float stepNoise = InterleavedGradientNoise(noisePixel + float2(37.0, 17.0));

    float visibility = 0.0;
    float weightSum = 0.0;
    for (uint slice = 0u; slice < aoConstants.sliceCount; ++slice)
    {
        float phi = (PI / float(aoConstants.sliceCount)) * (float(slice) + sliceNoise);
        // Screen direction in pixels, +y down. For square pixels the view-space direction it
        // traces is (x, -y, 0).
        float2 omega = float2(cos(phi), sin(phi));
        float3 direction = float3(omega.x, -omega.y, 0.0);
        float3 orthoDirection = direction - dot(direction, V) * V;
        float3 axis = normalize(cross(direction, V));
        float3 projectedNormal = normal - axis * dot(normal, axis);
        float projectedLength = length(projectedNormal);
        // The normal's angle from V within the slice, positive toward +direction.
        float signN = dot(orthoDirection, projectedNormal) >= 0.0 ? 1.0 : -1.0;
        float cosN = clamp(dot(projectedNormal, V) / max(projectedLength, 1e-4), 0.0, 1.0);
        float n = signN * acos(cosN);

        uint occlusion = 0u;
        for (int side = 0; side < 2; ++side)
        {
            float sideSign = side == 0 ? 1.0 : -1.0;
            for (uint stepIndex = 0u; stepIndex < aoConstants.stepCount; ++stepIndex)
            {
                // Quadratic spacing puts more samples near the pixel, where occluders subtend
                // the most angle.
                float t = (float(stepIndex) + stepNoise) / float(aoConstants.stepCount);
                float distancePixels = 1.0 + t * t * (pixelRadius - 1.0);
                float2 samplePixel = floor(float2(pixel) + 0.5 + omega * sideSign * distancePixels);
                if (any(samplePixel < float2(0.0)) || any(samplePixel >= aoConstants.extent))
                {
                    break;
                }

                float sampleDepth = depthTexture.Load(int3(int2(samplePixel), 0)).r;
                if (IsFarDepth(sampleDepth))
                {
                    continue;
                }

                float3 samplePosition = ViewPositionFromDepth((samplePixel + 0.5) * aoConstants.invExtent, sampleDepth);
                float3 deltaFront = samplePosition - position;
                float frontLength = length(deltaFront);
                if (frontLength > aoConstants.radius || frontLength < 1e-4)
                {
                    continue;
                }
                // The slab's back face: pushed away from the camera along the view ray.
                float3 deltaBack = deltaFront - V * aoConstants.thickness;

                float2 horizonCos = float2(dot(deltaFront / frontLength, V), dot(normalize(deltaBack), V));
                // Angles from V, signed toward the side being marched.
                float2 theta = sideSign * acos(clamp(horizonCos, -1.0, 1.0));
                // Position within the hemisphere [n - pi/2, n + pi/2] as 0..1, cosine weighted:
                // equal sector widths carry equal shares of the ambient integral.
                float2 sectorT = 0.5 + 0.5 * sin(clamp(theta - n, -HALF_PI, HALF_PI));
                occlusion |= OccludedSectors(min(sectorT.x, sectorT.y), max(sectorT.x, sectorT.y));
            }
        }

        float sliceVisibility = 1.0 - float(countbits(occlusion)) / float(SECTOR_COUNT);
        // Cosine weighting across slices too: a slice the normal lies far out of contributes less.
        float weight = projectedLength;
        visibility += sliceVisibility * weight;
        weightSum += weight;
    }

    aoRawImage[halfPixel] = float4(weightSum > 0.0 ? visibility / weightSum : 1.0, 1.0, 0.0, 0.0);
}
