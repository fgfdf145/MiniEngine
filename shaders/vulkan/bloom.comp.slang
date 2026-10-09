// Glare on the bloom mip chain of Jimenez 2014 ("Next Generation Post Processing in Call of Duty:
// Advanced Warfare"), weighted as diffraction spreads energy (see engine/renderer/glare.h). One
// shader, four modes, dispatched by VulkanBloomPass in this order:
//   first downsample  SceneTaa -> level 0, 13 taps, Karis average against fireflies;
//   downsample        level i-1 -> level i, 13 taps;
//   upsample          level i = band_i * level i + 3x3 tent of level i+1, from the bottom up;
//   composite         SceneTaa = SceneTaa * (1 - total) + level 0.
// Level 0 ends up holding sum_k band_k * blur_k, the energy diffraction carries past 2 pixels, and
// the composite takes exactly that much from each pixel where it was.

#include "pre_exposure.slang"

// Set 1 is VulkanBloomPass's NVRHI binding set: the texture at 0, its sampler at 64.
[[vk::binding(0, 1)]] Texture2D sourceTexture;
[[vk::binding(64, 1)]] SamplerState sourceTextureSampler;
[[vk::binding(1, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> destinationImage;

// Must match BloomPushConstants in engine/renderer/vulkan/bloom_pass.cpp.
struct BloomConstants
{
    uint2 destinationExtent;
    float2 sourceTexelSize;
    uint mode;
    float unused0;
    float unused1;
    float unused2;
    // Upsample: level = destination * destinationWeight + tent(source) * sourceWeight. Composite:
    // scene = scene * destinationWeight + level 0 * sourceWeight. rgb used.
    float4 destinationWeight;
    float4 sourceWeight;
};
[[vk::push_constant]] ConstantBuffer<BloomConstants> bloom;

static const uint MODE_FIRST_DOWNSAMPLE = 0u;
static const uint MODE_DOWNSAMPLE = 1u;
static const uint MODE_UPSAMPLE = 2u;
static const uint MODE_COMPOSITE = 3u;

float3 Sanitize(float3 color)
{
    if (any(isnan(color)) || any(isinf(color)))
        return float3(0.0);
    return clamp(color, float3(0.0), float3(65504.0));
}

float3 Tap(float2 uv, float2 offset)
{
    return Sanitize(sourceTexture.SampleLevel(sourceTextureSampler, uv + offset * bloom.sourceTexelSize, 0.0).rgb);
}

float KarisWeight(float3 color)
{
    // Exposed luma (1.0 = sensor saturation): Karis' weights must see what the viewer sees.
    return 1.0 / (1.0 + dot(color, float3(0.2126, 0.7152, 0.0722)) * kExposedPerFrameBufferUnit);
}

// The 13 bilinear taps of Jimenez's downsample, grouped into five overlapping 2x2 boxes: the centre
// box weighs 0.5, the four corner boxes 0.125 each. With karis, each box is also weighted by
// 1 / (1 + its exposed luma), and the weights renormalised.
float3 Downsample(float2 uv, bool karis)
{
    float3 a = Tap(uv, float2(-2.0, -2.0));
    float3 b = Tap(uv, float2(0.0, -2.0));
    float3 c = Tap(uv, float2(2.0, -2.0));
    float3 d = Tap(uv, float2(-1.0, -1.0));
    float3 e = Tap(uv, float2(1.0, -1.0));
    float3 f = Tap(uv, float2(-2.0, 0.0));
    float3 g = Tap(uv, float2(0.0, 0.0));
    float3 h = Tap(uv, float2(2.0, 0.0));
    float3 i = Tap(uv, float2(-1.0, 1.0));
    float3 j = Tap(uv, float2(1.0, 1.0));
    float3 k = Tap(uv, float2(-2.0, 2.0));
    float3 l = Tap(uv, float2(0.0, 2.0));
    float3 m = Tap(uv, float2(2.0, 2.0));

    float3 boxes[5] = {(d + e + i + j) * 0.25, (a + b + f + g) * 0.25, (b + c + g + h) * 0.25, (f + g + k + l) * 0.25, (g + h + l + m) * 0.25};
    float weights[5] = {0.5, 0.125, 0.125, 0.125, 0.125};
    float3 sum = float3(0.0);
    float weightSum = 0.0;
    for (int box = 0; box < 5; ++box)
    {
        float weight = weights[box] * (karis ? KarisWeight(boxes[box]) : 1.0);
        sum += boxes[box] * weight;
        weightSum += weight;
    }
    return sum / weightSum;
}

// 3x3 tent over the coarser level's texels: weights 1 2 1 / 2 4 2 / 1 2 1, sixteenths.
float3 UpsampleTent(float2 uv)
{
    float3 sum = Tap(uv, float2(-1.0, -1.0)) + Tap(uv, float2(1.0, -1.0)) + Tap(uv, float2(-1.0, 1.0)) + Tap(uv, float2(1.0, 1.0));
    sum += 2.0 * (Tap(uv, float2(0.0, -1.0)) + Tap(uv, float2(-1.0, 0.0)) + Tap(uv, float2(1.0, 0.0)) + Tap(uv, float2(0.0, 1.0)));
    sum += 4.0 * Tap(uv, float2(0.0));
    return sum / 16.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    if (any(uint2(pixel) >= bloom.destinationExtent))
    {
        return;
    }
    float2 uv = (float2(pixel) + 0.5) / float2(bloom.destinationExtent);

    float3 result;
    if (bloom.mode == MODE_FIRST_DOWNSAMPLE || bloom.mode == MODE_DOWNSAMPLE)
    {
        result = Downsample(uv, bloom.mode == MODE_FIRST_DOWNSAMPLE);
    }
    else if (bloom.mode == MODE_UPSAMPLE)
    {
        // Each pixel reads and writes only its own texel of this level, so the read-modify-write
        // needs no ordering between invocations.
        result = Sanitize(destinationImage[pixel].rgb) * bloom.destinationWeight.rgb +
                 UpsampleTent(uv) * bloom.sourceWeight.rgb;
    }
    else
    {
        float3 scene = Sanitize(destinationImage[pixel].rgb);
        float3 glow = Sanitize(sourceTexture.SampleLevel(sourceTextureSampler, uv, 0.0).rgb);
        result = scene * bloom.destinationWeight.rgb + glow * bloom.sourceWeight.rgb;
    }
    destinationImage[pixel] = float4(Sanitize(result), 1.0);
}
