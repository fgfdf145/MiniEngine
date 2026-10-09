#include "scene_raycast.h"

#include <engine/core/threading/task_system.h>
#include <engine/renderer/renderer_world.h>
#include <engine/scene/scene_world.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace me
{
namespace
{
// Triangles each task tests at least: a big map's ground is millions of them.
constexpr uint32_t kTrianglesPerRange = 16384;

// Möller-Trumbore: the ray's parameter where it crosses the triangle, either face, or a negative one.
double RayTriangle(const glm::dvec3& origin, const glm::dvec3& direction, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    const glm::dvec3 edge1 = b - a;
    const glm::dvec3 edge2 = c - a;
    const glm::dvec3 p = glm::cross(direction, edge2);
    const double determinant = glm::dot(edge1, p);
    if (std::abs(determinant) < 1e-14)
    {
        return -1.0;
    }
    const double inverse = 1.0 / determinant;
    const glm::dvec3 s = origin - a;
    const double u = glm::dot(s, p) * inverse;
    if (u < 0.0 || u > 1.0)
    {
        return -1.0;
    }
    const glm::dvec3 q = glm::cross(s, edge1);
    const double v = glm::dot(direction, q) * inverse;
    if (v < 0.0 || u + v > 1.0)
    {
        return -1.0;
    }
    return glm::dot(edge2, q) * inverse;
}

// A submesh whose bounds the ray passes through, with the ray in its own space (where the ray's
// parameter is the same as in the world for the unnormalised direction: a hit at t is t world metres
// along the unit ray), and where its triangles start in the count over all of them.
struct Candidate
{
    const MeshData* mesh = nullptr;
    entt::entity entity = entt::null;
    glm::dvec3 origin{0.0};
    glm::dvec3 direction{0.0};
    size_t firstTriangle = 0;
};
}

std::optional<SceneRayHit> RaycastScene(
    const RendererWorld& renderWorld, const ISceneWorld& scene, const glm::dvec3& origin, const glm::dvec3& direction, double maxDistance,
    entt::entity exclude)
{
    const double length = glm::length(direction);
    if (!(length > 0.0) || !(maxDistance > 0.0))
    {
        return std::nullopt;
    }
    const glm::dvec3 unit = direction / length;

    std::vector<Candidate> candidates;
    size_t triangles = 0;
    for (const std::shared_ptr<const CpuRenderSubmesh>& entry : renderWorld.GetRenderSubmeshes())
    {
        const CpuRenderSubmesh& submesh = *entry;
        if (submesh.entity == exclude || !submesh.mesh || !submesh.mesh->IsValid() || submesh.decal || submesh.water || submesh.skinned ||
            submesh.drawDistance.min > 0.0f || submesh.alphaMode == MaterialAlphaMode::Blend || !scene.IsValidEntity(submesh.entity))
        {
            continue;
        }
        const glm::dmat4 model(scene.GetModelMatrix(submesh.entity));
        // The submesh's bounding sphere first: most of a big map is nowhere near the ray.
        if (submesh.localBoundsRadius > 0.0f)
        {
            const glm::dvec3 center(model * glm::dvec4(glm::dvec3(submesh.localBoundsCenter), 1.0));
            const double scale = std::max({glm::length(glm::dvec3(model[0])), glm::length(glm::dvec3(model[1])), glm::length(glm::dvec3(model[2]))});
            const glm::dvec3 toCenter = center - origin;
            const double along = std::clamp(glm::dot(toCenter, unit), 0.0, maxDistance);
            if (glm::length(toCenter - unit * along) > submesh.localBoundsRadius * scale)
            {
                continue;
            }
        }
        const glm::dmat4 toLocal = glm::inverse(model);
        Candidate candidate;
        candidate.mesh = submesh.mesh.get();
        candidate.entity = submesh.entity;
        candidate.origin = glm::dvec3(toLocal * glm::dvec4(origin, 1.0));
        candidate.direction = glm::dvec3(toLocal * glm::dvec4(unit, 0.0));
        candidate.firstTriangle = triangles;
        candidates.push_back(candidate);
        triangles += submesh.mesh->indices.size() / 3;
    }
    if (candidates.empty())
    {
        return std::nullopt;
    }

    // Every candidate's triangles in ranges on all the workers; each range keeps its nearest, and the
    // nearest of those wins.
    std::mutex mutex;
    double best = maxDistance;
    entt::entity bestEntity = entt::null;
    TaskSystem::ParallelFor(
        static_cast<uint32_t>(triangles), kTrianglesPerRange, [&](uint32_t begin, uint32_t end)
        {
            double nearest = maxDistance;
            entt::entity nearestEntity = entt::null;
            auto candidate = std::upper_bound(
                                 candidates.begin(), candidates.end(), static_cast<size_t>(begin), [](size_t triangle, const Candidate& c)
                                 {
                                     return triangle < c.firstTriangle;
                                 }) -
                             1;
            for (uint32_t triangle = begin; triangle < end; ++triangle)
            {
                while (std::next(candidate) != candidates.end() && std::next(candidate)->firstTriangle <= triangle)
                {
                    ++candidate;
                }
                const std::vector<Vertex>& vertices = candidate->mesh->vertices;
                const std::vector<uint32_t>& indices = candidate->mesh->indices;
                const size_t corner = (triangle - candidate->firstTriangle) * 3;
                const uint32_t i0 = indices[corner];
                const uint32_t i1 = indices[corner + 1];
                const uint32_t i2 = indices[corner + 2];
                if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
                {
                    continue;
                }
                const auto at = [&](uint32_t index)
                {
                    const Vertex& vertex = vertices[index];
                    return glm::dvec3(vertex.position[0], vertex.position[1], vertex.position[2]);
                };
                const double t = RayTriangle(candidate->origin, candidate->direction, at(i0), at(i1), at(i2));
                if (t > 0.0 && t < nearest)
                {
                    nearest = t;
                    nearestEntity = candidate->entity;
                }
            }
            if (nearestEntity != entt::null)
            {
                const std::lock_guard lock(mutex);
                if (nearest < best)
                {
                    best = nearest;
                    bestEntity = nearestEntity;
                }
            }
        });
    if (bestEntity == entt::null)
    {
        return std::nullopt;
    }
    return SceneRayHit{origin + unit * best, best, bestEntity};
}
}
