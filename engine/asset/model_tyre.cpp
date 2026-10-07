#include "model_tyre.h"

#include <engine/core/log/log.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>

namespace me
{

namespace
{
// A tyre reaches the wheel's outer radius (a rim only its lip, some three quarters of it) and has a
// sidewall: its bead sits this much of the radius further in.
constexpr float kTyreReach = 0.97f;
constexpr float kTyreMinimumSidewall = 0.08f;
// The finest step about the axle a refined tyre keeps between vertices, and the most parts one edge
// is cut into.
constexpr float kTyreStepRadians = glm::radians(2.0f);
constexpr uint32_t kMaxTyreCuts = 8;

// The wheel's frame: its centre, its axle and two directions across it.
struct WheelFrame
{
    glm::vec3 center{0.0f};
    glm::vec3 axle{1.0f, 0.0f, 0.0f};
    glm::vec3 u{0.0f, 1.0f, 0.0f};
    glm::vec3 v{0.0f, 0.0f, 1.0f};
};

WheelFrame MakeWheelFrame(const ModelTyreShape& shape)
{
    WheelFrame frame;
    frame.center = shape.center;
    frame.axle = glm::normalize(shape.axle);
    const glm::vec3 helper = std::abs(frame.axle.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    frame.u = glm::normalize(glm::cross(helper, frame.axle));
    frame.v = glm::cross(frame.axle, frame.u);
    return frame;
}

glm::vec3 PositionOf(const Vertex& vertex)
{
    return {vertex.position[0], vertex.position[1], vertex.position[2]};
}

// A point's angle about the axle, its distance from it and its place along it.
struct Cylindrical
{
    float angle = 0.0f;
    float radius = 0.0f;
    float axial = 0.0f;
};

Cylindrical ToCylindrical(const WheelFrame& frame, const glm::vec3& position)
{
    const glm::vec3 offset = position - frame.center;
    const float axial = glm::dot(offset, frame.axle);
    const glm::vec3 across = offset - frame.axle * axial;
    return {std::atan2(glm::dot(across, frame.v), glm::dot(across, frame.u)), glm::length(across), axial};
}

glm::vec3 RadialAt(const WheelFrame& frame, float angle)
{
    return frame.u * std::cos(angle) + frame.v * std::sin(angle);
}

glm::vec3 TangentialAt(const WheelFrame& frame, float angle)
{
    return frame.v * std::cos(angle) - frame.u * std::sin(angle);
}

// A direction as parts along the radius, around the wheel and along the axle at `angle`, which turn
// with the point when it moves round.
glm::vec3 ToLocal(const WheelFrame& frame, const glm::vec3& direction, float angle)
{
    return {glm::dot(direction, RadialAt(frame, angle)), glm::dot(direction, TangentialAt(frame, angle)), glm::dot(direction, frame.axle)};
}

glm::vec3 FromLocal(const WheelFrame& frame, const glm::vec3& local, float angle)
{
    return RadialAt(frame, angle) * local.x + TangentialAt(frame, angle) * local.y + frame.axle * local.z;
}

void WriteVec3(float* target, const glm::vec3& value)
{
    target[0] = value.x;
    target[1] = value.y;
    target[2] = value.z;
}

// The vertex at weights[i] of corners[i] on the surface of revolution through them: angle, radius and
// axial place blended (angles unwrapped about the first corner's), every other attribute blended
// straight, directions blended in the turning frame and normalised.
template <size_t Count>
Vertex BlendOnWheel(const WheelFrame& frame, const std::array<const Vertex*, Count>& corners, const std::array<float, Count>& weights)
{
    std::array<Cylindrical, Count> cylindrical{};
    for (size_t index = 0; index < Count; ++index)
    {
        cylindrical[index] = ToCylindrical(frame, PositionOf(*corners[index]));
        if (index > 0)
        {
            cylindrical[index].angle =
                cylindrical[0].angle + std::remainder(cylindrical[index].angle - cylindrical[0].angle, glm::two_pi<float>());
        }
    }

    Vertex result = *corners[0];
    Cylindrical blended{};
    glm::vec3 color(0.0f);
    glm::vec2 texCoord(0.0f);
    glm::vec2 texCoord1(0.0f);
    glm::vec3 normal(0.0f);
    glm::vec3 tangent(0.0f);
    glm::vec3 outline(0.0f);
    for (size_t index = 0; index < Count; ++index)
    {
        const Vertex& corner = *corners[index];
        const float weight = weights[index];
        const float angle = cylindrical[index].angle;
        blended.angle += weight * angle;
        blended.radius += weight * cylindrical[index].radius;
        blended.axial += weight * cylindrical[index].axial;
        color += weight * glm::vec3(corner.color[0], corner.color[1], corner.color[2]);
        texCoord += weight * glm::vec2(corner.texCoord[0], corner.texCoord[1]);
        texCoord1 += weight * glm::vec2(corner.texCoord1[0], corner.texCoord1[1]);
        normal += weight * ToLocal(frame, glm::vec3(corner.normal[0], corner.normal[1], corner.normal[2]), angle);
        tangent += weight * ToLocal(frame, glm::vec3(corner.tangent[0], corner.tangent[1], corner.tangent[2]), angle);
        outline += weight * ToLocal(frame, glm::vec3(corner.outlineNormal[0], corner.outlineNormal[1], corner.outlineNormal[2]), angle);
    }

    WriteVec3(result.position, frame.center + frame.axle * blended.axial + RadialAt(frame, blended.angle) * blended.radius);
    WriteVec3(result.color, color);
    result.texCoord[0] = texCoord.x;
    result.texCoord[1] = texCoord.y;
    result.texCoord1[0] = texCoord1.x;
    result.texCoord1[1] = texCoord1.y;
    const auto direction = [&](const glm::vec3& local, const float* fallback)
    {
        const glm::vec3 world = FromLocal(frame, local, blended.angle);
        const float length = glm::length(world);
        return length > 1e-6f ? world / length : glm::vec3(fallback[0], fallback[1], fallback[2]);
    };
    WriteVec3(result.normal, direction(normal, corners[0]->normal));
    WriteVec3(result.tangent, direction(tangent, corners[0]->tangent));
    if (glm::dot(outline, outline) > 0.0f)
    {
        WriteVec3(result.outlineNormal, direction(outline, corners[0]->outlineNormal));
    }
    return result;
}

// Whether a's position comes before b's, x then y then z: the order an edge's two ends are taken in
// whichever triangle cuts it, so copies of the edge get the same points.
bool PositionBefore(const Vertex& a, const Vertex& b)
{
    return std::tie(a.position[0], a.position[1], a.position[2]) < std::tie(b.position[0], b.position[1], b.position[2]);
}

float EdgeSpan(const WheelFrame& frame, const Vertex& a, const Vertex& b)
{
    const float angleA = ToCylindrical(frame, PositionOf(a)).angle;
    const float angleB = ToCylindrical(frame, PositionOf(b)).angle;
    return std::abs(std::remainder(angleB - angleA, glm::two_pi<float>()));
}
}

uint32_t RefineTyreMesh(MeshData& mesh, const ModelTyreShape& shape, float maxStepRadians)
{
    if (!mesh.IsValid() || mesh.IsSkinned() || maxStepRadians <= 0.0f)
    {
        return 1;
    }
    const WheelFrame frame = MakeWheelFrame(shape);
    float widest = 0.0f;
    for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3)
    {
        for (size_t edge = 0; edge < 3; ++edge)
        {
            widest = std::max(
                widest, EdgeSpan(frame, mesh.vertices[mesh.indices[index + edge]], mesh.vertices[mesh.indices[index + (edge + 1) % 3]]));
        }
    }
    const uint32_t cuts = std::clamp(static_cast<uint32_t>(std::ceil(widest / maxStepRadians - 1e-4f)), 1u, kMaxTyreCuts);
    if (cuts == 1)
    {
        return 1;
    }

