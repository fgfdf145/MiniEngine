#include <engine/renderer/ray_tracing_bvh.h>

#include <glm/ext/matrix_transform.hpp>

#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct Soup
{
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
};

// count small triangles scattered through a 10 m cube.
Soup RandomSoup(std::mt19937& rng, uint32_t count)
{
    std::uniform_real_distribution<float> place(-5.0f, 5.0f);
    std::uniform_real_distribution<float> offset(-0.6f, 0.6f);
    Soup soup;
    for (uint32_t triangle = 0; triangle < count; ++triangle)
    {
        const glm::vec3 center(place(rng), place(rng), place(rng));
        for (int corner = 0; corner < 3; ++corner)
        {
            soup.indices.push_back(static_cast<uint32_t>(soup.positions.size()));
            soup.positions.push_back(center + glm::vec3(offset(rng), offset(rng), offset(rng)));
        }
    }
    return soup;
}

Ray RandomRay(std::mt19937& rng)
{
    std::uniform_real_distribution<float> place(-8.0f, 8.0f);
    std::normal_distribution<float> gaussian(0.0f, 1.0f);
    Ray ray;
    ray.origin = glm::vec3(place(rng), place(rng), place(rng));
    ray.direction = glm::normalize(glm::vec3(gaussian(rng), gaussian(rng), gaussian(rng)));
    return ray;
}

// Walks every node: bounds contain the children or the leaf's triangles, leaves are small, every
// triangle sits in exactly one leaf, and no leaf is deeper than the traversal stack allows.
void CheckStructure(const MeshBvh& bvh, uint32_t triangleCount, const std::string& name)
{
    Require(bvh.triangles.size() == triangleCount, name + ": every triangle is kept");
    std::vector<uint32_t> seen(triangleCount, 0u);
    std::vector<std::pair<uint32_t, uint32_t>> pending = {{0u, 0u}};
    constexpr float kSlack = 1e-5f;
    while (!pending.empty())
    {
        const auto [index, depth] = pending.back();
        pending.pop_back();
        Require(depth <= kMaxBvhDepth, name + ": depth " + std::to_string(depth) + " exceeds the stack");
        const BvhNode& node = bvh.nodes[index];
        if (node.count == 0)
        {
            for (uint32_t child = node.first; child < node.first + 2; ++child)
            {
                Require(glm::all(glm::greaterThanEqual(bvh.nodes[child].boundsMin + kSlack, node.boundsMin)) &&
                            glm::all(glm::lessThanEqual(bvh.nodes[child].boundsMax - kSlack, node.boundsMax)),
                        name + ": a child's box leaves its parent's");
                pending.push_back({child, depth + 1});
            }
            continue;
        }
        Require(node.count <= kMaxLeafTriangles, name + ": leaf too large");
        for (uint32_t k = node.first; k < node.first + node.count; ++k)
        {
            ++seen[k];
            const BvhTriangle& triangle = bvh.triangles[k];
            const glm::vec3 corners[3] = {
                glm::vec3(triangle.v0),
                glm::vec3(triangle.v0 + triangle.e1),
                glm::vec3(triangle.v0 + triangle.e2)};
            for (const glm::vec3& corner : corners)
            {
                Require(glm::all(glm::greaterThanEqual(corner + kSlack, node.boundsMin)) &&
                            glm::all(glm::lessThanEqual(corner - kSlack, node.boundsMax)),
                        name + ": a triangle leaves its leaf's box");
            }
        }
    }
    for (uint32_t count : seen)
    {
        Require(count == 1, name + ": a triangle is in " + std::to_string(count) + " leaves");
    }
}

RayScene SceneOf(const MeshBvh& bvh)
{
    RayScene scene;
    AppendMesh(scene, bvh);
    const RayInstanceInput input{};
    BuildTopLevel(scene, std::span<const RayInstanceInput>(&input, 1));
    return scene;
}

