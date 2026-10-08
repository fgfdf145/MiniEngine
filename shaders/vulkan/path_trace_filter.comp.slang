// One iteration of the path tracer's spatial filter (VulkanPathTracePass): an a-trous 5 x 5 B-spline
// kernel (Dammertz et al. 2010) whose taps are spaced stepSize pixels apart, three iterations at 1, 2
// and 4 covering 17 x 17. Edge-stopping as SVGF has it (Schied et al. 2017), without its variance
// buffer: a tap counts as far as it lies on the pixel's plane, faces its way and, for luminance, is
// within a few standard deviations of the pixel's 3 x 3 neighbourhood, so the noise is averaged and
// the edges between surfaces and light are not. The specular channel also asks for a similar
// roughness. As a still image's history grows past the motion cap the filter fades out, so the
// converged image is the accumulation alone: the reference. stepSize 0 copies source to target.

#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "path_trace_common.slang"

static const float KERNEL[3] = {0.375, 0.25, 0.0625};
// Luminance taps further than this many standard deviations from the pixel fade out.
static const float LUMINANCE_SIGMA = 4.0;
static const float NORMAL_POWER = 64.0;
// How far off the pixel's plane a tap may lie, as a share of its distance from the camera, per pixel
// of spacing.
static const float PLANE_TOLERANCE = 0.004;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    if (any(pixel >= int2(pathTrace.extent)))
    {
        return;
    }
    float depth = ptDepth.Load(int3(PtGBufferPixel(pixel), 0)).r;
    float4 centerDiffuse;
    float4 centerSpecular;
    LoadPathTracePair(pathTrace.source, pixel, centerDiffuse, centerSpecular);
    if (pathTrace.stepSize == 0u || !PathTraceSurfaceShaded(pixel, depth))
    {
        StorePathTracePair(pathTrace.target, pixel, centerDiffuse, centerSpecular);
        return;
    }

    // How converged each channel is: the frames its history averages.
    float diffuseFrames = 1.0;
    float specularFrames = 1.0;
    if ((pathTrace.flags & PT_FLAG_ACCUMULATE) != 0u)
    {
        diffuseFrames = ptHistoryDiffuse[pixel].a;
        specularFrames = ptHistorySpecular[pixel].a;
    }
    float diffuseFade = smoothstep(pathTrace.motionFrames, 8.0 * pathTrace.motionFrames, diffuseFrames);
    float specularFade = smoothstep(pathTrace.motionFrames, 8.0 * pathTrace.motionFrames, specularFrames);
    if (diffuseFade >= 1.0 && specularFade >= 1.0)
    {
        StorePathTracePair(pathTrace.target, pixel, centerDiffuse, centerSpecular);
        return;
    }

    // The 3 x 3 neighbourhood's luminance spread, which scales the luminance edge-stopping.
    float2 sum = float2(0.0);
    float2 sumSquares = float2(0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 source = clamp(pixel + int2(x, y), int2(0), int2(pathTrace.extent) - 1);
            float4 d;
            float4 s;
            LoadPathTracePair(pathTrace.source, source, d, s);
            float2 luminance = float2(PathTraceLuminance(d.rgb), PathTraceLuminance(s.rgb));
            sum += luminance;
            sumSquares += luminance * luminance;
        }
    }
    float2 mean = sum / 9.0;
    float2 deviation = sqrt(max(sumSquares / 9.0 - mean * mean, float2(0.0)));
    float2 luminanceScale = 1.0 / (LUMINANCE_SIGMA * deviation + 1e-4);

    float3 position = PathTraceWorldPosition(pixel, depth);
    float3 N = DecodeNormalOctahedral(ptNormal.Load(int3(PtGBufferPixel(pixel), 0)).rg);
    float planeScale = 1.0 / max(PLANE_TOLERANCE * length(position - ubo.cameraWorldPosition.xyz) * float(pathTrace.stepSize), 1e-3);
    float roughness = PathTraceSpecularRoughness(pixel);
    float2 centerLuminance = float2(PathTraceLuminance(centerDiffuse.rgb), PathTraceLuminance(centerSpecular.rgb));

    float3 diffuseSum = float3(0.0);
    float3 specularSum = float3(0.0);
    float diffuseWeightSum = 0.0;
    float specularWeightSum = 0.0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            int2 source = pixel + int2(x, y) * int(pathTrace.stepSize);
            if (any(source < int2(0)) || any(source >= int2(pathTrace.extent)))
            {
                continue;
            }
            float sourceDepth = ptDepth.Load(int3(PtGBufferPixel(source), 0)).r;
            if (!PathTraceSurfaceShaded(source, sourceDepth))
            {
                continue;
            }
            float kernel = KERNEL[abs(x)] * KERNEL[abs(y)];
            float3 sourceNormal = DecodeNormalOctahedral(ptNormal.Load(int3(PtGBufferPixel(source), 0)).rg);
            float plane = abs(dot(N, PathTraceWorldPosition(source, sourceDepth) - position));
            float geometry = kernel * exp(-plane * planeScale) * pow(max(dot(N, sourceNormal), 0.0), NORMAL_POWER);
            float4 d;
            float4 s;
            LoadPathTracePair(pathTrace.source, source, d, s);
            float2 luminance = float2(PathTraceLuminance(d.rgb), PathTraceLuminance(s.rgb));
            float2 luminanceWeight = exp(-abs(luminance - centerLuminance) * luminanceScale);
            float diffuseWeight = geometry * luminanceWeight.x;
            float specularWeight = geometry * luminanceWeight.y * exp(-8.0 * abs(PathTraceSpecularRoughness(source) - roughness));
            diffuseSum += d.rgb * diffuseWeight;
            specularSum += s.rgb * specularWeight;
            diffuseWeightSum += diffuseWeight;
            specularWeightSum += specularWeight;
        }
    }
    float3 diffuse = diffuseWeightSum > 1e-6 ? diffuseSum / diffuseWeightSum : centerDiffuse.rgb;
    float3 specular = specularWeightSum > 1e-6 ? specularSum / specularWeightSum : centerSpecular.rgb;
    StorePathTracePair(
        pathTrace.target, pixel,
        float4(lerp(diffuse, centerDiffuse.rgb, diffuseFade), centerDiffuse.a),
        float4(lerp(specular, centerSpecular.rgb, specularFade), centerSpecular.a));
}
