#include "ray_tracing_bvh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace me
{

namespace
{
constexpr uint32_t kBinCount = 12;
constexpr uint32_t kStackSize = kMaxBvhDepth + 1;
// SAH splits up to this depth, median splits below it: with at most kMaxLeafTriangles per leaf,
// 2^(kMaxBvhDepth - kSahDepth) * 4 triangles (4 million) still fit under kMaxBvhDepth.
constexpr uint32_t kSahDepth = 16;

struct Bounds
{
    glm::vec3 min{std::numeric_limits<float>::infinity()};
    glm::vec3 max{-std::numeric_limits<float>::infinity()};

    void Grow(const glm::vec3& point)
    {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    void Grow(const Bounds& other)
    {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }

    float HalfArea() const
    {
        const glm::vec3 extent = max - min;
        if (extent.x < 0.0f)
        {
            return 0.0f;
        }
        return extent.x * extent.y + extent.y * extent.z + extent.z * extent.x;
    }
};

// Builds over primitives given by their bounds and centroids. order is permuted into leaf order;
// the nodes' first and count index into it.
std::vector<BvhNode> BuildOverPrimitives(
    const std::vector<Bounds>& primitiveBounds,
    const std::vector<glm::vec3>& centroids,
    std::vector<uint32_t>& order,
    uint32_t maxLeafSize)
{
    const uint32_t primitiveCount = static_cast<uint32_t>(primitiveBounds.size());
    order.resize(primitiveCount);
    // The loops below run over every primitive at every level of the tree. They go through raw
    // pointers rather than the vectors, so a Debug build's checked iterators and indices (MSVC's
    // iterator debugging, which the optimised Debug flags of optimized/CMakeLists.txt leave on)
    // do not multiply the build's time.
    const Bounds* const primitiveBoundsData = primitiveBounds.data();
    const glm::vec3* const centroidData = centroids.data();
    uint32_t* const orderData = order.data();
    for (uint32_t index = 0; index < primitiveCount; ++index)
    {
        orderData[index] = index;
    }
    std::vector<BvhNode> nodes;
    if (primitiveCount == 0)
    {
        return nodes;
    }
    nodes.reserve(2 * static_cast<size_t>(primitiveCount));
    nodes.push_back(BvhNode{});
    nodes[0].first = 0;
    nodes[0].count = primitiveCount;

    // (node, depth). Past kSahDepth splits go to the median, which bounds the depth for the
    // traversal stacks (kMaxBvhDepth).
    std::vector<std::pair<uint32_t, uint32_t>> pending = {{0u, 0u}};
    while (!pending.empty())
    {
        const auto [nodeIndex, depth] = pending.back();
        pending.pop_back();
        const uint32_t first = nodes[nodeIndex].first;
        const uint32_t count = nodes[nodeIndex].count;

        Bounds bounds;
        Bounds centroidBounds;
        for (uint32_t k = first; k < first + count; ++k)
        {
            bounds.Grow(primitiveBoundsData[orderData[k]]);
            centroidBounds.Grow(centroidData[orderData[k]]);
        }
        nodes[nodeIndex].boundsMin = bounds.min;
        nodes[nodeIndex].boundsMax = bounds.max;
        if (count <= 1)
        {
            continue;
        }

        // The cheapest binned split over the three axes, by surface area heuristic.
        float bestCost = std::numeric_limits<float>::infinity();
        int bestAxis = -1;
        uint32_t bestBin = 0;
        const glm::vec3 centroidExtent = centroidBounds.max - centroidBounds.min;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!(centroidExtent[axis] > 0.0f))
            {
                continue;
            }
            Bounds binBounds[kBinCount]{};
            uint32_t binCounts[kBinCount]{};
            const float scale = static_cast<float>(kBinCount) / centroidExtent[axis];
            const float axisMin = centroidBounds.min[axis];
            for (uint32_t k = first; k < first + count; ++k)
            {
                const uint32_t primitive = orderData[k];
                const uint32_t bin = std::min(
                    kBinCount - 1,
                    static_cast<uint32_t>((centroidData[primitive][axis] - axisMin) * scale));
                binBounds[bin].Grow(primitiveBoundsData[primitive]);
                ++binCounts[bin];
            }
            // Sweep from the left and the right: plane b splits bins [0, b] from [b + 1, end).
            std::array<float, kBinCount - 1> leftArea{};
            std::array<uint32_t, kBinCount - 1> leftCount{};
            Bounds left;
            uint32_t leftSum = 0;
            for (uint32_t bin = 0; bin + 1 < kBinCount; ++bin)
            {
                left.Grow(binBounds[bin]);
                leftSum += binCounts[bin];
                leftArea[bin] = left.HalfArea();
                leftCount[bin] = leftSum;
            }
            Bounds right;
            uint32_t rightSum = 0;
            for (uint32_t bin = kBinCount - 1; bin > 0; --bin)
            {
                right.Grow(binBounds[bin]);
                rightSum += binCounts[bin];
                const uint32_t plane = bin - 1;
                if (leftCount[plane] == 0 || rightSum == 0)
                {
                    continue;
                }
                const float cost = static_cast<float>(leftCount[plane]) * leftArea[plane] + static_cast<float>(rightSum) * right.HalfArea();
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestAxis = axis;
                    bestBin = plane;
                }
            }
        }

        // A leaf costs one intersection per primitive over the node's area; a split costs one
        // traversal step plus its children's (the step is folded in as a unit area).
        const float leafCost = static_cast<float>(count) * bounds.HalfArea();
        uint32_t middle = first;
        if (depth >= kSahDepth && count > maxLeafSize)
        {
            int axis = 0;
            if (centroidExtent.y > centroidExtent[axis])
            {
                axis = 1;
            }
            if (centroidExtent.z > centroidExtent[axis])
            {
                axis = 2;
            }
            middle = first + count / 2;
            std::nth_element(
                orderData + first, orderData + middle, orderData + first + count,
                [&](uint32_t a, uint32_t b)
                {
                    return centroidData[a][axis] < centroidData[b][axis];
                });
        }
        else if (bestAxis >= 0 && (bestCost + bounds.HalfArea() < leafCost || count > maxLeafSize))
        {
            const float scale = static_cast<float>(kBinCount) / centroidExtent[bestAxis];
            const auto isLeft = [&](uint32_t primitive)
            {
                const uint32_t bin = std::min(
                    kBinCount - 1,
                    static_cast<uint32_t>((centroidData[primitive][bestAxis] - centroidBounds.min[bestAxis]) * scale));
                return bin <= bestBin;
            };
            middle = static_cast<uint32_t>(std::partition(orderData + first, orderData + first + count, isLeft) - orderData);
        }
        else if (count > maxLeafSize)
        {
            // Every centroid in one place: split the run in half, which is as good as anything.
            middle = first + count / 2;
        }
        else
        {
            continue;
        }

        const uint32_t leftChild = static_cast<uint32_t>(nodes.size());
        nodes.push_back(BvhNode{});
        nodes.push_back(BvhNode{});
        nodes[leftChild].first = first;
        nodes[leftChild].count = middle - first;
        nodes[leftChild + 1].first = middle;
        nodes[leftChild + 1].count = first + count - middle;
        nodes[nodeIndex].first = leftChild;
        nodes[nodeIndex].count = 0;
        pending.push_back({leftChild + 1, depth + 1});
        pending.push_back({leftChild, depth + 1});
    }
    return nodes;
}