void StructureInvariants()
{
    std::mt19937 rng(7u);
    const Soup soup = RandomSoup(rng, 3000);
    CheckStructure(BuildMeshBvh(soup.positions, soup.indices), 3000, "random soup");

    // Every centroid in one place: no SAH split exists, the build must still bound its leaves.
    Soup stacked;
    for (uint32_t triangle = 0; triangle < 5000; ++triangle)
    {
        for (int corner = 0; corner < 3; ++corner)
        {
            stacked.indices.push_back(static_cast<uint32_t>(stacked.positions.size()));
        }
        stacked.positions.push_back({0.0f, 0.0f, 0.0f});
        stacked.positions.push_back({1.0f, 0.0f, 0.0f});
        stacked.positions.push_back({0.0f, 1.0f, 0.0f});
    }
    CheckStructure(BuildMeshBvh(stacked.positions, stacked.indices), 5000, "stacked");

    // Sizes growing geometrically along a line: SAH peels one triangle per level, so only the
    // median fallback keeps the depth bounded.
    Soup peeling;
    for (uint32_t triangle = 0; triangle < 4000; ++triangle)
    {
        const float x = std::pow(1.01f, static_cast<float>(triangle));
        const float size = 0.001f * x;
        for (int corner = 0; corner < 3; ++corner)
        {
            peeling.indices.push_back(static_cast<uint32_t>(peeling.positions.size()));
        }
        peeling.positions.push_back({x, 0.0f, 0.0f});
        peeling.positions.push_back({x + size, 0.0f, 0.0f});
        peeling.positions.push_back({x, size, 0.0f});
    }
    CheckStructure(BuildMeshBvh(peeling.positions, peeling.indices), 4000, "peeling");

    const MeshBvh empty = BuildMeshBvh({}, {});
    Require(empty.nodes.empty() && empty.triangles.empty(), "an empty mesh has an empty hierarchy");
    RayScene scene;
    AppendMesh(scene, empty);
    const RayInstanceInput input{};
    BuildTopLevel(scene, std::span<const RayInstanceInput>(&input, 1));
    Ray ray;
    RayHit hit;
    Require(scene.instances.empty() && !TraceRay(scene, ray, hit), "an empty mesh is left out of the top level");
}

// The hierarchy finds exactly what testing every triangle finds.
void MatchesBruteForce()
{
    std::mt19937 rng(11u);
    const Soup soup = RandomSoup(rng, 2000);
    const MeshBvh bvh = BuildMeshBvh(soup.positions, soup.indices);
    const RayScene scene = SceneOf(bvh);
    uint32_t hits = 0;
    for (int trial = 0; trial < 3000; ++trial)
    {
        const Ray ray = RandomRay(rng);
        float bruteT = std::numeric_limits<float>::infinity();
        bool bruteFront = false;
        for (const BvhTriangle& triangle : bvh.triangles)
        {
            Ray limited = ray;
            limited.tMax = bruteT;
            float t = 0.0f;
            float u = 0.0f;
            float v = 0.0f;
            bool front = false;
            if (IntersectTriangle(triangle, limited, t, u, v, front))
            {
                bruteT = t;
                bruteFront = front;
            }
        }
        RayHit hit;
        const bool found = TraceRay(scene, ray, hit);
        Require(found == std::isfinite(bruteT), "closest hit: found disagrees with brute force");
        if (found)
        {
            ++hits;
            Require(std::abs(hit.t - bruteT) <= 1e-4f * std::max(1.0f, bruteT), "closest hit: t disagrees with brute force");
            Require(hit.frontFace == bruteFront, "closest hit: facing disagrees with brute force");
        }
        RayHit any;
        Require(TraceRay(scene, ray, any, true) == found, "any hit disagrees with closest hit");
    }
    Require(hits > 300, "the test rays hit something often enough to mean anything");
}