    const std::vector<Vertex> source = mesh.vertices;
    std::vector<Vertex> vertices = source;
    std::vector<uint32_t> indices;
    indices.reserve(mesh.indices.size() * cuts * cuts);
    // An edge's inner points by its ends (first in PositionBefore order, ties by index) and the step
    // from the first: made once, shared by both triangles on it.
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> edgePoints;
    const auto edgePoint = [&](uint32_t a, uint32_t b, uint32_t stepFromA)
    {
        const bool swap = PositionBefore(source[b], source[a]) || (!PositionBefore(source[a], source[b]) && b < a);
        const uint32_t first = swap ? b : a;
        const uint32_t second = swap ? a : b;
        const uint32_t step = swap ? cuts - stepFromA : stepFromA;
        const auto key = std::make_tuple(first, second, step);
        if (const auto found = edgePoints.find(key); found != edgePoints.end())
        {
            return found->second;
        }
        const float t = static_cast<float>(step) / static_cast<float>(cuts);
        vertices.push_back(BlendOnWheel<2>(frame, {&source[first], &source[second]}, {1.0f - t, t}));
        const uint32_t made = static_cast<uint32_t>(vertices.size() - 1);
        edgePoints.emplace(key, made);
        return made;
    };

    std::vector<uint32_t> grid;
    for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3)
    {
        const uint32_t a = mesh.indices[index];
        const uint32_t b = mesh.indices[index + 1];
        const uint32_t c = mesh.indices[index + 2];
        // The triangle's points p(i, j) at weights ((n - i - j), i, j) / n of a, b and c.
        const auto at = [&](uint32_t i, uint32_t j) -> uint32_t
        {
            if (i == 0 && j == 0)
            {
                return a;
            }
            if (i == cuts)
            {
                return b;
            }
            if (j == cuts)
            {
                return c;
            }
            if (j == 0)
            {
                return edgePoint(a, b, i);
            }
            if (i == 0)
            {
                return edgePoint(a, c, j);
            }
            if (i + j == cuts)
            {
                return edgePoint(b, c, j);
            }
            const float n = static_cast<float>(cuts);
            vertices.push_back(BlendOnWheel<3>(
                frame, {&source[a], &source[b], &source[c]},
                {static_cast<float>(cuts - i - j) / n, static_cast<float>(i) / n, static_cast<float>(j) / n}));
            return static_cast<uint32_t>(vertices.size() - 1);
        };
        // Row j holds the points with i from 0 to cuts - j.
        grid.clear();
        std::vector<uint32_t> rowStart;
        for (uint32_t j = 0; j <= cuts; ++j)
        {
            rowStart.push_back(static_cast<uint32_t>(grid.size()));
            for (uint32_t i = 0; i + j <= cuts; ++i)
            {
                grid.push_back(at(i, j));
            }
        }
        const auto point = [&](uint32_t i, uint32_t j)
        {
            return grid[rowStart[j] + i];
        };
        // Each cell's triangles keep the original's winding.
        for (uint32_t j = 0; j < cuts; ++j)
        {
            for (uint32_t i = 0; i + j < cuts; ++i)
            {
                indices.insert(indices.end(), {point(i, j), point(i + 1, j), point(i, j + 1)});
                if (i + j + 1 < cuts)
                {
                    indices.insert(indices.end(), {point(i + 1, j), point(i + 1, j + 1), point(i, j + 1)});
                }
            }
        }
    }
    mesh.vertices = std::move(vertices);
    mesh.indices = std::move(indices);
    return cuts;
}