bool IntersectBounds(const glm::vec3& boundsMin, const glm::vec3& boundsMax, const glm::vec3& origin, const glm::vec3& inverseDirection, float tMin, float tMax, float& tEnter)
{
    const glm::vec3 t0 = (boundsMin - origin) * inverseDirection;
    const glm::vec3 t1 = (boundsMax - origin) * inverseDirection;
    const glm::vec3 near = glm::min(t0, t1);
    const glm::vec3 far = glm::max(t0, t1);
    tEnter = std::max(std::max(near.x, near.y), std::max(near.z, tMin));
    const float tExit = std::min(std::min(far.x, far.y), std::min(far.z, tMax));
    return tEnter <= tExit;
}

glm::vec3 SafeInverse(const glm::vec3& direction)
{
    const auto invert = [](float value)
    {
        constexpr float kTiny = 1e-20f;
        return 1.0f / (std::abs(value) > kTiny ? value : std::copysign(kTiny, value));
    };
    return glm::vec3(invert(direction.x), invert(direction.y), invert(direction.z));
}
}

MeshBvh BuildMeshBvh(std::span<const glm::vec3> positions, std::span<const uint32_t> indices)
{
    // Raw pointers in the per-triangle loops, as in BuildOverPrimitives.
    const glm::vec3* const positionData = positions.data();
    const uint32_t* const indexData = indices.data();
    const uint32_t triangleCount = static_cast<uint32_t>(indices.size() / 3);
    std::vector<Bounds> bounds(triangleCount);
    std::vector<glm::vec3> centroids(triangleCount);
    Bounds* const boundsData = bounds.data();
    glm::vec3* const centroidData = centroids.data();
    for (uint32_t triangle = 0; triangle < triangleCount; ++triangle)
    {
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            boundsData[triangle].Grow(positionData[indexData[3 * triangle + corner]]);
        }
        centroidData[triangle] = 0.5f * (boundsData[triangle].min + boundsData[triangle].max);
    }

    MeshBvh mesh;
    mesh.nodes = BuildOverPrimitives(bounds, centroids, mesh.sourceTriangles, kMaxLeafTriangles);
    mesh.triangles.resize(triangleCount);
    BvhTriangle* const triangleData = mesh.triangles.data();
    const uint32_t* const sourceData = mesh.sourceTriangles.data();
    for (uint32_t triangle = 0; triangle < triangleCount; ++triangle)
    {
        const uint32_t source = sourceData[triangle];
        const glm::vec3 v0 = positionData[indexData[3 * source]];
        const glm::vec3 v1 = positionData[indexData[3 * source + 1]];
        const glm::vec3 v2 = positionData[indexData[3 * source + 2]];
        triangleData[triangle] = BvhTriangle{glm::vec4(v0, 0.0f), glm::vec4(v1 - v0, 0.0f), glm::vec4(v2 - v0, 0.0f)};
    }
    return mesh;
}

