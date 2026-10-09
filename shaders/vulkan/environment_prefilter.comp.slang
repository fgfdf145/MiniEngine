#include "cubemap_common.slang"

// One mip of the prefiltered cube: GGX importance sampling with N = V = R (Karis 2013), each
// sample read from the radiance cube at the mip whose texel solid angle matches its own.
[[vk::binding(1, 1)]] TextureCube radianceCube;
[[vk::binding(65, 1)]] SamplerState radianceCubeSampler;
[[vk::binding(2, 1)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> prefilteredOut;

// Must match PrefilterConstants in engine/renderer/vulkan/environment_probe.cpp.
struct PrefilterConstants
{
    float roughness;
    uint size;
};
[[vk::push_constant]] ConstantBuffer<PrefilterConstants> constants;

static const float PI = 3.14159265358979;
static const uint SAMPLE_COUNT = 64u;

float RadicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float3 ImportanceSampleGgx(float2 xi, float roughness, float3 N)
{
    float alpha = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float3 H = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 tangentX = normalize(cross(up, N));
    float3 tangentY = cross(N, tangentX);
    return tangentX * H.x + tangentY * H.y + N * H.z;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint3 id = dispatchThreadId;
    if (id.x >= constants.size || id.y >= constants.size)
    {
        return;
    }
    float3 N = CubeFaceDirection(id.z, (float2(id.xy) + 0.5) / float(constants.size));
    float3 color;
    if (constants.roughness <= 0.0)
    {
        color = radianceCube.SampleLevel(radianceCubeSampler, N, 0.0).rgb;
    }
    else
    {
        float alpha = constants.roughness * constants.roughness;
        float texelSolidAngle = 4.0 * PI / (6.0 * RADIANCE_CUBE_SIZE * RADIANCE_CUBE_SIZE);
        float3 sum = float3(0.0);
        float weight = 0.0;
        for (uint i = 0u; i < SAMPLE_COUNT; ++i)
        {
            float3 H = ImportanceSampleGgx(float2(float(i) / float(SAMPLE_COUNT), RadicalInverse(i)), constants.roughness, N);
            float3 L = normalize(2.0 * dot(N, H) * H - N);
            float NdL = dot(N, L);
            if (NdL > 0.0)
            {
                float NdH = max(dot(N, H), 0.0);
                float denominator = NdH * NdH * (alpha * alpha - 1.0) + 1.0;
                float D = alpha * alpha / (PI * denominator * denominator);
                // With V = N, VdH = NdH and the pdf D NdH / (4 VdH) is D / 4.
                float pdf = D * 0.25;
                float sampleSolidAngle = 1.0 / (float(SAMPLE_COUNT) * pdf + 1e-4);
                float lod = clamp(0.5 * log2(sampleSolidAngle / texelSolidAngle), 0.0, RADIANCE_CUBE_MIP_COUNT - 1.0);
                sum += radianceCube.SampleLevel(radianceCubeSampler, L, lod).rgb * NdL;
                weight += NdL;
            }
        }
        color = sum / max(weight, 1e-4);
    }
    prefilteredOut[int3(id)] = float4(color, 1.0);
}
