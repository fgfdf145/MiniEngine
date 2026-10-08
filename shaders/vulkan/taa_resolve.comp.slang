// Temporal anti-aliasing resolve (Karis 2014, with the refinements that became standard). Each frame
// is rendered with a sub-pixel jitter; this pass blends it into last frame's result, reprojected
// through the motion vectors, so a still image converges to many samples per pixel.
//
// - velocity from the nearest-depth pixel of the 3x3 neighbourhood, so an edge moves with the
//   surface in front of it; background pixels, which have no velocity, reproject by camera motion;
// - history sampled with a 5-tap Catmull-Rom filter, which keeps it from blurring frame over frame;
// - history clipped toward the neighbourhood mean in YCoCg, to mean +- CLIP_GAMMA * sigma of the
//   current 3x3 (variance clipping), which removes ghosts left by disocclusion and changing light;
// - a 10% / 90% blend weighted by 1 / (1 + exposed luma), so one bright sample cannot dominate.

#include "scene_common.slang"
#include "pre_exposure.slang"

// Set 1 is VulkanTaaPass's NVRHI binding set: the inputs read with Load have no samplers.
[[vk::binding(0, 1)]] Texture2D currentTexture;  // SceneHdr
[[vk::binding(1, 1)]] Texture2D depthTexture;
[[vk::binding(2, 1)]] Texture2D velocityTexture;
[[vk::binding(3, 1)]] Texture2D historyTexture;  // last frame's result
[[vk::binding(67, 1)]] SamplerState historyTextureSampler; // linear
[[vk::binding(4, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> historyImage;
[[vk::binding(5, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outputImage; // SceneTaa

// Must match TaaPushConstants in engine/renderer/vulkan/taa_pass.cpp.
struct TaaConstants
{
    float2 extent;
    float2 invExtent;
    // Current over previous pre-exposure: the history holds last frame's pre-exposed values and is
    // multiplied by this before it is used, so a changing exposure does not flash or trail. 1 when
    // the history is not valid.
    float historyScale;
    uint flags;
    float2 unused;
};
[[vk::push_constant]] ConstantBuffer<TaaConstants> taa;

static const uint TAA_FLAG_ENABLED = 1u;
static const uint TAA_FLAG_HISTORY_VALID = 2u;

static const float CURRENT_WEIGHT = 0.1;
static const float CLIP_GAMMA = 1.0;

// NaN, infinities and negatives would never leave the history once they entered it.
float3 Sanitize(float3 color)
{
    if (any(isnan(color)) || any(isinf(color)))
        return float3(0.0);
    return clamp(color, float3(0.0), float3(65504.0));
}

float3 LoadCurrent(int2 pixel)
{
    return Sanitize(currentTexture.Load(int3(clamp(pixel, int2(0), int2(taa.extent) - 1), 0)).rgb);
}

float3 RgbToYCoCg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 YCoCgToRgb(float3 c)
{
    float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Catmull-Rom through nine texels using five bilinear taps (the corner taps' weights are small
// enough to drop).
float3 SampleHistoryCatmullRom(float2 uv)
{
    float2 samplePosition = uv * taa.extent;
    float2 center = floor(samplePosition - 0.5) + 0.5;
    float2 f = samplePosition - center;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 position0 = (center - 1.0) * taa.invExtent;
    float2 position3 = (center + 2.0) * taa.invExtent;
    float2 position12 = (center + w2 / w12) * taa.invExtent;

    float3 result = historyTexture.SampleLevel(historyTextureSampler, float2(position12.x, position0.y), 0.0).rgb * (w12.x * w0.y);
    result += historyTexture.SampleLevel(historyTextureSampler, float2(position0.x, position12.y), 0.0).rgb * (w0.x * w12.y);
    result += historyTexture.SampleLevel(historyTextureSampler, position12, 0.0).rgb * (w12.x * w12.y);
    result += historyTexture.SampleLevel(historyTextureSampler, float2(position3.x, position12.y), 0.0).rgb * (w3.x * w12.y);
    result += historyTexture.SampleLevel(historyTextureSampler, float2(position12.x, position3.y), 0.0).rgb * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    // The negative lobes can ring below zero at hard edges; the clip below bounds the rest.
    return max(result / weight, float3(0.0));
}

// Moves the history along the line to the box centre until it is inside the box (Playdead's
// AABB clip), rather than clamping each axis, which would shift its hue.
float3 ClipToBox(float3 history, float3 boxMin, float3 boxMax)
{
    float3 center = 0.5 * (boxMin + boxMax);
    float3 halfSize = 0.5 * (boxMax - boxMin) + 1e-5;
    float3 offset = history - center;
    float3 scale = halfSize / max(abs(offset), float3(1e-5));
    float t = clamp(min(scale.x, min(scale.y, scale.z)), 0.0, 1.0);
    return center + offset * t;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    if (any(pixel >= int2(taa.extent)))
    {
        return;
    }

    float3 current = LoadCurrent(pixel);

    // TAA off, or the forward-only order: pass the image through untouched. History is left alone;
    // the renderer reports it invalid for the next frame that resolves.
    if ((taa.flags & TAA_FLAG_ENABLED) == 0u)
    {
        outputImage[pixel] = float4(current, 1.0);
        return;
    }

    float3 moment1 = float3(0.0);
    float3 moment2 = float3(0.0);
    float closestDepth = DEPTH_FAR;
    int2 closestPixel = pixel;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 neighbor = clamp(pixel + int2(x, y), int2(0), int2(taa.extent) - 1);
            float3 color = RgbToYCoCg(LoadCurrent(neighbor));
            moment1 += color;
            moment2 += color * color;
            // Reverse-Z, the near plane at 1: the greatest depth is the nearest surface.
            float depth = depthTexture.Load(int3(neighbor, 0)).r;
            if (depth > closestDepth)
            {
                closestDepth = depth;
                closestPixel = neighbor;
            }
        }
    }
    float3 mean = moment1 / 9.0;
    float3 sigma = sqrt(max(moment2 / 9.0 - mean * mean, float3(0.0)));
    float3 boxMin = mean - CLIP_GAMMA * sigma;
    float3 boxMax = mean + CLIP_GAMMA * sigma;

    float2 uv = (float2(pixel) + 0.5) * taa.invExtent;
    float2 velocity;
    if (IsFarDepth(closestDepth))
    {
        // Nothing but background around this pixel, and background writes no motion vector: it
        // moves with the camera alone, so reproject its far-plane point.
        float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, DEPTH_FAR, 1.0));
        float4 previousClip = mul(ubo.prevViewProj, float4(world.xyz / world.w, 1.0));
        velocity = uv - (previousClip.xy / previousClip.w * 0.5 + 0.5);
    }
    else
    {
        velocity = velocityTexture.Load(int3(closestPixel, 0)).rg;
    }
    float2 previousUv = uv - velocity;

    float3 result = current;
    bool onScreen = all(previousUv >= float2(0.0)) && all(previousUv <= float2(1.0));
    if ((taa.flags & TAA_FLAG_HISTORY_VALID) != 0u && onScreen)
    {
        float3 history = SampleHistoryCatmullRom(previousUv) * taa.historyScale;
        history = YCoCgToRgb(ClipToBox(RgbToYCoCg(history), boxMin, boxMax));
        // Exposed luma (1.0 = sensor saturation), as the weights were tuned.
        float currentWeight = CURRENT_WEIGHT / (1.0 + Luma(current) * kExposedPerFrameBufferUnit);
        float historyWeight = (1.0 - CURRENT_WEIGHT) / (1.0 + Luma(history) * kExposedPerFrameBufferUnit);
        result = (current * currentWeight + history * historyWeight) / (currentWeight + historyWeight);
    }
    result = Sanitize(result);

    historyImage[pixel] = float4(result, 1.0);
    outputImage[pixel] = float4(result, 1.0);
}