std::vector<glm::vec3> GatherPositions(const float* firstPosition, size_t count, size_t strideBytes)
{
    std::vector<glm::vec3> positions(count);
    glm::vec3* const out = positions.data();
    const auto* bytes = reinterpret_cast<const unsigned char*>(firstPosition);
    for (size_t index = 0; index < count; ++index)
    {
        std::memcpy(&out[index], bytes + index * strideBytes, sizeof(glm::vec3));
    }
    return positions;
}

BoxBvh BuildBoxBvh(std::span<const glm::vec3> boxMins, std::span<const glm::vec3> boxMaxs, uint32_t maxLeafSize)
{
    std::vector<Bounds> bounds(boxMins.size());
    std::vector<glm::vec3> centroids(boxMins.size());
    for (size_t index = 0; index < boxMins.size(); ++index)
    {
        bounds[index].min = boxMins[index];
        bounds[index].max = boxMaxs[index];
        centroids[index] = 0.5f * (boxMins[index] + boxMaxs[index]);
    }
    BoxBvh result;
    result.nodes = BuildOverPrimitives(bounds, centroids, result.order, std::max(maxLeafSize, 1u));
    return result;
}

RayMeshRange AppendMesh(RayScene& scene, const MeshBvh& mesh)
{
    RayMeshRange range{};
    range.nodeOffset = static_cast<uint32_t>(scene.meshNodes.size());
    range.triangleOffset = static_cast<uint32_t>(scene.meshTriangles.size());
    range.nodeCount = static_cast<uint32_t>(mesh.nodes.size());
    if (!mesh.nodes.empty())
    {
        range.boundsMin = mesh.nodes[0].boundsMin;
        range.boundsMax = mesh.nodes[0].boundsMax;
    }
    scene.meshNodes.insert(scene.meshNodes.end(), mesh.nodes.begin(), mesh.nodes.end());
    scene.meshTriangles.insert(scene.meshTriangles.end(), mesh.triangles.begin(), mesh.triangles.end());
    scene.meshes.push_back(range);
    return range;
}

