#include "shader_helpers.slang"
#include "scene_common.slang"
#include "atmosphere_sampling.slang"
#include "volumetric_clouds.slang"
#include "cloud_reconstruction.slang"

// The clouds at the scene's extent, every frame after cloud_march.comp
// (VulkanAtmosphere::RecordClouds; cloud_reconstruction.slang). The pixel this frame marched takes
// its sample, blended with its history; the others take last frame's clouds reprojected, held to
// the range of this frame's samples around them, so a moving view leaves no trail. Without a
// history (the first frame, a resize) every pixel interpolates this frame's samples. The sky pass
// composites the result; the RecordClouds copy makes it next frame's history.
// Set 1 bindings 11-13 of the atmosphere's compute set.
[[vk::binding(11, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> cloudSamples;
[[vk::binding(12, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> cloudResolvedOut;
[[vk::binding(13, 1)]] Texture2D cloudHistory;
[[vk::binding(77, 1)]] SamplerState cloudHistorySampler;

// A freshly marched pixel keeps this much of its own sample over its history, which averages the
// march's jitter over the frames before TAA does.
static const float CLOUD_FRESH_WEIGHT = 0.5;
// The others lean toward this frame's interpolated samples by this much per pixel the clouds moved,
// up to the most: the reprojection takes one distance per ray, so a fast-moving camera's parallax
// lands a little off, and pixels refreshed on different frames would comb the edges.
static const float CLOUD_MOTION_WEIGHT_PER_PIXEL = 0.125;
static const float CLOUD_MAX_MOTION_WEIGHT = 0.5;

float4 MarchedSample(int2 texel)
{
    return cloudSamples[clamp(texel, int2(0), ImageSize(cloudSamples) - 1)];
}

// Catmull-Rom over the history in five bilinear taps, as taa_resolve.comp: a moving view resamples
// the history every frame, and bilinear would blur it a little more each time.
float4 SampleHistory(float2 uv, float2 extent)
{
    float2 samplePosition = uv * extent;
    float2 center = floor(samplePosition - 0.5) + 0.5;
    float2 f = samplePosition - center;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 position0 = (center - 1.0) / extent;
    float2 position3 = (center + 2.0) / extent;
    float2 position12 = (center + w2 / w12) / extent;

    float4 result = cloudHistory.SampleLevel(cloudHistorySampler, float2(position12.x, position0.y), 0.0) * (w12.x * w0.y);
    result += cloudHistory.SampleLevel(cloudHistorySampler, float2(position0.x, position12.y), 0.0) * (w0.x * w12.y);
    result += cloudHistory.SampleLevel(cloudHistorySampler, position12, 0.0) * (w12.x * w12.y);
    result += cloudHistory.SampleLevel(cloudHistorySampler, float2(position3.x, position12.y), 0.0) * (w3.x * w12.y);
    result += cloudHistory.SampleLevel(cloudHistorySampler, float2(position12.x, position3.y), 0.0) * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    // The negative lobes can ring below zero at hard edges; the box below bounds the rest.
    return max(result / weight, float4(0.0));
}

// How far the clouds along direction lie from the camera, km, to reproject them: the middle of the
// first stretch of the layer the ray crosses, as the march lights them; far when it misses.
float CloudDistanceKm(float3 direction)
{
    float3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float inner = BottomRadius() + ubo.cloudLayer.x;
    float thickness = ubo.cloudLayer.y;
    float2 span = CloudShellInterval(camera, direction, BottomRadius(), inner, inner + thickness, CLOUD_MAX_DISTANCE_KM);
    return span.y > span.x ? span.x + min(span.y - span.x, thickness) * 0.5 : CLOUD_MAX_DISTANCE_KM;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    int2 extent = CloudSceneExtent();
    if (any(pixel >= extent))
    {
        return;
    }

    // This frame's samples sit at 2 t + marched: the pixel in sample units, and the 3 x 3 samples
    // around the nearest, which hold the bilinear four.
    int2 marched = CloudMarchedPixelInBlock();
    float2 position = float2(pixel - marched) * 0.5;
    int2 base = int2(floor(position));
    float2 f = position - float2(base);
    int2 nearest = base + int2((f >= float2(0.5)));
    float4 samples[9];
    float4 low = float4(65504.0);
    float4 high = float4(0.0);
    for (int y = 0; y < 3; ++y)
    {
        for (int x = 0; x < 3; ++x)
        {
            float4 marchedSample = MarchedSample(nearest + int2(x - 1, y - 1));
            samples[y * 3 + x] = marchedSample;
            low = min(low, marchedSample);
            high = max(high, marchedSample);
        }
    }
    int2 corner = base - nearest + int2(1);
    float4 s00 = samples[corner.y * 3 + corner.x];
    float4 s10 = samples[corner.y * 3 + min(corner.x + 1, 2)];
    float4 s01 = samples[min(corner.y + 1, 2) * 3 + corner.x];
    float4 s11 = samples[min(corner.y + 1, 2) * 3 + min(corner.x + 1, 2)];
    float4 current = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
    bool fresh = all((pixel & 1) == marched);

    float4 result = current;
    if (CloudHistoryValid())
    {
        float2 uv = (float2(pixel) + 0.5) / float2(extent);
        float3 direction = ViewDirectionFromTexCoord(uv + CloudJitterUv());
        float3 world = ubo.cameraWorldPosition.xyz + direction * (CloudDistanceKm(direction) * 1000.0);
        // The wind carried the clouds here: last frame they stood upwind by this frame's step.
        float3 previousWorld = world - float3(ubo.cloudMotionStep.x, 0.0, ubo.cloudMotionStep.y);
        float4 previousClip = mul(ubo.prevViewProj, float4(previousWorld, 1.0));
        float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
        if (previousClip.w > 0.0 && all(previousUv >= float2(0.0)) && all(previousUv <= float2(1.0)))
        {
            float4 history = clamp(SampleHistory(previousUv, float2(extent)), low, high);
            float motionPixels = length((uv - previousUv) * float2(extent));
            float weight = fresh ? CLOUD_FRESH_WEIGHT : min(motionPixels * CLOUD_MOTION_WEIGHT_PER_PIXEL, CLOUD_MAX_MOTION_WEIGHT);
            result = lerp(history, current, weight);
        }
    }
    cloudResolvedOut[pixel] = result;
}
