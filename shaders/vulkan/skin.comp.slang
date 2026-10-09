// Linear blend skinning (VulkanSkinningPass): one skinned submesh's bind pose vertices, each moved by
// the weighted sum of its joints' palette matrices, written over the vertex buffer every pass draws
// and the position stream the depth-only passes read.
// Vertex in engine/asset/mesh.h, 20 floats: position, colour, UV 0, normal, tangent, UV 1, outline
// normal.
static const uint kVertexFloats = 20u;
static const uint kPosition = 0u;
static const uint kNormal = 8u;
static const uint kTangent = 11u;
static const uint kOutlineNormal = 17u;

[[vk::binding(0, 0)]] StructuredBuffer<float> bindPose : register(t0, space0);
// VertexSkin, six words a vertex: four 16-bit joints in two words, then four float weights.
[[vk::binding(1, 0)]] StructuredBuffer<uint> skin : register(t1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<float> vertices : register(u2, space0);
// Read as well: last frame's pose rolls into previousPositions before this one is written.
[[vk::binding(3, 0)]] RWStructuredBuffer<float> positions : register(u3, space0);
[[vk::binding(4, 0)]] RWStructuredBuffer<float> previousPositions : register(u4, space0);
// This frame's joint matrices, every skinned submesh's run in turn.
[[vk::binding(0, 1)]] StructuredBuffer<float4x4> palette : register(t0, space1);

struct Constants
{
    uint vertexCount;
    uint paletteBase;
};
#include "d3d_register.slang"

[[vk::push_constant]] ConstantBuffer<Constants> constants D3D_PUSH_CONSTANTS(0);

float3 ReadVec3(uint base)
{
    return float3(bindPose[base], bindPose[base + 1u], bindPose[base + 2u]);
}

void WriteVec3(uint base, float3 value)
{
    vertices[base] = value.x;
    vertices[base + 1u] = value.y;
    vertices[base + 2u] = value.z;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint vertex = dispatchThreadId.x;
    if (vertex >= constants.vertexCount)
    {
        return;
    }

    uint s = vertex * 6u;
    uint4 joints = uint4(skin[s] & 0xFFFFu, skin[s] >> 16u, skin[s + 1u] & 0xFFFFu, skin[s + 1u] >> 16u);
    float4 weights = float4(asfloat(skin[s + 2u]), asfloat(skin[s + 3u]), asfloat(skin[s + 4u]), asfloat(skin[s + 5u]));
    uint base = constants.paletteBase;
    float4x4 m = weights.x * palette[base + joints.x] + weights.y * palette[base + joints.y] +
             weights.z * palette[base + joints.z] + weights.w * palette[base + joints.w];

    uint v = vertex * kVertexFloats;
    // What skinning leaves alone (colour, UVs, the tangent's sign) is copied through.
    for (uint i = 0u; i < kVertexFloats; ++i)
    {
        vertices[v + i] = bindPose[v + i];
    }

    float3 position = mul(m, float4(ReadVec3(v + kPosition), 1.0)).xyz;
    WriteVec3(v + kPosition, position);
    // Where the vertex was, for the motion vectors (triangle.vert); the first pose sees the bind pose.
    previousPositions[vertex * 3u] = positions[vertex * 3u];
    previousPositions[vertex * 3u + 1u] = positions[vertex * 3u + 1u];
    previousPositions[vertex * 3u + 2u] = positions[vertex * 3u + 2u];
    positions[vertex * 3u] = position.x;
    positions[vertex * 3u + 1u] = position.y;
    positions[vertex * 3u + 2u] = position.z;

    // Directions by the matrix's linear part: rotations and the near-uniform scales a rig has.
    float3x3 linear = (float3x3)m;
    float3 normal = mul(linear, ReadVec3(v + kNormal));
    WriteVec3(v + kNormal, dot(normal, normal) > 0.0 ? normalize(normal) : ReadVec3(v + kNormal));
    float3 tangent = mul(linear, ReadVec3(v + kTangent));
    WriteVec3(v + kTangent, dot(tangent, tangent) > 0.0 ? normalize(tangent) : ReadVec3(v + kTangent));
    float3 outline = mul(linear, ReadVec3(v + kOutlineNormal));
    WriteVec3(v + kOutlineNormal, dot(outline, outline) > 0.0 ? normalize(outline) : outline);
}
