#pragma once

#include <engine/asset/mesh.h>
#include <engine/renderer/ray_tracing_bvh.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace me
{

// One mesh of the Material Editor's preview (docs/design/2026-10-09-material-editor-redesign-design.md):
// a submesh of the model, or a preview shape, and the material slot it draws with.
struct MaterialPreviewMesh
{
    std::shared_ptr<const MeshData> mesh;
    uint32_t materialSlot = 0;
    bool hasTexCoords = false;
    // The glTF node's scale (ModelSubmeshData::nodeScale), which a volume's thickness follows.
    glm::vec3 nodeScale{1.0f};
};

// The nearest surface a ray meets in the preview.
struct MaterialPreviewHit
{
    float t = std::numeric_limits<float>::infinity();
    uint32_t mesh = 0;
    // The triangle's index in its mesh: its corners are indices 3 * triangle to 3 * triangle + 2.
    uint32_t triangle = 0;
    // Barycentrics of the triangle's second and third corners.
    float u = 0.0f;
    float v = 0.0f;
};

// What a ray meets, offered to the caller before it counts: a Mask surface's cut-out parts and the
// surfaces a ray passes through (Blend ones, for shadow and occlusion rays) are refused.
struct MaterialPreviewCandidate
{
    uint32_t mesh = 0;
    uint32_t triangle = 0;
    float u = 0.0f;
    float v = 0.0f;
    float t = 0.0f;
    // The ray meets the triangle's front, its corners counter-clockwise seen from the ray's origin.
    bool frontFace = true;
};

// The preview's meshes ready to trace: one bounding volume hierarchy over every triangle, built as
// the ray scene builds a mesh's (BuildMeshBvh, binned SAH). One over all, not one per mesh under one
// over their boxes: a car's submeshes overlap (the body's box holds the cabin, the glass and the
// wheels), so a two-level walk visits most of them for every ray. The meshes are in the model's own
// space. Immutable once built, so the renders in flight share it.
class MaterialPreviewGeometry
{
  public:
    // Builds the hierarchy (on the calling thread; the editor builds it in a background task).
    static std::shared_ptr<const MaterialPreviewGeometry> Build(std::vector<MaterialPreviewMesh> meshes);

    const std::vector<MaterialPreviewMesh>& Meshes() const
    {
        return m_meshes;
    }
    // Every mesh's vertices, empty for no meshes.
    glm::vec3 BoundsMin() const
    {
        return m_boundsMin;
    }
    glm::vec3 BoundsMax() const
    {
        return m_boundsMax;
    }
    bool HasBounds() const
    {
        return m_boundsMin.x <= m_boundsMax.x;
    }
    uint64_t TriangleCount() const
    {
        return m_triangles.size();
    }

    // The nearest hit in (ray.tMin, ray.tMax) that accept takes; false when there is none.
    // accept(const MaterialPreviewCandidate&) -> bool.
    template <typename Accept>
    bool Intersect(const Ray& ray, MaterialPreviewHit& hit, Accept&& accept) const
    {
        return Trace<false>(ray, hit, accept);
    }
    // Whether anything accept takes lies in (ray.tMin, ray.tMax).
    template <typename Accept>
    bool Occluded(const Ray& ray, Accept&& accept) const
    {
        MaterialPreviewHit hit;
        return Trace<true>(ray, hit, accept);
    }

  private:
    // The slab test against a node's box: whether the ray enters it before tMax (and leaves it after
    // tMin), and where it enters.
    static bool HitsBox(const BvhNode& node, const glm::vec3& origin, const glm::vec3& inverseDirection, float tMin, float tMax, float& entry)
    {
        const glm::vec3 t0 = (node.boundsMin - origin) * inverseDirection;
        const glm::vec3 t1 = (node.boundsMax - origin) * inverseDirection;
        const glm::vec3 nearest = glm::min(t0, t1);
        const glm::vec3 farthest = glm::max(t0, t1);
        entry = std::max(std::max(nearest.x, nearest.y), std::max(nearest.z, tMin));
        const float exit = std::min(std::min(farthest.x, farthest.y), std::min(farthest.z, tMax));
        return entry <= exit;
    }

    template <bool AnyHit, typename Accept>
    bool Trace(const Ray& ray, MaterialPreviewHit& hit, Accept& accept) const
    {
        if (m_nodes.empty())
        {
            return false;
        }
        // Axis-parallel directions divide to a large finite number, not infinity, so a box face the
        // ray lies in gives 0 * large rather than NaN.
        const auto inverse = [](float d)
        {
            return 1.0f / (std::abs(d) > 1e-20f ? d : std::copysign(1e-20f, d));
        };
        const glm::vec3 inverseDirection(inverse(ray.direction.x), inverse(ray.direction.y), inverse(ray.direction.z));
        const BvhNode* const nodes = m_nodes.data();
        const BvhTriangle* const triangles = m_triangles.data();
        const uint32_t* const sources = m_sourceTriangles.data();
        const uint32_t* const meshOf = m_triangleMesh.data();
        const uint32_t* const meshFirst = m_meshFirstTriangle.data();
        float tMax = ray.tMax;
        bool found = false;
        uint32_t stack[kMaxBvhDepth + 2];
        uint32_t stackSize = 0;
        uint32_t nodeIndex = 0;
        float entry = 0.0f;
        if (!HitsBox(nodes[0], ray.origin, inverseDirection, ray.tMin, tMax, entry))
        {
            return false;
        }
        for (;;)
        {
            const BvhNode& node = nodes[nodeIndex];
            if (node.count == 0)
            {
                float entryA = 0.0f;
                float entryB = 0.0f;
                const bool hitA = HitsBox(nodes[node.first], ray.origin, inverseDirection, ray.tMin, tMax, entryA);
                const bool hitB = HitsBox(nodes[node.first + 1], ray.origin, inverseDirection, ray.tMin, tMax, entryB);
                if (hitA && hitB)
                {
                    // The nearer child first; the other waits on the stack.
                    const bool aFirst = entryA <= entryB;
                    stack[stackSize++] = aFirst ? node.first + 1 : node.first;
                    nodeIndex = aFirst ? node.first : node.first + 1;
                    continue;
                }
                if (hitA || hitB)
                {
                    nodeIndex = hitA ? node.first : node.first + 1;
                    continue;
                }
            }
            else
            {
                for (uint32_t index = node.first; index < node.first + node.count; ++index)
                {
                    // Moller-Trumbore on the vertex and the two edges from it.
                    const BvhTriangle& triangle = triangles[index];
                    const glm::vec3 e1(triangle.e1);
                    const glm::vec3 e2(triangle.e2);
                    const glm::vec3 p = glm::cross(ray.direction, e2);
                    const float determinant = glm::dot(e1, p);
                    if (std::abs(determinant) < 1e-14f)
                    {
                        continue;
                    }
                    const float inverseDeterminant = 1.0f / determinant;
                    const glm::vec3 s = ray.origin - glm::vec3(triangle.v0);
                    const float u = glm::dot(s, p) * inverseDeterminant;
                    if (u < 0.0f || u > 1.0f)
                    {
                        continue;
                    }
                    const glm::vec3 q = glm::cross(s, e1);
                    const float v = glm::dot(ray.direction, q) * inverseDeterminant;
                    if (v < 0.0f || u + v > 1.0f)
                    {
                        continue;
                    }
                    const float t = glm::dot(e2, q) * inverseDeterminant;
                    if (t <= ray.tMin || t >= tMax)
                    {
                        continue;
                    }
                    const uint32_t source = sources[index];
                    const uint32_t mesh = meshOf[source];
                    // The determinant is -dot(direction, e1 x e2): positive toward the front face.
                    const MaterialPreviewCandidate candidate{mesh, source - meshFirst[mesh], u, v, t, determinant > 0.0f};
                    if (!accept(candidate))
                    {
                        continue;
                    }
                    found = true;
                    tMax = t;
                    hit.t = t;
                    hit.mesh = candidate.mesh;
                    hit.triangle = candidate.triangle;
                    hit.u = u;
                    hit.v = v;
                    if constexpr (AnyHit)
                    {
                        return true;
                    }
                }
            }
            // Next from the stack, skipping nodes the ray now meets beyond its nearest hit.
            bool next = false;
            while (stackSize > 0)
            {
                nodeIndex = stack[--stackSize];
                if (HitsBox(nodes[nodeIndex], ray.origin, inverseDirection, ray.tMin, tMax, entry))
                {
                    next = true;
                    break;
                }
            }
            if (!next)
            {
                return found;
            }
        }
    }

    std::vector<MaterialPreviewMesh> m_meshes;
    std::vector<BvhNode> m_nodes;
    std::vector<BvhTriangle> m_triangles;
    // For each triangle in leaf order, its index over all meshes; and for each of those, its mesh,
    // and each mesh's first.
    std::vector<uint32_t> m_sourceTriangles;
    std::vector<uint32_t> m_triangleMesh;
    std::vector<uint32_t> m_meshFirstTriangle;
    glm::vec3 m_boundsMin{std::numeric_limits<float>::max()};
    glm::vec3 m_boundsMax{std::numeric_limits<float>::lowest()};
};
}
