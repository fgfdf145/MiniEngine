// A tyre squashed on the ground and bent by its carcass (VulkanSkinningPass): one tyre submesh's
// vertices at rest, moved as engine/renderer/tyre_deformation.cpp moves them (this mirrors it line for
// line), written over the vertex buffer every pass draws and the position stream the depth-only
// passes read. Shares skin.comp's descriptor sets: the skin binding is unused.
// Vertex in engine/asset/mesh.h, 20 floats: position, colour, UV 0, normal, tangent, UV 1, outline
// normal.

#include "d3d_register.slang"
#include "shader_helpers.slang"

static const uint kVertexFloats = 20u;
static const uint kPosition = 0u;
static const uint kNormal = 8u;
static const uint kTangent = 11u;

// TyreDeformation's constants (engine/renderer/tyre_deformation.h).
static const float kTyreCarcassSpreadRadians = 1.2;
static const float kTyreMinimumLift = 0.25;
static const float kPi = 3.14159265358979;

[[vk::binding(0, 0)]] StructuredBuffer<float> bindPose : register(t0, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<float> vertices : register(u2, space0);
// Read as well: last frame's shape rolls into previousPositions before this one is written.
[[vk::binding(3, 0)]] RWStructuredBuffer<float> positions : register(u3, space0);
[[vk::binding(4, 0)]] RWStructuredBuffer<float> previousPositions : register(u4, space0);
// This frame's matrices: each tyre's two (PackTyreDeformation) among the skinned submeshes' joints.
[[vk::binding(0, 1)]] StructuredBuffer<float4x4> palette : register(t0, space1);

struct Constants
{
    uint vertexCount;
    uint paletteBase;
};
[[vk::push_constant]] ConstantBuffer<Constants> constants D3D_PUSH_CONSTANTS(0);

struct TyreDeformation
{
    bool onGround;
    float3 center;
    float3 axle;
    float outerRadius;
    float innerRadius;
    float axialCenter;
    float3 groundNormal;
    float groundOffset;
    float3 forward;
    float3 left;
    float carcassForward;
    float carcassLeft;
    float carcassTwist;
    float carcassBending;
    float bulge;
    float smoothing;
};

TyreDeformation Unpack(float4x4 a, float4x4 b)
{
    TyreDeformation d;
    d.center = ColumnOf(a, 0).xyz;
    d.outerRadius = ColumnOf(a, 0).w;
    d.axle = ColumnOf(a, 1).xyz;
    d.innerRadius = ColumnOf(a, 1).w;
    d.groundNormal = ColumnOf(a, 2).xyz;
    d.groundOffset = ColumnOf(a, 2).w;
    d.forward = ColumnOf(a, 3).xyz;
    d.axialCenter = ColumnOf(a, 3).w;
    d.left = ColumnOf(b, 0).xyz;
    d.onGround = ColumnOf(b, 0).w > 0.5;
    d.carcassForward = ColumnOf(b, 1).x;
    d.carcassLeft = ColumnOf(b, 1).y;
    d.carcassTwist = ColumnOf(b, 1).z;
    d.carcassBending = ColumnOf(b, 1).w;
    d.bulge = ColumnOf(b, 2).x;
    d.smoothing = ColumnOf(b, 2).y;
    return d;
}

float TyreSmoothPositive(float x, float width)
{
    if (x >= width)
    {
        return x;
    }
    if (x <= -width)
    {
        return 0.0;
    }
    return (x + width) * (x + width) / (4.0 * width);
}

float3 DeformTyrePoint(TyreDeformation d, float3 point)
{
    float3 offset = point - d.center;
    float axial = dot(offset, d.axle);
    float3 across = offset - d.axle * axial;
    float r = length(across);
    float span = d.outerRadius - d.innerRadius;
    if (!d.onGround || r < 1e-6 || span <= 0.0)
    {
        return point;
    }
    float3 radial = across / r;
    float h = clamp((r - d.innerRadius) / span, 0.0, 1.0);
    float lift = max(-dot(radial, d.groundNormal), kTyreMinimumLift);

    // In: out of the ground, and with the tread above.
    float penetration = d.groundOffset - dot(point, d.groundNormal);
    float3 treadPoint = d.center + d.axle * axial + radial * d.outerRadius;
    float treadPenetration = d.groundOffset - dot(treadPoint, d.groundNormal);
    float treadIn = TyreSmoothPositive(treadPenetration, d.smoothing) / lift;
    float moveIn = min(max(TyreSmoothPositive(penetration, d.smoothing) / lift, h * h * treadIn), max(r - d.innerRadius, 0.0));
    float3 moved = point - radial * moveIn;

    // The sidewall's bulge, away from the middle.
    float side = axial - d.axialCenter >= 0.0 ? 1.0 : -1.0;
    moved += d.axle * (side * d.bulge * sin(kPi * h) * treadIn);

    // The carcass, round the patch.
    float3 down = -d.groundNormal - d.axle * dot(-d.groundNormal, d.axle);
    float downLength = length(down);
    if (downLength > 1e-4)
    {
        down /= downLength;
        float height = (dot(d.center, d.groundNormal) - d.groundOffset) / max(-dot(down, d.groundNormal), 0.1);
        float patchCos = clamp(height / d.outerRadius, -1.0, 1.0);
        float patchHalfAngle = acos(patchCos);
        float patchHalfLength = d.outerRadius * sin(patchHalfAngle);
        float angle = acos(clamp(dot(radial, down), -1.0, 1.0));
        float spread = 1.0 - smoothstep(patchHalfAngle, patchHalfAngle + kTyreCarcassSpreadRadians, angle);
        float3 patchCenter = d.center + down * height;
        float x = clamp(dot(moved - patchCenter, d.forward), -patchHalfLength, patchHalfLength);
        float dx = x - d.carcassForward;
        float y = d.carcassLeft + d.carcassTwist * dx - 0.5 * d.carcassLeft * d.carcassBending * dx * dx;
        moved += (d.forward * d.carcassForward + d.left * y) * (h * h * spread);
    }
    return moved;
}

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
    TyreDeformation d = Unpack(palette[constants.paletteBase], palette[constants.paletteBase + 1u]);

    uint v = vertex * kVertexFloats;
    // What the deformation leaves alone (colour, UVs, the tangent's sign) is copied through.
    for (uint i = 0u; i < kVertexFloats; ++i)
    {
        vertices[v + i] = bindPose[v + i];
    }

    float3 restPosition = ReadVec3(v + kPosition);
    float3 position = restPosition;
    if (d.onGround)
    {
        // DeformTyreVertex: the normal and tangent from the deformation's differences.
        float3 restNormal = ReadVec3(v + kNormal);
        float3 restTangent = ReadVec3(v + kTangent);
        float step = 1e-3 * d.outerRadius;
        float3 normal = normalize(restNormal);
        float3 along = restTangent - normal * dot(restTangent, normal);
        if (dot(along, along) < 1e-8)
        {
            float3 helper = abs(normal.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
            along = cross(helper, normal);
        }
        along = normalize(along);
        float3 across = cross(normal, along);

        position = DeformTyrePoint(d, restPosition);
        float3 alongMoved = DeformTyrePoint(d, restPosition + along * step) - position;
        float3 acrossMoved = DeformTyrePoint(d, restPosition + across * step) - position;
        float3 normalMoved = cross(alongMoved, acrossMoved);
        float3 newNormal = dot(normalMoved, normalMoved) > 0.0 ? normalize(normalMoved) : normal;
        float3 tangent = alongMoved - newNormal * dot(alongMoved, newNormal);
        float3 newTangent = dot(tangent, tangent) > 0.0 ? normalize(tangent) : restTangent;
        WriteVec3(v + kPosition, position);
        WriteVec3(v + kNormal, newNormal);
        WriteVec3(v + kTangent, newTangent);
    }

    // Where the vertex was, for the motion vectors (triangle.vert).
    previousPositions[vertex * 3u] = positions[vertex * 3u];
    previousPositions[vertex * 3u + 1u] = positions[vertex * 3u + 1u];
    previousPositions[vertex * 3u + 2u] = positions[vertex * 3u + 2u];
    positions[vertex * 3u] = position.x;
    positions[vertex * 3u + 1u] = position.y;
    positions[vertex * 3u + 2u] = position.z;
}
