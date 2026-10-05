#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace me
{

// The scene as the DDGI probe rays see it (docs/design/2026-09-27-ddgi-design.md): a bounding volume
// hierarchy per mesh (the bottom level), and one over the mesh instances rebuilt every frame (the top
// level). The GPU walks the same arrays byte for byte (shaders/vulkan/ray_tracing_common.glsl);
// TraceRay below is the CPU walk the tests and tools use.

// 32 bytes. An inner node (count 0) has its two children at first and first + 1; a leaf holds count
// primitives from first on: triangles in a mesh's hierarchy, instances in the top level's.
struct BvhNode
{
    glm::vec3 boundsMin{0.0f};
    uint32_t first = 0;
    glm::vec3 boundsMax{0.0f};
    uint32_t count = 0;
};

static_assert(sizeof(BvhNode) == 32, "BvhNode must stay two vec4 to match the shader");

// 48 bytes: a vertex and the two edges from it, the form Moller-Trumbore intersects. w unused.
struct BvhTriangle
{
    glm::vec4 v0{0.0f};
    glm::vec4 e1{0.0f};
    glm::vec4 e2{0.0f};
};

static_assert(sizeof(BvhTriangle) == 48, "BvhTriangle must stay three vec4 to match the shader");

// One mesh's bottom level: nodes from index 0 (the root), and its triangles in leaf order. Indices are
// local to the mesh, so any number of them concatenate without rewriting.
struct MeshBvh
{
    std::vector<BvhNode> nodes;
    std::vector<BvhTriangle> triangles;
    // For each triangle in leaf order, its index in the mesh's index list / 3.
    std::vector<uint32_t> sourceTriangles;
};

// Binned SAH (12 bins per axis), at most kMaxLeafTriangles triangles per leaf. Degenerate triangles
// are kept (they never hit). An empty mesh gives an empty hierarchy.
inline constexpr uint32_t kMaxLeafTriangles = 4;
// No leaf is deeper than this below its root (the root is depth 0), for meshes of up to 4 million
// triangles; the traversal stacks hold kMaxBvhDepth + 1 entries.
inline constexpr uint32_t kMaxBvhDepth = 36;
MeshBvh BuildMeshBvh(std::span<const glm::vec3> positions, std::span<const uint32_t> indices);
// count positions (three floats each) strideBytes apart from firstPosition, packed for BuildMeshBvh:
// a mesh's vertices hold more than their position. Here, with the build, so it is optimised in a
// Debug build too (optimized/CMakeLists.txt).
std::vector<glm::vec3> GatherPositions(const float* firstPosition, size_t count, size_t strideBytes);

// The same build over arbitrary boxes: what the top level uses. Returns the nodes and, in leaf order,
// the index of each box; the caller reorders its instances by it.
struct BoxBvh
{
    std::vector<BvhNode> nodes;
    std::vector<uint32_t> order;
};
BoxBvh BuildBoxBvh(std::span<const glm::vec3> boxMins, std::span<const glm::vec3> boxMaxs, uint32_t maxLeafSize);

// Where a mesh's hierarchy starts in the concatenated node and triangle arrays.
struct RayMeshRange
{
    uint32_t nodeOffset = 0;
    uint32_t triangleOffset = 0;
    // 0 for an empty mesh, which BuildTopLevel leaves out.
    uint32_t nodeCount = 0;
    // The root's box, which is all the top level needs of a mesh: the node arrays can go to the GPU
    // alone.
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

// 64 bytes, one per render submesh, in the top level's leaf order. worldToObject holds the rows of
// the inverse model matrix (a ray is carried into the mesh's space, where its t stays the world t
// because the direction is not renormalised). data: x node offset, y triangle offset, z the ray
// material, w flags (RAY_INSTANCE_*).
struct RayInstance
{
    glm::vec4 worldToObject[3]{};
    glm::uvec4 data{0u};
};

static_assert(sizeof(RayInstance) == 64, "RayInstance must stay four vec4 to match the shader");

// Instances with this flag are skipped by every ray (moving ones, see the DDGI design).
inline constexpr uint32_t kRayInstanceSkip = 1u;

// A scene ready to trace: every mesh's hierarchy concatenated, and this frame's instances and top
// level.
struct RayScene
{
    std::vector<BvhNode> meshNodes;
    std::vector<BvhTriangle> meshTriangles;
    std::vector<RayMeshRange> meshes;
    std::vector<RayInstance> instances;
    std::vector<BvhNode> topNodes;
};

// Appends a mesh's hierarchy and returns where it went.
RayMeshRange AppendMesh(RayScene& scene, const MeshBvh& mesh);

// What BuildTopLevel needs of each instance: which mesh, where, its material and flags.
struct RayInstanceInput
{
    uint32_t mesh = 0;
    glm::mat4 objectToWorld{1.0f};
    uint32_t material = 0;
    uint32_t flags = 0;
};

// Replaces the scene's instances and top level. The instances end up in leaf order; the returned
// vector maps each of them back to its input index.
std::vector<uint32_t> BuildTopLevel(RayScene& scene, std::span<const RayInstanceInput> inputs);

// The top level kept from frame to frame. Rebuilding it over every instance each frame cost tens of
// milliseconds on a map (31,000 instances) with nothing moving. A full build holds every instance; one
// whose input changes afterwards (a driven car's parts) is "moved": its leaf in the full build is
// skipped, and it goes into a small hierarchy built each frame, which a new root joins to the full
// build's. More moved instances than MaxMoved, or a different instance count, and the next update
// builds everything again.
class IncrementalTopLevel
{
  public:
    static size_t MaxMoved(size_t instanceCount);
    // Upper bounds of the scene's instances and top-level nodes, for sizing buffers.
    static size_t MaxInstances(size_t instanceCount);
    static size_t MaxNodes(size_t instanceCount);

    // Leaves scene.instances and scene.topNodes ready to trace for these inputs. False, touching
    // nothing, when the inputs are the same as the last call's.
    bool Update(RayScene& scene, std::span<const RayInstanceInput> inputs);
    void Reset();

    size_t MovedCount() const;
    size_t FullBuildCount() const;

  private:
    void FullBuild(RayScene& scene, std::span<const RayInstanceInput> inputs);

    bool m_valid = false;
    size_t m_fullBuilds = 0;
    std::vector<RayInstanceInput> m_built; // the inputs of the full build
    std::vector<RayInstanceInput> m_last;  // the inputs of the last update
    std::vector<BvhNode> m_fullNodes;
    std::vector<RayInstance> m_fullInstances;
    std::vector<uint32_t> m_slotOfInput; // input -> index in m_fullInstances, or ~0u when left out
    std::vector<uint8_t> m_moved;
    std::vector<uint32_t> m_movedList;
};

struct Ray
{
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
    float tMin = 0.0f;
    float tMax = std::numeric_limits<float>::infinity();
};

struct RayHit
{
    float t = std::numeric_limits<float>::infinity();
    // Index into RayScene::instances (leaf order).
    uint32_t instance = 0;
    // Index into RayScene::meshTriangles, the mesh's offset included.
    uint32_t triangle = 0;
    // Barycentrics of v1 and v2.
    float u = 0.0f;
    float v = 0.0f;
    // The triangle's winding faces the ray (counter-clockwise seen from the origin).
    bool frontFace = false;
};

// Decides whether a candidate hit counts (a Mask surface's coverage, a clear one); none accepts all.
using RayHitFilter = std::function<bool(const RayHit&)>;

// The nearest accepted hit, or none. anyHit stops at the first accepted one (shadow rays).
bool TraceRay(const RayScene& scene, const Ray& ray, RayHit& hit, bool anyHit = false, const RayHitFilter& filter = {});

// Moller-Trumbore against one triangle; t in (tMin, tMax).
bool IntersectTriangle(const BvhTriangle& triangle, const Ray& ray, float& t, float& u, float& v, bool& frontFace);
}