std::vector<uint32_t> BuildTopLevel(RayScene& scene, std::span<const RayInstanceInput> inputs)
{
    std::vector<glm::vec3> boxMins;
    std::vector<glm::vec3> boxMaxs;
    std::vector<uint32_t> kept;
    boxMins.reserve(inputs.size());
    boxMaxs.reserve(inputs.size());
    for (uint32_t index = 0; index < static_cast<uint32_t>(inputs.size()); ++index)
    {
        const RayInstanceInput& input = inputs[index];
        const RayMeshRange& range = scene.meshes[input.mesh];
        if (range.nodeCount == 0)
        {
            continue;
        }
        // The mesh root's box, carried to world space corner by corner.
        Bounds world;
        for (uint32_t corner = 0; corner < 8; ++corner)
        {
            const glm::vec3 local(
                (corner & 1u) ? range.boundsMax.x : range.boundsMin.x,
                (corner & 2u) ? range.boundsMax.y : range.boundsMin.y,
                (corner & 4u) ? range.boundsMax.z : range.boundsMin.z);
            world.Grow(glm::vec3(input.objectToWorld * glm::vec4(local, 1.0f)));
        }
        boxMins.push_back(world.min);
        boxMaxs.push_back(world.max);
        kept.push_back(index);
    }

    // One instance per leaf: instances overlap too much for a shared leaf to pay.
    BoxBvh top = BuildBoxBvh(boxMins, boxMaxs, 1u);
    scene.topNodes = std::move(top.nodes);
    scene.instances.clear();
    scene.instances.reserve(top.order.size());
    std::vector<uint32_t> sourceIndices;
    sourceIndices.reserve(top.order.size());
    for (uint32_t boxIndex : top.order)
    {
        const RayInstanceInput& input = inputs[kept[boxIndex]];
        const glm::mat4 worldToObject = glm::inverse(input.objectToWorld);
        RayInstance instance{};
        // glm is column-major: row r of the matrix is (m[0][r], m[1][r], m[2][r], m[3][r]).
        for (int row = 0; row < 3; ++row)
        {
            instance.worldToObject[row] = glm::vec4(worldToObject[0][row], worldToObject[1][row], worldToObject[2][row], worldToObject[3][row]);
        }
        const RayMeshRange& range = scene.meshes[input.mesh];
        instance.data = glm::uvec4(range.nodeOffset, range.triangleOffset, input.material, (input.flags & kRayInstanceFlagMask) | (input.mesh << kRayInstanceMeshShift));
        scene.instances.push_back(instance);
        sourceIndices.push_back(kept[boxIndex]);
    }
    return sourceIndices;
}

namespace
{
bool SameInput(const RayInstanceInput& a, const RayInstanceInput& b)
{
    return a.mesh == b.mesh && a.material == b.material && a.flags == b.flags &&
           std::memcmp(&a.objectToWorld, &b.objectToWorld, sizeof(glm::mat4)) == 0;
}
}

size_t IncrementalTopLevel::MaxMoved(size_t instanceCount)
{
    return std::max<size_t>(64, instanceCount / 16);
}

size_t IncrementalTopLevel::MaxInstances(size_t instanceCount)
{
    return instanceCount + MaxMoved(instanceCount);
}

size_t IncrementalTopLevel::MaxNodes(size_t instanceCount)
{
    // A tree of n leaves has 2n - 1 nodes; two of them and the root joining them.
    return 2 * instanceCount + 2 * MaxMoved(instanceCount) + 1;
}

void IncrementalTopLevel::Reset()
{
    m_valid = false;
}

size_t IncrementalTopLevel::MovedCount() const
{
    return m_movedList.size();
}

size_t IncrementalTopLevel::FullBuildCount() const
{
    return m_fullBuilds;
}

void IncrementalTopLevel::FullBuild(RayScene& scene, std::span<const RayInstanceInput> inputs)
{
    const std::vector<uint32_t> sources = BuildTopLevel(scene, inputs);
    m_fullNodes = scene.topNodes;
    m_fullInstances = scene.instances;
    m_slotOfInput.assign(inputs.size(), ~0u);
    for (uint32_t slot = 0; slot < static_cast<uint32_t>(sources.size()); ++slot)
    {
        m_slotOfInput[sources[slot]] = slot;
    }
    m_built.assign(inputs.begin(), inputs.end());
    m_last = m_built;
    m_moved.assign(inputs.size(), 0);
    m_movedList.clear();
    m_valid = true;
    ++m_fullBuilds;
}