// Instances under rotation, non-uniform scale and a mirror: the same answers as the transformed
// triangles tested in world space, t in world units, and the front face as glTF defines it.
void InstancesMatchWorldSpace()
{
    std::mt19937 rng(23u);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    RayScene scene;
    std::vector<MeshBvh> meshes;
    std::vector<Soup> soups;
    for (int mesh = 0; mesh < 3; ++mesh)
    {
        soups.push_back(RandomSoup(rng, 300));
        meshes.push_back(BuildMeshBvh(soups.back().positions, soups.back().indices));
        AppendMesh(scene, meshes.back());
    }
    std::vector<RayInstanceInput> inputs;
    for (uint32_t instance = 0; instance < 12; ++instance)
    {
        glm::mat4 model(1.0f);
        model = glm::translate(model, glm::vec3(unit(rng) * 20.0f - 10.0f, unit(rng) * 4.0f - 2.0f, unit(rng) * 20.0f - 10.0f));
        model = glm::rotate(model, unit(rng) * 6.28f, glm::normalize(glm::vec3(unit(rng), unit(rng) + 0.1f, unit(rng))));
        glm::vec3 scale(0.3f + unit(rng), 0.3f + unit(rng), 0.3f + unit(rng));
        if (instance % 4 == 3)
        {
            scale.x = -scale.x;
        }
        model = glm::scale(model, scale);
        inputs.push_back(RayInstanceInput{instance % 3, model, instance, 0u});
    }
    const std::vector<uint32_t> sources = BuildTopLevel(scene, inputs);
    Require(sources.size() == inputs.size(), "every non-empty instance is kept");
    for (size_t leaf = 0; leaf < sources.size(); ++leaf)
    {
        Require(scene.instances[leaf].data.z == inputs[sources[leaf]].material, "the leaf order maps back to the inputs");
    }

    std::uniform_real_distribution<float> place(-14.0f, 14.0f);
    std::normal_distribution<float> gaussian(0.0f, 1.0f);
    uint32_t hits = 0;
    for (int trial = 0; trial < 3000; ++trial)
    {
        Ray ray;
        ray.origin = glm::vec3(place(rng), place(rng) * 0.3f, place(rng));
        ray.direction = glm::normalize(glm::vec3(gaussian(rng), gaussian(rng) * 0.3f, gaussian(rng)));
        float bruteT = std::numeric_limits<float>::infinity();
        uint32_t bruteMaterial = 0;
        bool bruteFront = false;
        for (const RayInstanceInput& input : inputs)
        {
            const Soup& soup = soups[input.mesh];
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(input.objectToWorld)));
            for (size_t index = 0; index < soup.indices.size(); index += 3)
            {
                const glm::vec3 local0 = soup.positions[soup.indices[index]];
                const glm::vec3 local1 = soup.positions[soup.indices[index + 1]];
                const glm::vec3 local2 = soup.positions[soup.indices[index + 2]];
                const glm::vec3 p0 = glm::vec3(input.objectToWorld * glm::vec4(local0, 1.0f));
                const glm::vec3 p1 = glm::vec3(input.objectToWorld * glm::vec4(local1, 1.0f));
                const glm::vec3 p2 = glm::vec3(input.objectToWorld * glm::vec4(local2, 1.0f));
                const BvhTriangle world{glm::vec4(p0, 0.0f), glm::vec4(p1 - p0, 0.0f), glm::vec4(p2 - p0, 0.0f)};
                Ray limited = ray;
                limited.tMax = bruteT;
                float t = 0.0f;
                float u = 0.0f;
                float v = 0.0f;
                bool windingFront = false;
                if (IntersectTriangle(world, limited, t, u, v, windingFront))
                {
                    bruteT = t;
                    bruteMaterial = input.material;
                    // glTF's front: the object-space normal carried by the inverse transpose.
                    const glm::vec3 normal = normalMatrix * glm::cross(local1 - local0, local2 - local0);
                    bruteFront = glm::dot(normal, ray.direction) < 0.0f;
                }
            }
        }
        RayHit hit;
        const bool found = TraceRay(scene, ray, hit);
        Require(found == std::isfinite(bruteT), "instances: found disagrees with world space");
        if (found)
        {
            ++hits;
            Require(std::abs(hit.t - bruteT) <= 1e-3f * std::max(1.0f, bruteT), "instances: t is not the world t");
            Require(scene.instances[hit.instance].data.z == bruteMaterial, "instances: a different instance was hit");
            Require(hit.frontFace == bruteFront, "instances: facing is not glTF's under the transform");
        }
    }
    Require(hits > 200, "the instance rays hit something often enough to mean anything");
}

// A filter that rejects hits lets the ray through to the next surface; a skipped instance is
// invisible.
void FilterAndSkip()
{
    // Two parallel quads facing +z, at z = 0 and z = -2.
    const std::vector<glm::vec3> quad = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    RayScene scene;
    AppendMesh(scene, BuildMeshBvh(quad, indices));
    const std::vector<RayInstanceInput> inputs = {
        RayInstanceInput{0, glm::mat4(1.0f), 5u, 0u},
        RayInstanceInput{0, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -2)), 6u, 0u}};
    BuildTopLevel(scene, inputs);

    Ray ray;
    ray.origin = glm::vec3(0.2f, 0.1f, 5.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    RayHit hit;
    Require(TraceRay(scene, ray, hit) && scene.instances[hit.instance].data.z == 5u && hit.frontFace, "the near quad's front");
    Require(std::abs(hit.t - 5.0f) < 1e-5f, "t is the distance");

    const RayHitFilter rejectNear = [&](const RayHit& candidate)
    {
        return scene.instances[candidate.instance].data.z != 5u;
    };
    Require(TraceRay(scene, ray, hit, false, rejectNear) && scene.instances[hit.instance].data.z == 6u, "a rejected hit passes the ray on");
    Require(std::abs(hit.t - 7.0f) < 1e-5f, "the far quad at 7 m");

    ray.origin = glm::vec3(0.2f, 0.1f, -5.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, 1.0f);
    Require(TraceRay(scene, ray, hit) && !hit.frontFace, "from behind the quad is a back face");

    for (RayInstance& instance : scene.instances)
    {
        instance.data.w = kRayInstanceSkip;
    }
    Require(!TraceRay(scene, ray, hit), "skipped instances are invisible");

    ray.tMax = 3.5f;
    for (RayInstance& instance : scene.instances)
    {
        instance.data.w = 0u;
    }
    Require(TraceRay(scene, ray, hit) && std::abs(hit.t - 3.0f) < 1e-5f, "the near quad at 3 m");
    ray.tMax = 2.9f;
    Require(!TraceRay(scene, ray, hit), "tMax limits the ray");
}
}

int main()
{
    try
    {
        StructureInvariants();
        MatchesBruteForce();
        InstancesMatchWorldSpace();
        FilterAndSkip();
    }
    catch (const std::exception& error)
    {
        std::cerr << "ray tracing bvh tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "ray tracing bvh tests passed\n";
    return 0;
}