void PrepareModelTyres(LoadedModelData& modelData)
{
    if (!modelData.wheelRig.has_value())
    {
        return;
    }
    const ModelWheelRig& rig = *modelData.wheelRig;
    size_t tyres = 0;
    for (ModelSubmeshData& submesh : modelData.submeshes)
    {
        if (submesh.wheelPart != ModelWheelPart::Wheel || submesh.wheelCorner >= kModelWheelCornerCount || !submesh.mesh.IsValid() ||
            submesh.mesh.IsSkinned())
        {
            continue;
        }
        const ModelWheelRig::Corner& corner = rig.corners[submesh.wheelCorner];
        float innermost = corner.radius;
        float outermost = 0.0f;
        float axialMin = 0.0f;
        float axialMax = 0.0f;
        bool first = true;
        for (const Vertex& vertex : submesh.mesh.vertices)
        {
            const glm::vec3 offset = PositionOf(vertex) - corner.center;
            const float axial = glm::dot(offset, rig.axle);
            const float radius = glm::length(offset - rig.axle * axial);
            innermost = std::min(innermost, radius);
            outermost = std::max(outermost, radius);
            axialMin = first ? axial : std::min(axialMin, axial);
            axialMax = first ? axial : std::max(axialMax, axial);
            first = false;
        }
        if (outermost < kTyreReach * corner.radius || outermost - innermost < kTyreMinimumSidewall * corner.radius)
        {
            continue;
        }

        ModelTyreShape shape;
        shape.center = corner.center;
        shape.axle = rig.axle;
        shape.outerRadius = outermost;
        shape.innerRadius = innermost;
        shape.axialCenter = 0.5f * (axialMin + axialMax);
        shape.halfWidth = 0.5f * (axialMax - axialMin);
        const size_t before = submesh.mesh.vertices.size();
        const uint32_t cuts = RefineTyreMesh(submesh.mesh, shape, kTyreStepRadians);
        submesh.mesh.deformable = true;
        submesh.tyre = shape;
        // The deformed tyre bulges past its shape at rest and moves with its carcass: the bounds allow
        // for both, so it is not culled while still in view.
        const MeshBounds bounds = ComputeMeshBounds(submesh.mesh);
        submesh.boundsCenter = bounds.center;
        submesh.boundsRadius = bounds.radius + 0.5f * (outermost - innermost) + 0.05f * outermost;
        LOG_INFO(
            "Tyre '{}': radius {:.3f} m, bead {:.3f} m, width {:.3f} m; {} -> {} vertices ({} cuts an edge)",
            submesh.name, outermost, innermost, axialMax - axialMin, before, submesh.mesh.vertices.size(), cuts);
        ++tyres;
    }
    if (tyres > 0)
    {
        LOG_INFO("{} tyres deform on the ground", tyres);
    }
}
}
