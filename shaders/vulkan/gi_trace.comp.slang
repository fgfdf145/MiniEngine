// One bounce of diffuse light, screen space: the indirect lighting half of Screen Space Indirect
// Lighting with Visibility Bitmask (Therrien, Levesque, Gilet 2023), whose AO half is
// vbao_trace.comp. It marches the same slices and the same 32-sector bitmask over the hemisphere
// around the normal; every sector a depth sample newly covers receives that sample's radiance, and
// sectors already covered by nearer samples stay theirs, so light arrives only from what is visible.
// The sectors are spaced by the sine of the angle, so each carries an equal share of the cosine-
// weighted hemisphere: the sum over them is the irradiance over pi, the same unit the ambient term's
// luminance has.
//
// The radiance is this frame's lit HDR target as the lighting pass left it: direct light, the sky's
// and ambient lights' contribution and emission, but no indirect diffuse (the composite adds that
// after this pass), so the result is exactly one bounce and nothing feeds back. It is the radiance
// the sample sends toward the camera, which is what a diffuse surface sends every way.

#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "vbao_common.slang"

[[vk::binding(0, 1)]] Sampler2D depthTexture;
[[vk::binding(1, 1)]] Sampler2D normalTexture;
[[vk::binding(2, 1)]] Sampler2D hdrTexture;
[[vk::binding(3, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> giRawImage;

static const float PI = 3.14159265359;
static const float HALF_PI = 1.57079632679;
static const uint SECTOR_COUNT = 32u;
// HDR target units (1.0 = 100 cd/m^2 on screen). A sun glint or a lamp's bulb seen by one sample
// would otherwise flash across the whole search disk; this keeps the bounce from the lit scene and
// drops the fireflies, which the temporal filter could not average away in a few frames.
static const float MAX_SAMPLE_RADIANCE = 64.0;
// Two consecutive samples of one side closer than this share of the radius are taken to lie on one
// continuous surface, and the angles between them are covered too. The march takes a dozen samples
// over metres, so a floor seen as separate slabs of the AO's thickness would cover a fraction of the
// sectors it fills in reality and bounce a fraction of its light (a ninth, in the sunlit Sponza).
static const float CONTINUITY_SHARE = 0.25;

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Bits [floor(minT * 32), floor(minT * 32) + ceil((maxT - minT) * 32)), as vbao_trace.comp.
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
        giRawImage[halfPixel] = float4(0.0);
        return;
    }

    float2 uv = (float2(pixel) + 0.5) * aoConstants.invExtent;
    float3 position = ViewPositionFromDepth(uv, depth);
    // The geometric normal, as the AO trace uses: the depth buffer does not follow the normal map.
    float3 normal = normalize(mul((float3x3)ubo.view, DecodeNormalOctahedral(normalTexture.Load(int3(pixel, 0)).ba)));
    float3 V = normalize(-position);

    float pixelRadius = aoConstants.radius * abs(ubo.proj[1][1]) * 0.5 * aoConstants.extent.y / -position.z;
    pixelRadius = min(pixelRadius, aoConstants.maxPixelRadius);
    if (pixelRadius < 1.0)
    {
        giRawImage[halfPixel] = float4(0.0);
        return;
    }

    // Offset from the AO trace's noise so the two passes' patterns do not line up.
    float2 noisePixel = float2(pixel) + 5.588238 * float(aoConstants.frameIndex % 64u) + float2(11.0, 23.0);
    float sliceNoise = InterleavedGradientNoise(noisePixel);
    float stepNoise = InterleavedGradientNoise(noisePixel + float2(37.0, 17.0));

    float3 indirect = float3(0.0);
    float weightSum = 0.0;
    for (uint slice = 0u; slice < aoConstants.sliceCount; ++slice)
    {
        float phi = (PI / float(aoConstants.sliceCount)) * (float(slice) + sliceNoise);
        float2 omega = float2(cos(phi), sin(phi));
        float3 direction = float3(omega.x, -omega.y, 0.0);
        float3 orthoDirection = direction - dot(direction, V) * V;
        float3 axis = normalize(cross(direction, V));
        float3 projectedNormal = normal - axis * dot(normal, axis);
        float projectedLength = length(projectedNormal);
        float signN = dot(orthoDirection, projectedNormal) >= 0.0 ? 1.0 : -1.0;
        float cosN = clamp(dot(projectedNormal, V) / max(projectedLength, 1e-4), 0.0, 1.0);
        float n = signN * acos(cosN);

        uint occlusion = 0u;
        float3 sliceLight = float3(0.0);
        for (int side = 0; side < 2; ++side)
        {
            float sideSign = side == 0 ? 1.0 : -1.0;
            // The previous sample's front angle as a sector position and its position, while one
            // is on this side's continuous surface; a negative angle when there is none.
            float previousT = -1.0;
            float3 previousPosition = float3(0.0);
            for (uint stepIndex = 0u; stepIndex < aoConstants.stepCount; ++stepIndex)
            {
                float t = (float(stepIndex) + stepNoise) / float(aoConstants.stepCount);
                float distancePixels = 1.0 + t * t * (pixelRadius - 1.0);
                float2 samplePixel = floor(float2(pixel) + 0.5 + omega * sideSign * distancePixels);
                if (any(samplePixel < float2(0.0)) || any(samplePixel >= aoConstants.extent))
                {
                    break;
                }

                int2 sampleCoord = int2(samplePixel);
                float sampleDepth = depthTexture.Load(int3(sampleCoord, 0)).r;
                if (IsFarDepth(sampleDepth))
                {
                    previousT = -1.0;
                    continue;
                }

                float3 samplePosition = ViewPositionFromDepth((samplePixel + 0.5) * aoConstants.invExtent, sampleDepth);
                float3 deltaFront = samplePosition - position;
                float frontLength = length(deltaFront);
                if (frontLength > aoConstants.radius || frontLength < 1e-4)
                {
                    previousT = -1.0;
                    continue;
                }
                float3 deltaBack = deltaFront - V * aoConstants.thickness;

                float2 horizonCos = float2(dot(deltaFront / frontLength, V), dot(normalize(deltaBack), V));
                float2 theta = sideSign * acos(clamp(horizonCos, -1.0, 1.0));
                float2 sectorT = 0.5 + 0.5 * sin(clamp(theta - n, -HALF_PI, HALF_PI));
                float minT = min(sectorT.x, sectorT.y);
                float maxT = max(sectorT.x, sectorT.y);
                if (previousT >= 0.0 && distance(samplePosition, previousPosition) < CONTINUITY_SHARE * aoConstants.radius)
                {
                    minT = min(minT, previousT);
                    maxT = max(maxT, previousT);
                }
                previousT = sectorT.x;
                previousPosition = samplePosition;
                uint sampleSectors = OccludedSectors(minT, maxT);
                uint newSectors = sampleSectors & ~occlusion;
                occlusion |= sampleSectors;
                if (newSectors == 0u)
                {
                    continue;
                }
                // Light leaves the sample only from its front: a surface seen from behind (the far
                // side of a wall's slab) sends nothing toward this pixel.
                float3 sampleNormal = normalize(mul((float3x3)ubo.view, DecodeNormalOctahedral(normalTexture.Load(int3(sampleCoord, 0)).ba)));
                float emitterFacing = dot(sampleNormal, -deltaFront / frontLength);
                if (emitterFacing <= 0.0)
                {
                    continue;
                }
                float3 radiance = min(hdrTexture.Load(int3(sampleCoord, 0)).rgb, float3(MAX_SAMPLE_RADIANCE));
                sliceLight += radiance * (float(countbits(newSectors)) / float(SECTOR_COUNT));
            }
        }

        // Cosine weighting across slices, as the AO trace weights them.
        float weight = projectedLength;
        indirect += sliceLight * weight;
        weightSum += weight;
    }

    float3 result = weightSum > 0.0 ? indirect / weightSum : float3(0.0);
    giRawImage[halfPixel] = float4(result * aoConstants.strength, 1.0);
}