bool IncrementalTopLevel::Update(RayScene& scene, std::span<const RayInstanceInput> inputs)
{
    if (!m_valid || inputs.size() != m_built.size())
    {
        FullBuild(scene, inputs);
        return true;
    }

    bool changed = false;
    for (uint32_t index = 0; index < static_cast<uint32_t>(inputs.size()); ++index)
    {
        if (SameInput(inputs[index], m_last[index]))
        {
            continue;
        }
        changed = true;
        if (m_moved[index] == 0 && !SameInput(inputs[index], m_built[index]))
        {
            m_moved[index] = 1;
            m_movedList.push_back(index);
        }
    }
    if (!changed)
    {
        return false;
    }
    if (m_movedList.size() > MaxMoved(inputs.size()))
    {
        FullBuild(scene, inputs);
        return true;
    }

    // The moved instances' own hierarchy, as it stands this frame.
    std::vector<RayInstanceInput> movedInputs;
    movedInputs.reserve(m_movedList.size());
    for (uint32_t index : m_movedList)
    {
        movedInputs.push_back(inputs[index]);
    }
    BuildTopLevel(scene, movedInputs);
    std::vector<BvhNode> movedNodes = std::move(scene.topNodes);
    std::vector<RayInstance> movedInstances = std::move(scene.instances);

    // The full build's instances, with the moved ones' old leaves skipped.
    scene.instances = m_fullInstances;
    for (uint32_t index : m_movedList)
    {
        if (m_slotOfInput[index] != ~0u)
        {
            scene.instances[m_slotOfInput[index]].data.w |= kRayInstanceSkip;
        }
    }
    const uint32_t movedInstanceBase = static_cast<uint32_t>(scene.instances.size());
    scene.instances.insert(scene.instances.end(), movedInstances.begin(), movedInstances.end());

    if (movedNodes.empty() || m_fullNodes.empty())
    {
        // One side has nothing to trace: the other is the whole tree.
        scene.topNodes = movedNodes.empty() ? m_fullNodes : movedNodes;
        if (!movedNodes.empty())
        {
            for (BvhNode& node : scene.topNodes)
            {
                node.first += node.count > 0 ? movedInstanceBase : 0u;
            }
        }
    }
    else
    {
        // [root][full root][moved root][rest of full][rest of moved]. An inner node's children are a
        // pair, so moving each tree's non-root nodes by one offset keeps every pair together.
        const uint32_t fullBase = 3;
        const uint32_t movedBase = fullBase + static_cast<uint32_t>(m_fullNodes.size()) - 1;
        const auto place = [](const BvhNode& node, uint32_t childBase, uint32_t leafBase)
        {
            BvhNode placed = node;
            placed.first = node.count > 0 ? node.first + leafBase : childBase + node.first - 1;
            return placed;
        };
        scene.topNodes.resize(1 + m_fullNodes.size() + movedNodes.size());
        BvhNode& root = scene.topNodes[0];
        root.boundsMin = glm::min(m_fullNodes[0].boundsMin, movedNodes[0].boundsMin);
        root.boundsMax = glm::max(m_fullNodes[0].boundsMax, movedNodes[0].boundsMax);
        root.first = 1;
        root.count = 0;
        scene.topNodes[1] = place(m_fullNodes[0], fullBase, 0);
        scene.topNodes[2] = place(movedNodes[0], movedBase, movedInstanceBase);
        for (size_t node = 1; node < m_fullNodes.size(); ++node)
        {
            scene.topNodes[fullBase + node - 1] = place(m_fullNodes[node], fullBase, 0);
        }
        for (size_t node = 1; node < movedNodes.size(); ++node)
        {
            scene.topNodes[movedBase + node - 1] = place(movedNodes[node], movedBase, movedInstanceBase);
        }
    }
    m_last.assign(inputs.begin(), inputs.end());
    return true;
}

bool IntersectTriangle(const BvhTriangle& triangle, const Ray& ray, float& t, float& u, float& v, bool& frontFace)
{
    const glm::vec3 e1(triangle.e1);
    const glm::vec3 e2(triangle.e2);
    const glm::vec3 p = glm::cross(ray.direction, e2);
    const float determinant = glm::dot(e1, p);
    if (determinant == 0.0f)
    {
        return false;
    }
    const float inverseDeterminant = 1.0f / determinant;
    const glm::vec3 s = ray.origin - glm::vec3(triangle.v0);
    u = glm::dot(s, p) * inverseDeterminant;
    if (u < 0.0f || u > 1.0f)
    {
        return false;
    }
    const glm::vec3 q = glm::cross(s, e1);
    v = glm::dot(ray.direction, q) * inverseDeterminant;
    if (v < 0.0f || u + v > 1.0f)
    {
        return false;
    }
    t = glm::dot(e2, q) * inverseDeterminant;
    if (!(t > ray.tMin && t < ray.tMax))
    {
        return false;
    }
    // det = dot(e1, d x e2) = -dot(d, e1 x e2): positive when the normal faces against the ray.
    frontFace = determinant > 0.0f;
    return true;
}

