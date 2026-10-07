#include "tyre_deformation.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace me
{

std::array<glm::mat4, 2> PackTyreDeformation(const TyreDeformation& d)
{
    std::array<glm::mat4, 2> packed{glm::mat4(0.0f), glm::mat4(0.0f)};
    packed[0][0] = glm::vec4(d.center, d.outerRadius);
    packed[0][1] = glm::vec4(d.axle, d.innerRadius);
    packed[0][2] = glm::vec4(d.groundNormal, d.groundOffset);
    packed[0][3] = glm::vec4(d.forward, d.axialCenter);
    packed[1][0] = glm::vec4(d.left, d.onGround ? 1.0f : 0.0f);
    packed[1][1] = glm::vec4(d.carcassForward, d.carcassLeft, d.carcassTwist, d.carcassBending);
    packed[1][2] = glm::vec4(d.bulge, d.smoothing, 0.0f, 0.0f);
    return packed;
}

TyreDeformation UnpackTyreDeformation(const std::array<glm::mat4, 2>& packed)
{
    TyreDeformation d;
    d.center = glm::vec3(packed[0][0]);
    d.outerRadius = packed[0][0].w;
    d.axle = glm::vec3(packed[0][1]);
    d.innerRadius = packed[0][1].w;
    d.groundNormal = glm::vec3(packed[0][2]);
    d.groundOffset = packed[0][2].w;
    d.forward = glm::vec3(packed[0][3]);
    d.axialCenter = packed[0][3].w;
    d.left = glm::vec3(packed[1][0]);
    d.onGround = packed[1][0].w > 0.5f;
    d.carcassForward = packed[1][1].x;
    d.carcassLeft = packed[1][1].y;
    d.carcassTwist = packed[1][1].z;
    d.carcassBending = packed[1][1].w;
    d.bulge = packed[1][2].x;
    d.smoothing = packed[1][2].y;
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
        return 0.0f;
    }
    return (x + width) * (x + width) / (4.0f * width);
}

glm::vec3 DeformTyrePoint(const TyreDeformation& d, const glm::vec3& point)
{
    const glm::vec3 offset = point - d.center;
    const float axial = glm::dot(offset, d.axle);
    const glm::vec3 across = offset - d.axle * axial;
    const float r = glm::length(across);
    const float span = d.outerRadius - d.innerRadius;
    if (!d.onGround || r < 1e-6f || span <= 0.0f)
    {
        return point;
    }
    const glm::vec3 radial = across / r;
    const float h = std::clamp((r - d.innerRadius) / span, 0.0f, 1.0f);
    const float lift = std::max(-glm::dot(radial, d.groundNormal), kTyreMinimumLift);

    // In: out of the ground, and with the tread above.
    const float penetration = d.groundOffset - glm::dot(point, d.groundNormal);
    const glm::vec3 treadPoint = d.center + d.axle * axial + radial * d.outerRadius;
    const float treadPenetration = d.groundOffset - glm::dot(treadPoint, d.groundNormal);
    const float treadIn = TyreSmoothPositive(treadPenetration, d.smoothing) / lift;
    const float in = std::min(std::max(TyreSmoothPositive(penetration, d.smoothing) / lift, h * h * treadIn), std::max(r - d.innerRadius, 0.0f));
    glm::vec3 moved = point - radial * in;

    // The sidewall's bulge, away from the middle.
    const float side = axial - d.axialCenter >= 0.0f ? 1.0f : -1.0f;
    moved += d.axle * (side * d.bulge * std::sin(glm::pi<float>() * h) * treadIn);

    // The carcass, round the patch: the direction from the centre to the ground in the wheel's plane,
    // how far the ground is along it, and the patch's half angle and half length.
    glm::vec3 down = -d.groundNormal - d.axle * glm::dot(-d.groundNormal, d.axle);
    const float downLength = glm::length(down);
    if (downLength > 1e-4f)
    {
        down /= downLength;
        const float height = (glm::dot(d.center, d.groundNormal) - d.groundOffset) / std::max(-glm::dot(down, d.groundNormal), 0.1f);
        const float patchCos = std::clamp(height / d.outerRadius, -1.0f, 1.0f);
        const float patchHalfAngle = std::acos(patchCos);
        const float patchHalfLength = d.outerRadius * std::sin(patchHalfAngle);
        const float angle = std::acos(std::clamp(glm::dot(radial, down), -1.0f, 1.0f));
        const float spread =
            1.0f - glm::smoothstep(patchHalfAngle, patchHalfAngle + kTyreCarcassSpreadRadians, angle);
        const glm::vec3 patchCenter = d.center + down * height;
        const float x = std::clamp(glm::dot(moved - patchCenter, d.forward), -patchHalfLength, patchHalfLength);
        const float dx = x - d.carcassForward;
        const float y = d.carcassLeft + d.carcassTwist * dx - 0.5f * d.carcassLeft * d.carcassBending * dx * dx;
        moved += (d.forward * d.carcassForward + d.left * y) * (h * h * spread);
    }
    return moved;
}

TyreVertexFrame DeformTyreVertex(const TyreDeformation& d, const TyreVertexFrame& vertex)
{
    if (!d.onGround)
    {
        return vertex;
    }
    const float step = 1e-3f * d.outerRadius;
    const glm::vec3 normal = glm::normalize(vertex.normal);
    glm::vec3 along = vertex.tangent - normal * glm::dot(vertex.tangent, normal);
    if (glm::dot(along, along) < 1e-8f)
    {
        const glm::vec3 helper = std::abs(normal.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        along = glm::cross(helper, normal);
    }
    along = glm::normalize(along);
    const glm::vec3 across = glm::cross(normal, along);

    TyreVertexFrame result;
    result.position = DeformTyrePoint(d, vertex.position);
    const glm::vec3 alongMoved = DeformTyrePoint(d, vertex.position + along * step) - result.position;
    const glm::vec3 acrossMoved = DeformTyrePoint(d, vertex.position + across * step) - result.position;
    const glm::vec3 normalMoved = glm::cross(alongMoved, acrossMoved);
    result.normal = glm::dot(normalMoved, normalMoved) > 0.0f ? glm::normalize(normalMoved) : normal;
    const glm::vec3 tangent = alongMoved - result.normal * glm::dot(alongMoved, result.normal);
    result.tangent = glm::dot(tangent, tangent) > 0.0f ? glm::normalize(tangent) : vertex.tangent;
    return result;
}
}