bool TraceRay(const RayScene& scene, const Ray& ray, RayHit& hit, bool anyHit, const RayHitFilter& filter)
{
    if (scene.topNodes.empty())
    {
        return false;
    }
    bool found = false;
    float closest = ray.tMax;
    const glm::vec3 worldInverse = SafeInverse(ray.direction);

    std::array<uint32_t, kStackSize> topStack{};
    uint32_t topSize = 0;
    topStack[topSize++] = 0;
    while (topSize > 0)
    {
        const BvhNode& node = scene.topNodes[topStack[--topSize]];
        float tEnter = 0.0f;
        if (!IntersectBounds(node.boundsMin, node.boundsMax, ray.origin, worldInverse, ray.tMin, closest, tEnter))
        {
            continue;
        }
        if (node.count == 0)
        {
            topStack[topSize++] = node.first + 1;
            topStack[topSize++] = node.first;
            continue;
        }
        for (uint32_t instanceIndex = node.first; instanceIndex < node.first + node.count; ++instanceIndex)
        {
            const RayInstance& instance = scene.instances[instanceIndex];
            if ((instance.data.w & (kRayInstanceSkip | kRayInstanceDynamic | kRayInstanceBlend)) != 0u)
            {
                continue;
            }
            Ray local = ray;
            local.origin = glm::vec3(
                glm::dot(instance.worldToObject[0], glm::vec4(ray.origin, 1.0f)),
                glm::dot(instance.worldToObject[1], glm::vec4(ray.origin, 1.0f)),
                glm::dot(instance.worldToObject[2], glm::vec4(ray.origin, 1.0f)));
            local.direction = glm::vec3(
                glm::dot(glm::vec3(instance.worldToObject[0]), ray.direction),
                glm::dot(glm::vec3(instance.worldToObject[1]), ray.direction),
                glm::dot(glm::vec3(instance.worldToObject[2]), ray.direction));
            const glm::vec3 localInverse = SafeInverse(local.direction);
            const uint32_t nodeOffset = instance.data.x;
            const uint32_t triangleOffset = instance.data.y;

            std::array<uint32_t, kStackSize> stack{};
            uint32_t size = 0;
            stack[size++] = 0;
            while (size > 0)
            {
                const BvhNode& meshNode = scene.meshNodes[nodeOffset + stack[--size]];
                float meshEnter = 0.0f;
                if (!IntersectBounds(meshNode.boundsMin, meshNode.boundsMax, local.origin, localInverse, local.tMin, closest, meshEnter))
                {
                    continue;
                }
                if (meshNode.count == 0)
                {
                    // Nearer child on top of the stack.
                    const BvhNode& left = scene.meshNodes[nodeOffset + meshNode.first];
                    const BvhNode& right = scene.meshNodes[nodeOffset + meshNode.first + 1];
                    float leftEnter = 0.0f;
                    float rightEnter = 0.0f;
                    const bool hitLeft = IntersectBounds(left.boundsMin, left.boundsMax, local.origin, localInverse, local.tMin, closest, leftEnter);
                    const bool hitRight = IntersectBounds(right.boundsMin, right.boundsMax, local.origin, localInverse, local.tMin, closest, rightEnter);
                    if (hitLeft && hitRight)
                    {
                        const bool leftFirst = leftEnter <= rightEnter;
                        stack[size++] = meshNode.first + (leftFirst ? 1u : 0u);
                        stack[size++] = meshNode.first + (leftFirst ? 0u : 1u);
                    }
                    else if (hitLeft)
                    {
                        stack[size++] = meshNode.first;
                    }
                    else if (hitRight)
                    {
                        stack[size++] = meshNode.first + 1;
                    }
                    continue;
                }
                for (uint32_t k = meshNode.first; k < meshNode.first + meshNode.count; ++k)
                {
                    local.tMax = closest;
                    RayHit candidate{};
                    if (!IntersectTriangle(scene.meshTriangles[triangleOffset + k], local, candidate.t, candidate.u, candidate.v, candidate.frontFace))
                    {
                        continue;
                    }
                    candidate.instance = instanceIndex;
                    candidate.triangle = triangleOffset + k;
                    // frontFace is the winding in the mesh's space, which is what glTF calls the
                    // front under a mirroring transform too (the rasteriser flips its culling).
                    if (filter && !filter(candidate))
                    {
                        continue;
                    }
                    closest = candidate.t;
                    hit = candidate;
                    found = true;
                    if (anyHit)
                    {
                        return true;
                    }
                }
            }
        }
    }
    return found;
}
}
