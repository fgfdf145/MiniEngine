#include "physics_world.h"
#include "task_job_system.h"
#include "water_surface.h"
#include "vehicle_suspension.h"

#include <engine/suspension/suspension_corner.h>
#include <engine/tyre/tyre_brush.h>

// Jolt.h comes first: it sets up the configuration every other Jolt header depends on.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/AABBTree/TriangleCodec/TriangleCodecIndexed8BitPackSOA4Flags.h> // after MeshShape.h: it needs its types
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace me
{

namespace
{
namespace ObjectLayers
{
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::uint kCount = 2;
}

namespace BroadPhaseLayers
{
constexpr JPH::BroadPhaseLayer kStatic(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
constexpr JPH::uint kCount = 2;
}

// A wheel rolls on a surface whose normal is within 70 degrees of up. Steeper is a wall: the chassis
// hits it, but a wheel's cylinder cast would take its face for ground and climb it.
constexpr float kMinWheelSurfaceNormalY = 0.34f;
constexpr uint64_t kWheelsIgnoreBody = 1; // JPH::Body::GetUserData of a wall

// A mesh shape stores its vertices in 21 bits an axis across the mesh's bounds (a millimetre on a 2 km
// map) and leaves out the triangles whose corners then meet, but not those whose corners then lie in a
// line, or nearly. Such a triangle has no normal: the physics engine's sum for it comes out zero and
// normalizes to NaN, a floating-point exception in its contact code on its own threads (BeamNG's logs
// at 30 km/h) and a contact normal of NaN in the engine's. Imported meshes have them where they fill a seam. They are
// quantized here as the physics engine does it and dropped; the bounds of what is left are taken again.
void DropTrianglesFlatOnceStored(const JPH::VertexList& vertices, JPH::IndexedTriangleList& triangles)
{
    using Codec = JPH::TriangleCodecIndexed8BitPackSOA4Flags;
    constexpr double kMinStoredHeight = 2.0; // quantization steps
    for (int pass = 0; pass < 4 && !triangles.empty(); ++pass)
    {
        JPH::AABox bounds;
        for (const JPH::IndexedTriangle& triangle : triangles)
        {
            for (const JPH::uint32 index : triangle.mIdx)
            {
                bounds.Encapsulate(JPH::Vec3(vertices[index]));
            }
        }
        const JPH::Vec3 scale = JPH::Vec3::sReplicate(static_cast<float>(Codec::COMPONENT_MASK)) / JPH::Vec3::sMax(bounds.GetSize(), JPH::Vec3::sReplicate(1.0e-20f));
        const auto quantize = [&](JPH::uint32 index)
        {
            const JPH::UVec4 q = ((JPH::Vec3(vertices[index]) - bounds.mMin) * scale + JPH::Vec3::sReplicate(0.5f)).ToInt();
            return std::array<int64_t, 3>{q.GetX(), q.GetY(), q.GetZ()};
        };
        const size_t before = triangles.size();
        const auto flat = std::remove_if(
            triangles.begin(),
            triangles.end(),
            [&](const JPH::IndexedTriangle& triangle)
            {
                const std::array<int64_t, 3> a = quantize(triangle.mIdx[0]);
                const std::array<int64_t, 3> b = quantize(triangle.mIdx[1]);
                const std::array<int64_t, 3> c = quantize(triangle.mIdx[2]);
                const std::array<int64_t, 3> u = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                const std::array<int64_t, 3> v = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const std::array<int64_t, 3> w = {c[0] - b[0], c[1] - b[1], c[2] - b[2]};
                const auto lengthSq = [](const std::array<int64_t, 3>& e) { return double(e[0]) * double(e[0]) + double(e[1]) * double(e[1]) + double(e[2]) * double(e[2]); };
                const double x = double(u[1]) * double(v[2]) - double(u[2]) * double(v[1]);
                const double y = double(u[2]) * double(v[0]) - double(u[0]) * double(v[2]);
                const double z = double(u[0]) * double(v[1]) - double(u[1]) * double(v[0]);
                // |u x v| is the longest edge times the height: under kMinStoredHeight steps it is a sliver
                // whose normal the decoded floats may not keep either.
                const double longestSq = std::max({lengthSq(u), lengthSq(v), lengthSq(w)});
                return x * x + y * y + z * z < kMinStoredHeight * kMinStoredHeight * longestSq;
            });
        triangles.erase(flat, triangles.end());
        if (triangles.size() == before)
        {
            break;
        }
    }
}

// What lies beyond each edge of a ground triangle, for GroundEdgeContactFilter: kept in the triangle's
// user data, 10 bits an edge (edge i runs from corner i to corner i + 1). An edge with one neighbour holds
// the angle that neighbour's normal turns from the triangle's own, about the edge: positive toward the
// edge's outside, so a ridge (a convex edge) turns positive and a valley negative. Its neighbour's normal
// is then cos(turn) n + sin(turn) (e x n), n the triangle's normal and e the edge's direction.
namespace GroundEdgeTurns
{
constexpr uint32_t kBitsPerEdge = 10;
constexpr uint32_t kEdgeMask = (1u << kBitsPerEdge) - 1;
// No neighbour, or only the triangle's own back (a sheet's two sides): the mesh ends at the edge.
constexpr uint32_t kOpen = kEdgeMask;
// Three or more faces meet at the edge: no one turn says what is beyond it.
constexpr uint32_t kTangled = kEdgeMask - 1;
constexpr uint32_t kTurnSteps = kEdgeMask - 2; // -pi..pi
// Set on every triangle the turns were stored for.
constexpr uint32_t kStored = 1u << 31;

constexpr uint32_t Encode(float turn)
{
    const float share = (turn + std::numbers::pi_v<float>) / (2.0f * std::numbers::pi_v<float>);
    return static_cast<uint32_t>(std::clamp(share, 0.0f, 1.0f) * static_cast<float>(kTurnSteps) + 0.5f);
}

constexpr float Decode(uint32_t code)
{
    return static_cast<float>(code) / static_cast<float>(kTurnSteps) * 2.0f * std::numbers::pi_v<float> - std::numbers::pi_v<float>;
}

constexpr uint32_t EdgeCode(uint32_t userData, uint32_t edge)
{
    return (userData >> (edge * kBitsPerEdge)) & kEdgeMask;
}

// Stores the turns of the edges of every triangle in lists, its neighbours found in all of them (a
// walkable triangle's neighbour may be a wall's, or in another tile), by the vertices they share.
void Store(const JPH::VertexList& vertices, std::span<JPH::IndexedTriangleList> lists)
{
    std::vector<JPH::IndexedTriangle*> triangles;
    for (JPH::IndexedTriangleList& list : lists)
    {
        for (JPH::IndexedTriangle& triangle : list)
        {
            triangles.push_back(&triangle);
        }
    }
    std::vector<JPH::Vec3> normals(triangles.size());
    for (size_t index = 0; index < triangles.size(); ++index)
    {
        const JPH::Vec3 a(vertices[triangles[index]->mIdx[0]]);
        const JPH::Vec3 b(vertices[triangles[index]->mIdx[1]]);
        const JPH::Vec3 c(vertices[triangles[index]->mIdx[2]]);
        normals[index] = (b - a).Cross(c - a).NormalizedOr(JPH::Vec3::sZero());
        triangles[index]->mUserData = kStored;
    }
    struct EdgeEntry
    {
        uint64_t key = 0; // the lower vertex index above the higher
        uint32_t triangle = 0;
        uint32_t edge = 0;
        bool rising = false; // runs from the lower index to the higher
    };
    std::vector<EdgeEntry> edges;
    edges.reserve(triangles.size() * 3);
    for (uint32_t index = 0; index < triangles.size(); ++index)
    {
        for (uint32_t edge = 0; edge < 3; ++edge)
        {
            const uint32_t from = triangles[index]->mIdx[edge];
            const uint32_t to = triangles[index]->mIdx[(edge + 1) % 3];
            edges.push_back({static_cast<uint64_t>(std::min(from, to)) << 32 | std::max(from, to), index, edge, from < to});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const EdgeEntry& a, const EdgeEntry& b) { return a.key < b.key; });
    for (size_t first = 0; first < edges.size();)
    {
        size_t last = first + 1;
        while (last < edges.size() && edges[last].key == edges[first].key)
        {
            ++last;
        }
        for (size_t self = first; self < last; ++self)
        {
            const EdgeEntry& entry = edges[self];
            const JPH::Vec3 normal = normals[entry.triangle];
            uint32_t code = kOpen;
            if (normal != JPH::Vec3::sZero())
            {
                const JPH::Vec3 from(vertices[triangles[entry.triangle]->mIdx[entry.edge]]);
                const JPH::Vec3 to(vertices[triangles[entry.triangle]->mIdx[(entry.edge + 1) % 3]]);
                const JPH::Vec3 outward = (to - from).Normalized().Cross(normal);
                int found = 0;
                JPH::Vec3 beyond = JPH::Vec3::sZero();
                for (size_t other = first; other < last; ++other)
                {
                    if (other == self || normals[edges[other].triangle] == JPH::Vec3::sZero())
                    {
                        continue;
                    }
                    // A neighbour wound the same way runs the shared edge the other way; one that runs it
                    // the same way is wound against the triangle and faces the other side.
                    const JPH::Vec3 facing = edges[other].rising != entry.rising ? normals[edges[other].triangle] : -normals[edges[other].triangle];
                    if (facing.Dot(normal) < -0.9998f)
                    {
                        continue; // the triangle's own back
                    }
                    beyond = facing;
                    ++found;
                }
                if (found == 1)
                {
                    code = Encode(std::atan2(beyond.Dot(outward), beyond.Dot(normal)));
                }
                else if (found > 1)
                {
                    code = kTangled;
                }
            }
            triangles[entry.triangle]->mUserData |= code << (entry.edge * kBitsPerEdge);
        }
        first = last;
    }
}
} // namespace GroundEdgeTurns

// A mesh shape stores its vertices to the 21 bits an axis spread over its bounds (see
// DropTrianglesFlatOnceStored): a millimetre on a 2 km map, but BeamNG's ground plane, 218 km across,
// shared the collision mesh of the whole Grid Map, and its 10 cm steps moved every corner up to 5 cm
// and dropped the 20 cm tops of its bumps as slivers: the car's floor fell into the bumps. A mesh
// larger than kTileSize is cut into tiles that size by the triangles' centres. Triangles larger than a
// tile go together with those of their size (within four times), wherever they lie: an edge between
// two tiles is open to the physics engine, which takes a contact near an open edge for the edge's
// within a distance that grows with the triangle, and the plane's two halves in two tiles shoved the
// car across at 20 m/s anywhere near their diagonal. Kept together they meet as one mesh.
std::vector<JPH::IndexedTriangleList> CutIntoTiles(const JPH::VertexList& vertices, JPH::IndexedTriangleList triangles)
{
    constexpr float kTileSize = 1024.0f; // 0.5 mm steps
    std::vector<JPH::IndexedTriangleList> tiles;
    JPH::AABox bounds;
    for (const JPH::IndexedTriangle& triangle : triangles)
    {
        for (const JPH::uint32 index : triangle.mIdx)
        {
            bounds.Encapsulate(JPH::Vec3(vertices[index]));
        }
    }
    if (triangles.empty() || bounds.GetSize().ReduceMax() <= 2.0f * kTileSize)
    {
        tiles.push_back(std::move(triangles));
        return tiles;
    }
    std::map<std::array<int64_t, 4>, size_t> tileOf; // size step and cell (the first step's only)
    for (const JPH::IndexedTriangle& triangle : triangles)
    {
        const JPH::Vec3 a(vertices[triangle.mIdx[0]]);
        const JPH::Vec3 b(vertices[triangle.mIdx[1]]);
        const JPH::Vec3 c(vertices[triangle.mIdx[2]]);
        const float extent = (JPH::Vec3::sMax(JPH::Vec3::sMax(a, b), c) - JPH::Vec3::sMin(JPH::Vec3::sMin(a, b), c)).ReduceMax();
        int64_t step = 0;
        float size = kTileSize;
        while (extent > size)
        {
            size *= 4.0f;
            ++step;
        }
        const JPH::Vec3 centre = step == 0 ? (a + b + c) / 3.0f / size : JPH::Vec3::sZero();
        const std::array<int64_t, 4> key = {
            step, static_cast<int64_t>(std::floor(centre.GetX())), static_cast<int64_t>(std::floor(centre.GetY())), static_cast<int64_t>(std::floor(centre.GetZ()))};
        const auto [entry, added] = tileOf.try_emplace(key, tiles.size());
        if (added)
        {
            tiles.emplace_back();
        }
        tiles[entry->second].push_back(triangle);
    }
    return tiles;
}

// What a vehicle's wheels collide with: everything but the vehicle itself and the walls.
class WheelBodyFilter final : public JPH::BodyFilter
{
  public:
    explicit WheelBodyFilter(const JPH::BodyID& vehicle) : m_vehicle(vehicle)
    {
    }

    bool ShouldCollide(const JPH::BodyID& body) const override
    {
        return body != m_vehicle;
    }

    bool ShouldCollideLocked(const JPH::Body& body) const override
    {
        return body.GetUserData() != kWheelsIgnoreBody;
    }

  private:
    JPH::BodyID m_vehicle;
};

// A car's body meets the ground's edges and corners, not only its faces: a splitter a few centimetres
// up (Assetto Corsa's R34 has 6 cm) runs into the rim of a kerb top whose riser the map leaves out, or
// into a vertex standing out of the road (San Andreas has them, 20 cm). Pushed out along the edge,
// sideways to the surface, the body stops dead and is thrown up. Against walkable ground the body keeps
// a contact at an edge only where the faces meeting there hold it that way: its push lies between their
// normals, as a body resting across a ridge is held. Where nothing is beyond the edge (the kerb without
// its riser) and inside a face, it keeps only contacts along the face's own normal, from above or below:
// it rides over the kerb or the bump as a scraping splitter does. Walls (the steep body) still stop it.
class GroundEdgeContactFilter final : public JPH::ContactListener
{
  public:
    JPH::ValidateResult OnContactValidate(const JPH::Body& body1, const JPH::Body& body2, JPH::RVec3Arg baseOffset, const JPH::CollideShapeResult& result) override
    {
        const bool firstIsGround = body1.IsStatic() && body1.GetUserData() != kWheelsIgnoreBody;
        const bool secondIsGround = body2.IsStatic() && body2.GetUserData() != kWheelsIgnoreBody;
        if (firstIsGround == secondIsGround || !(firstIsGround ? body2 : body1).IsDynamic())
        {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }
        const JPH::Vec3 axis = result.mPenetrationAxis;
        const float length = axis.Length();
        if (!(length > 1e-12f))
        {
            return JPH::ValidateResult::AcceptContact;
        }
        // The penetration axis moves body 2 out of body 1: on the car it is the axis when the car is
        // body 2, and the reverse when it is body 1.
        const JPH::Vec3 push = (firstIsGround ? axis : -axis) / length;
        const JPH::Body& ground = firstIsGround ? body1 : body2;
        const JPH::SubShapeID& triangle = firstIsGround ? result.mSubShapeID1 : result.mSubShapeID2;
        const JPH::RVec3 point = baseOffset + (firstIsGround ? result.mContactPointOn1 : result.mContactPointOn2);
        bool keep = false;
        switch (JudgeEdge(ground, triangle, point, push))
        {
        case EdgeVerdict::Holds:
            keep = true;
            break;
        case EdgeVerdict::DoesNotHold:
            keep = false;
            break;
        case EdgeVerdict::NoEdge:
            keep = std::abs(push.Dot(ground.GetWorldSpaceSurfaceNormal(triangle, point))) >= kMinFaceAlignment;
            break;
        }
        return keep ? JPH::ValidateResult::AcceptContact : JPH::ValidateResult::RejectContact;
    }

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override
    {
        DropFalseManifold(body1, body2, manifold, settings);
    }

    void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override
    {
        DropFalseManifold(body1, body2, manifold, settings);
    }

  private:
    // How much deeper than the contact's own penetration its points may lie.
    static constexpr float kManifoldDepthSlack = 0.05f;

    // The physics engine builds a contact's points by clipping the two shapes' faces along its normal, and
    // where that normal is a triangle's own but not the contact's the points come out far apart: the R34's
    // floor resting on a BeamNG bump's top took the side normal of the bump's end (a wall) across its top
    // edge, its points 67 cm apart along it for a contact 2 cm clear. The position solver closes what the
    // points say, and moved the car 7 cm sideways and rolled it 4 degrees each step, the "thrown up" of the
    // suspension tests. A contact whose points lie deeper than its own depth is no contact: dropped.
    static void DropFalseManifold(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings)
    {
        if (body1.IsStatic() == body2.IsStatic())
        {
            return;
        }
        for (JPH::uint index = 0; index < manifold.mRelativeContactPointsOn1.size(); ++index)
        {
            const float penetration = (manifold.mRelativeContactPointsOn1[index] - manifold.mRelativeContactPointsOn2[index]).Dot(manifold.mWorldSpaceNormal);
            if (penetration > manifold.mPenetrationDepth + kManifoldDepthSlack)
            {
                settings.mIsSensor = true;
                return;
            }
        }
    }

    enum class EdgeVerdict
    {
        Holds,       // at an edge whose faces hold the body up along the push
        DoesNotHold, // at edges whose faces are known, none holding it so
        NoEdge,      // inside the face, at an edge with nothing (known) beyond it, or not pushed up
    };

    // Within 25 degrees of the face's normal (or its reverse): a face contact, not an edge's.
    static constexpr float kMinFaceAlignment = 0.9f;
    // How far from an edge the contact may lie and still be the edge's.
    static constexpr float kEdgeDistance = 0.01f;
    // How far out of the wedge between two faces' normals an edge's push may lean.
    static constexpr float kWedgeSlack = 0.0873f; // 5 degrees

    // A box's edge across the ridge of BeamNG's 20 cm bumps (45 degree sides) pushes straight up, along
    // the top's normal, and the triangle reported may be the side's: taken for a stray edge contact and
    // dropped, the floor sank into the bump, as it did between a log's facets. Only pushes that hold the
    // body up count: one leaning back between a stone's faces on BeamNG's rough road is as real, but it
    // stopped the splitter dead at 37 km/h and rolled the R34 over; it is judged as before, by the face.
    // An edge the physics engine keeps (an open one, or a seam between two meshes) may report a push
    // leaning back toward the body against a face that goes on flat beyond it: that is dropped, the
    // face beyond holds the body.
    static EdgeVerdict JudgeEdge(const JPH::Body& ground, const JPH::SubShapeID& triangle, JPH::RVec3Arg point, JPH::Vec3Arg push)
    {
        if (push.GetY() < kMinFaceAlignment)
        {
            return EdgeVerdict::NoEdge;
        }
        const JPH::Shape* shape = ground.GetShape();
        if (shape->GetSubType() != JPH::EShapeSubType::Mesh)
        {
            return EdgeVerdict::NoEdge;
        }
        const uint32_t turns = static_cast<const JPH::MeshShape*>(shape)->GetTriangleUserData(triangle);
        if ((turns & GroundEdgeTurns::kStored) == 0)
        {
            return EdgeVerdict::NoEdge;
        }
        // In the shape's own space, where its triangles are stored.
        const JPH::RMat44 toWorld = ground.GetCenterOfMassTransform();
        const JPH::Vec3 at(toWorld.InversedRotationTranslation() * point);
        const JPH::Vec3 out = toWorld.Multiply3x3Transposed(push);
        JPH::Shape::SupportingFace corners;
        shape->GetSupportingFace(triangle, JPH::Vec3::sAxisY(), JPH::Vec3::sOne(), JPH::Mat44::sIdentity(), corners);
        const JPH::Vec3 normal = corners.size() == 3 ? (corners[1] - corners[0]).Cross(corners[2] - corners[0]).NormalizedOr(JPH::Vec3::sZero()) : JPH::Vec3::sZero();
        if (normal == JPH::Vec3::sZero())
        {
            return EdgeVerdict::NoEdge;
        }
        // From below the face: its back, and its neighbours' backs.
        const float side = out.Dot(normal) >= 0.0f ? 1.0f : -1.0f;
        const auto angle = [](JPH::Vec3Arg a, JPH::Vec3Arg b) { return std::acos(std::clamp(a.Dot(b), -1.0f, 1.0f)); };
        bool atKnownEdge = false;
        for (uint32_t edge = 0; edge < 3; ++edge)
        {
            const JPH::Vec3 from = corners[edge];
            const JPH::Vec3 along = corners[(edge + 1) % 3] - from;
            const float t = std::clamp((at - from).Dot(along) / along.LengthSq(), 0.0f, 1.0f);
            if ((from + along * t - at).LengthSq() > kEdgeDistance * kEdgeDistance)
            {
                continue;
            }
            const uint32_t code = GroundEdgeTurns::EdgeCode(turns, edge);
            if (code == GroundEdgeTurns::kOpen || code == GroundEdgeTurns::kTangled)
            {
                return EdgeVerdict::NoEdge;
            }
            atKnownEdge = true;
            const float turn = GroundEdgeTurns::Decode(code);
            const JPH::Vec3 own = side * normal;
            const JPH::Vec3 beyond = side * (std::cos(turn) * normal + std::sin(turn) * along.Normalized().Cross(normal));
            if (angle(out, own) + angle(out, beyond) <= angle(own, beyond) + kWedgeSlack)
            {
                return EdgeVerdict::Holds;
            }
        }
        return atKnownEdge ? EdgeVerdict::DoesNotHold : EdgeVerdict::NoEdge;
    }
};

// Finds the ground with the wheel's cylinder, which rolls it over kerbs and seams a ray would catch
// on, but touches it as a thin disc in the wheel's middle plane does. The cylinder alone always
// touches on one of its two flat edges, the one its tilt to the ground puts lowest: as a wheel's
// camber to the road crosses zero (a front wheel steered against its caster, or the body rolling)
// its contact jumps the tyre's width across the tread, and the corner rises or sinks by half the
// width times the tilt. Steered to lock that rolled the R34 a third of a degree at a standstill.
// A tyre's patch stays near its middle; the suspension's own geometry already has the wheel's camber.
class VehicleCollisionTesterDisc final : public JPH::VehicleCollisionTesterCastCylinder
{
  public:
    explicit VehicleCollisionTesterDisc(JPH::ObjectLayer layer) : VehicleCollisionTesterCastCylinder(layer)
    {
    }

    // The suspension length (to the ground) past which the wheel can give no more (see Collide): with
    // an unsprung mass where its rim meets the ground, which moves with its hub. 0 (the default) for
    // the physics engine's own minimum.
    void SetFaceLength(size_t wheelIndex, float length)
    {
        if (wheelIndex < m_faceLengths.size())
        {
            m_faceLengths[wheelIndex] = length;
        }
    }

    bool Collide(JPH::PhysicsSystem& system, const JPH::VehicleConstraint& constraint, JPH::uint wheelIndex, JPH::RVec3Arg origin, JPH::Vec3Arg direction,
                 const JPH::BodyID& vehicleBody, JPH::Body*& outBody, JPH::SubShapeID& outSubShape, JPH::RVec3& outContactPosition, JPH::Vec3& outContactNormal,
                 float& outSuspensionLength) const override
    {
        if (!VehicleCollisionTesterCastCylinder::Collide(
                system, constraint, wheelIndex, origin, direction, vehicleBody, outBody, outSubShape, outContactPosition, outContactNormal, outSuspensionLength))
        {
            return false;
        }
        // The disc against the plane the cylinder found. The disc is the cylinder's middle, so it can
        // never meet the ground before the cylinder does: a shorter length means the plane is not the
        // ground's. At the crest of a bump the cast touches the edge but may report the steep side's
        // normal (BeamNG's 4 cm impact bumps, 55 degrees), and that side's plane, carried on past the
        // edge, stood 39 cm up in front of the wheel: the length went to nothing and the physics
        // engine's rigid stop threw the R34 up at 5 m/s from 18 km/h. Then the cylinder's own contact
        // stays, pushed along the wheel's own normal there: from the edge toward the axle.
        const JPH::RVec3 castPosition = outContactPosition;
        const JPH::Vec3 castNormal = outContactNormal;
        const float castLength = outSuspensionLength;
        const bool onPlane = TouchPlane(constraint, wheelIndex, origin, direction, outContactPosition, outContactNormal, outSuspensionLength);
        if (onPlane ? outSuspensionLength < castLength - kDiscLengthTolerance : castNormal.Dot(direction) > -1.0e-6f)
        {
            outContactPosition = castPosition;
            outSuspensionLength = castLength;
            outContactNormal = WheelNormalAt(constraint, wheelIndex, direction, origin + direction * castLength, castPosition, castNormal);
        }
        else if (!onPlane)
        {
            return false;
        }
        else
        {
            outSuspensionLength = std::max(outSuspensionLength, castLength);
        }
        // Past what the wheel can give (a kerb taller than the travel and the tyre take) its stop, rigid
        // or the rim's stiff spring, pushes along the contact's normal. On a kerb's edge that normal
        // leans back as far as 50 degrees, and the stop turned the car's speed into a leap: 9 m/s up at
        // 60 km/h. The ground's own face is pushed along instead, so the stop lifts the car onto the
        // kerb without throwing it. Within the travel the edge's normal stays: the tyre climbs it and
        // is slowed.
        const float faceLength = wheelIndex < m_faceLengths.size() && m_faceLengths[wheelIndex] > 0.0f
                                     ? m_faceLengths[wheelIndex]
                                     : constraint.GetWheel(wheelIndex)->GetSettings()->mSuspensionMinLength;
        if (outSuspensionLength < faceLength && outBody != nullptr)
        {
            // Only a face standing straighter than the contact's normal: the kerb's top, not the side
            // of a bump the cast came down on.
            const JPH::Vec3 face = outBody->GetWorldSpaceSurfaceNormal(outSubShape, outContactPosition);
            if (face.Dot(direction) < std::min(outContactNormal.Dot(direction), 0.0f))
            {
                outContactNormal = face;
            }
        }
        return true;
    }

    void PredictContactProperties(JPH::PhysicsSystem&, const JPH::VehicleConstraint& constraint, JPH::uint wheelIndex, JPH::RVec3Arg origin, JPH::Vec3Arg direction,
                                  const JPH::BodyID&, JPH::Body*&, JPH::SubShapeID&, JPH::RVec3& ioContactPosition, JPH::Vec3& ioContactNormal,
                                  float& ioSuspensionLength) const override
    {
        if (!TouchPlane(constraint, wheelIndex, origin, direction, ioContactPosition, ioContactNormal, ioSuspensionLength))
        {
            ioSuspensionLength = constraint.GetWheel(wheelIndex)->GetSettings()->mSuspensionMaxLength;
        }
    }

  private:
    // How much shorter than the cylinder's the disc's length may come out from rounding alone (m).
    static constexpr float kDiscLengthTolerance = 1.0e-4f;

    std::array<float, kVehicleWheelCount> m_faceLengths{};

    // The wheel's own surface normal where it meets `contact` with its centre at `center`: from the
    // contact toward the axle, square to it (a round wheel on an edge pushes along its radius).
    // `fallback` when the contact lies on the axle's line or the normal would not face the mount (up
    // `direction`, the suspension's).
    static JPH::Vec3 WheelNormalAt(const JPH::VehicleConstraint& constraint, JPH::uint wheelIndex, JPH::Vec3Arg direction, JPH::RVec3Arg center, JPH::RVec3Arg contact,
                                   JPH::Vec3Arg fallback)
    {
        const JPH::RMat44 wheel = constraint.GetWheelWorldTransform(wheelIndex, JPH::Vec3::sAxisY(), JPH::Vec3::sAxisX());
        const JPH::Vec3 axle = wheel.GetAxisY().Normalized();
        const JPH::Vec3 toCenter(center - contact);
        const JPH::Vec3 radial = toCenter - axle * toCenter.Dot(axle);
        const float length = radial.Length();
        return length > 1.0e-4f && radial.Dot(direction) < -0.1f * length ? radial / length : fallback;
    }

    // Moves the wheel along its suspension until the disc's lowest point toward the plane (through
    // `contact`, facing `normal`) lies on it; false when that is past full droop or the plane faces away.
    static bool TouchPlane(const JPH::VehicleConstraint& constraint, JPH::uint wheelIndex, JPH::RVec3Arg origin, JPH::Vec3Arg direction, JPH::RVec3& contact,
                           JPH::Vec3Arg normal, float& suspensionLength)
    {
        const JPH::WheelSettings& settings = *constraint.GetWheel(wheelIndex)->GetSettings();
        const float along = direction.Dot(normal);
        if (along > -1.0e-6f)
        {
            return false;
        }
        // The axle is the wheel transform's Y here (as the cylinder cast has it); the disc's lowest point
        // is its radius along the plane's inward normal, less its part along the axle.
        const JPH::RMat44 wheel = constraint.GetWheelWorldTransform(wheelIndex, JPH::Vec3::sAxisY(), JPH::Vec3::sAxisX());
        const JPH::Vec3 axle = wheel.GetAxisY().Normalized();
        JPH::Vec3 down = -normal + axle * normal.Dot(axle);
        const float downLength = down.Length();
        down = downLength > 1.0e-6f ? down / downLength : -normal;
        const JPH::RVec3 start = origin + down * settings.mRadius;
        const float length = JPH::Vec3(contact - start).Dot(normal) / along;
        if (length > settings.mSuspensionMaxLength)
        {
            return false;
        }
        suspensionLength = std::max(length, 0.0f);
        contact = start + direction * length;
        return true;
    }
};

constexpr JPH::uint kMaxBodies = 65536;
constexpr JPH::uint kBodyMutexCount = 0; // Jolt's default
constexpr JPH::uint kMaxBodyPairs = 65536;
constexpr JPH::uint kMaxContactConstraints = 10240;
constexpr size_t kTempAllocatorBytes = 16 * 1024 * 1024;

// The engine's task system when it runs; tests and tools that never start it get Jolt's own pool.
std::unique_ptr<JPH::JobSystem> MakePhysicsJobSystem()
{
    if (TaskSystem::IsRunning())
    {
        return std::make_unique<TaskJobSystem>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
    }
    return std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs,
        JPH::cMaxPhysicsBarriers,
        static_cast<int>(std::max(1u, std::thread::hardware_concurrency()) - 1));
}

void JoltTrace(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    char message[1024];
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "[Jolt] %s\n", message);
}

#ifdef JPH_ENABLE_ASSERTS
bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line)
{
    std::fprintf(stderr, "[Jolt] %s:%u: (%s) %s\n", file, line, expression, message != nullptr ? message : "");
    return true; // break into the debugger
}
#endif

// Jolt's allocator, factory and type registry are process-wide; the last world to go takes them down.
std::mutex g_joltMutex;
int g_joltUsers = 0;

void AcquireJolt()
{
    const std::lock_guard lock(g_joltMutex);
    if (g_joltUsers++ == 0)
    {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = JoltTrace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailed;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

void ReleaseJolt()
{
    const std::lock_guard lock(g_joltMutex);
    if (--g_joltUsers == 0)
    {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

JPH::Vec3 ToJolt(const glm::vec3& value)
{
    return JPH::Vec3(value.x, value.y, value.z);
}

JPH::RVec3 ToJoltPosition(const glm::dvec3& value)
{
    return JPH::RVec3(value.x, value.y, value.z);
}

JPH::Quat ToJolt(const glm::quat& value)
{
    return JPH::Quat(value.x, value.y, value.z, value.w).Normalized();
}

glm::vec3 FromJolt(JPH::Vec3Arg value)
{
    return glm::vec3(value.GetX(), value.GetY(), value.GetZ());
}

// A world position, kept in double (see PhysicsPose).
glm::dvec3 FromJoltPosition(JPH::RVec3Arg value)
{
    return glm::dvec3(value.GetX(), value.GetY(), value.GetZ());
}

glm::quat FromJolt(JPH::QuatArg value)
{
    return glm::quat(value.GetW(), value.GetX(), value.GetY(), value.GetZ());
}

PhysicsPose FromJolt(JPH::RMat44Arg transform)
{
    PhysicsPose pose;
    pose.position = FromJoltPosition(transform.GetTranslation());
    pose.rotation = FromJolt(transform.GetQuaternion());
    return pose;
}

PhysicsPose Interpolate(const PhysicsPose& from, const PhysicsPose& to, float alpha)
{
    PhysicsPose pose;
    pose.position = glm::mix(from.position, to.position, static_cast<double>(alpha));
    pose.rotation = glm::slerp(from.rotation, to.rotation, alpha);
    return pose;
}

// The pose of a vehicle and its wheels after one fixed step.
struct VehicleSnapshot
{
    PhysicsPose chassis;
    std::vector<VehicleWheelState> wheels;
};

// A wheel's friction curves from its axle's tyres: rising from nothing to the peak at the slip that
// gives it, then down to the share of the peak the physics engine's own curves fall to, at the
// same multiple of that slip.
void ApplyTyres(JPH::WheelSettingsWV& wheel, const VehicleTyreSettings& tyres)
{
    // The physics engine's default: 1.2 at a slip ratio of 0.06 and 3 degrees, 1.0 at 0.2 and 20.
    constexpr float kDefaultPostPeakShare = 1.0f / 1.2f;
    constexpr float kSlipRatioFall = 0.2f / 0.06f;
    constexpr float kSlipAngleFall = 20.0f / 3.0f;
    const float postPeakShare = tyres.postPeakShare > 0.0f ? std::min(tyres.postPeakShare, 1.0f) : kDefaultPostPeakShare;
    if (tyres.longitudinalGrip > 0.0f)
    {
        const float peak = tyres.peakSlipRatio > 0.0f ? tyres.peakSlipRatio : 0.06f;
        wheel.mLongitudinalFriction.Clear();
        wheel.mLongitudinalFriction.AddPoint(0.0f, 0.0f);
        wheel.mLongitudinalFriction.AddPoint(peak, tyres.longitudinalGrip);
        wheel.mLongitudinalFriction.AddPoint(peak * kSlipRatioFall, tyres.longitudinalGrip * postPeakShare);
    }
    if (tyres.lateralGrip > 0.0f)
    {
        const float peak = tyres.peakSlipAngleDegrees > 0.0f ? tyres.peakSlipAngleDegrees : 3.0f;
        wheel.mLateralFriction.Clear();
        wheel.mLateralFriction.AddPoint(0.0f, 0.0f);
        wheel.mLateralFriction.AddPoint(peak, tyres.lateralGrip);
        wheel.mLateralFriction.AddPoint(peak * kSlipAngleFall, tyres.lateralGrip * postPeakShare);
    }
    if (tyres.inertia > 0.0f)
    {
        wheel.mInertia = tyres.inertia;
    }
}

// How far a tyre deflects before its rim meets the ground through it, when the data gives no rim.
constexpr float kDefaultRimDeflection = 0.05f;
// Pinched between rim and ground, the tread and the folded sidewalls keep about this much (m).
constexpr float kPinchedTyreThickness = 0.015f;

// The ends of a hub's travel (the bump rubber pressed solid, the droop limit) and the rim's contact
// with the ground through the tyre are springs this many times the tyre's rate: stiff enough that
// against them the tyre takes the blow, soft enough to be stepped at 1 ms (their hub mode at
// omega dt about 0.35). What lands on them is the body's corner, not the hub: damped to this ratio
// against the corner's sprung mass, they give back some half of the speed they meet. With no data
// on either, both are assumptions.
constexpr double kEndStopTyreRates = 20.0;
constexpr double kEndStopDampingRatio = 0.7;
// The most those stiff springs give at their hardest (some 200 kN), kept clear before the physics
// engine's rigid stop.
constexpr float kEndStopGive = 0.03f;

// An end stop's push (N) pressed `depth` (m) into it and closing at `closing` (m/s), with its slopes
// along both. Its damping is held within its spring's force, as a rubber's loss follows its load: it
// starts from nothing at the touch (in full, a rim meeting a kerb at 8 m/s took 200 kN from the
// damper within the first 4 mm), and never pulls.
struct EndStopPush
{
    double force = 0.0;
    double stiffness = 0.0;
    double damping = 0.0;
};

EndStopPush PushOfEndStop(double rate, double damping, double depth, double closing)
{
    const double spring = rate * depth;
    const double damper = damping * closing;
    if (damper >= spring)
    {
        return {2.0 * spring, 2.0 * rate, 0.0};
    }
    if (damper <= -spring)
    {
        return {};
    }
    return {spring + damper, rate, damping};
}

// Rolling resistance as dry friction on a wheel's spin, the moment (N m) to apply with the step's
// `torque` already on it: up to `limit` it holds the wheel still, as a parked car stays put until pushed
// with about the coefficient's share of its weight; past it the roll is opposed by `limit`. Judged on
// the spin the step would leave, so it stops the wheel and never turns it back (the physics engine's
// brakes work the same way). Smoothed through the standstill instead (a viscous drag below some speed),
// it held nothing back: a few newtons left from the suspension settling rolled the R34 on its hub
// masses at 2 mm/s for over a minute.
float RollingResistanceTorque(float wheelSpeed, float torque, float inertia, float limit, float dt)
{
    return std::clamp(-(wheelSpeed * inertia / dt + torque), -limit, limit);
}

// How far the axle's tyre deflects before the rim meets the ground: its sidewall's height (radius
// less the rim's) less what stays pinched between them.
float RimDeflection(const VehicleSettings& settings, size_t wheelIndex)
{
    const VehicleTyreSettings& tyres = settings.tyres[wheelIndex];
    const float radius = GetVehicleWheelMount(settings, wheelIndex).radius;
    if (tyres.rimRadius <= 0.0f || tyres.rimRadius >= radius)
    {
        return kDefaultRimDeflection;
    }
    return std::max(radius - tyres.rimRadius - kPinchedTyreThickness, 0.02f);
}

// The hub's travel from the design position to the end of its bump: the bump stop's start and 4 cm
// of rubber.
float MultibodyBumpTravel(const VehicleSuspensionAxle& axle)
{
    return std::max(axle.bumpStopTravel, 0.03f) + 0.04f;
}

// With an unsprung mass the ground may come this much closer to the mount than the hub's full bump
// before the physics engine's rigid stop: the tyre's deflection to its rim, and the give of the end
// stops at the hub and the rim.
float TyreDeflectionRoom(const VehicleSettings& settings, size_t wheelIndex)
{
    return RimDeflection(settings, wheelIndex) + 2.0f * kEndStopGive;
}

// The multibody suspension's straight-spring stand-in: the mount sits this far above the wheel's
// design centre, enough for the bump travel of either axle and the tyre's deflection under it.
float MultibodyDesignLength(const VehicleSettings& settings)
{
    const float bump = std::max(MultibodyBumpTravel(settings.frontSuspension), MultibodyBumpTravel(settings.rearSuspension));
    const float tyre = std::max({TyreDeflectionRoom(settings, 0), TyreDeflectionRoom(settings, 2), kDefaultRimDeflection});
    return bump + 0.05f + tyre;
}

// A wheel's tyre spring and damper: its tyre's, or for a car imported before the tyre library its axle's.
float TyreVerticalRate(const VehicleSettings& settings, size_t index)
{
    const VehicleSuspensionAxle& axle = index < 2 ? settings.frontSuspension : settings.rearSuspension;
    return settings.tyres[index].verticalRate > 0.0f ? settings.tyres[index].verticalRate : axle.tyreRate;
}

float TyreVerticalDamping(const VehicleSettings& settings, size_t index)
{
    const VehicleSuspensionAxle& axle = index < 2 ? settings.frontSuspension : settings.rearSuspension;
    return settings.tyres[index].verticalRate > 0.0f ? settings.tyres[index].verticalDamping : std::max(axle.tyreDamping, 0.0f);
}

// The axle's wheels are masses of their own on tyre springs (the data gives hub mass and tyre rate):
// see PhysicsWorld::Impl::UpdateCorners.
bool HasUnsprungMass(const VehicleSuspensionAxle& axle)
{
    return axle.hubMass > 0.0f && axle.tyreRate > 0.0f;
}

// A wheel's hub mass when it rides on a tyre spring of its own (the multibody suspension's), else 0:
// the body carries it.
double SeparateHubMass(const VehicleSettings& settings, size_t index)
{
    const VehicleSuspensionAxle& axle = index < 2 ? settings.frontSuspension : settings.rearSuspension;
    return HasSuspensionGeometry(settings) && HasUnsprungMass(axle) ? axle.hubMass : 0.0;
}

// The body's own mass: the car's (Assetto Corsa's TOTALMASS, which has the hubs in it) less the hubs
// that ride on their own tyre springs, as the game takes its chassis' mass. The data's weight split,
// centre of mass height and inertia box are this body's.
double SprungMass(const VehicleSettings& settings)
{
    const double mass = std::max(settings.massKg, 1.0f);
    double hubs = 0.0;
    for (size_t index = 0; index < kVehicleWheelCount; ++index)
    {
        hubs += SeparateHubMass(settings, index);
    }
    return std::max(mass - hubs, 0.5 * mass);
}

// The physics engine's spring carries the tyre's force, which the suspension sets each step through
// the preload; this stiffness only keeps the spring defined, small enough that the body's motion
// within a step hardly changes the force.
constexpr float kTyreCarrierStiffness = 100.0f;

// A hub moving slower than this (m/s) has settled.
constexpr double kHubSettledRate = 5e-4;

// Where the wheel's centre is when the car rests: where the model draws it.
glm::vec3 MultibodyRestCenter(const VehicleSettings& settings, size_t index)
{
    return GetVehicleWheelMount(settings, index).center - glm::vec3(0.0f, ComputeRestSuspensionLength(settings, 9.81f), 0.0f);
}

// What a wheel's spring carries at rest: its share of the body's weight by where the body's centre of
// mass sits between the axles. A hub with a mass of its own puts its weight straight on the tyre.
double MultibodySpringLoad(const VehicleSettings& settings, size_t index)
{
    const glm::vec3 com = settings.chassisCenter + settings.centerOfMassOffset;
    const float frontZ = MultibodyRestCenter(settings, 0).z;
    const float rearZ = MultibodyRestCenter(settings, 2).z;
    const float frontShare = std::abs(frontZ - rearZ) > 1e-3f ? std::clamp((com.z - rearZ) / (frontZ - rearZ), 0.05f, 0.95f) : 0.5f;
    const bool front = index < 2;
    return 0.5 * SprungMass(settings) * 9.81 * (front ? frontShare : 1.0f - frontShare);
}

// Where the wheel's centre is at the design position (travel 0, the hardpoints' reference): where the
// model draws it, less how far a rod length's springs (Assetto Corsa's ROD_LENGTH) move it from there to
// where the car rests. Without a rod length the car rests at the design position.
glm::vec3 MultibodyDesignCenter(const VehicleSettings& settings, size_t index)
{
    return MultibodyRestCenter(settings, index) - VehicleRestWheelOffset(settings, index, MultibodySpringLoad(settings, index));
}

// The load each axle's wheel carries standing still: the body's weight by where its centre of mass sits
// between the axles, and the wheel's own hub.
float StaticWheelLoad(const VehicleSettings& settings, bool front)
{
    float frontShare = settings.frontWeightShare;
    if (!(frontShare > 0.0f && frontShare < 1.0f))
    {
        const float com = (settings.chassisCenter + settings.centerOfMassOffset).z;
        const float frontZ = GetVehicleWheelMount(settings, 0).center.z;
        const float rearZ = GetVehicleWheelMount(settings, 2).center.z;
        frontShare = std::abs(frontZ - rearZ) > 1e-3f ? std::clamp((com - rearZ) / (frontZ - rearZ), 0.05f, 0.95f) : 0.5f;
    }
    const double body = 0.5 * SprungMass(settings) * (front ? frontShare : 1.0f - frontShare);
    return static_cast<float>((body + SeparateHubMass(settings, front ? 0 : 2)) * 9.81);
}

// A wheel's brush tyre from its tyre's figures: the peak grip along and across the wheel at the load
// it carries standing still (a road tyre's 1.1 without data; one alone stands for both), the slip angle of
// the lateral peak (7 degrees without) and the slip ratio of the longitudinal one, the share of grip left
// well past the peak, the wheel's size and its tyre rate.
tyre::BrushTyreParameters BuildBrushTyreParameters(const VehicleSettings& settings, size_t index)
{
    const bool front = index < 2;
    const VehicleTyreSettings& tyres = settings.tyres[index];
    const VehicleWheelGeometry mount = GetVehicleWheelMount(settings, index);
    const double lateralGrip = tyres.lateralGrip > 0.0f ? tyres.lateralGrip : (tyres.longitudinalGrip > 0.0f ? tyres.longitudinalGrip : 1.1);
    const double longitudinalGrip = tyres.longitudinalGrip > 0.0f ? tyres.longitudinalGrip : lateralGrip;
    tyre::BrushTyreFigures figures;
    figures.peakFriction = lateralGrip;
    figures.longitudinalPeakFriction = longitudinalGrip;
    figures.referenceLoad = StaticWheelLoad(settings, front);
    figures.peakSlipAngle = (tyres.peakSlipAngleDegrees > 0.0f ? tyres.peakSlipAngleDegrees : 7.0f) * std::numbers::pi / 180.0;
    figures.peakSlipRatio = tyres.peakSlipRatio;
    figures.kineticShare = tyres.postPeakShare > 0.0f ? tyres.postPeakShare : 0.85f;
    figures.radius = std::max(mount.radius, 0.05f);
    figures.sectionWidth = std::max(mount.width, 0.05f);
    figures.rimRadius = tyres.rimRadius;
    figures.verticalRate = TyreVerticalRate(settings, index);
    figures.inflationPressure = tyres.inflationPressure;
    figures.relaxationLength = tyres.relaxationLength;
    figures.longitudinalStiffnessRatio = tyres.longitudinalStiffnessRatio;
    tyre::BrushTyreParameters parameters = tyre::MakeBrushTyreParameters(figures);
    // The tread squashes no further than the rim's contact, where the suspension's rim spring takes over.
    parameters.rimDeflection = RimDeflection(settings, index);
    // The data's load sensitivity; the bristles are fitted at the static load, where it changes nothing.
    if (tyres.longitudinalLoadExponent > 0.0f)
    {
        parameters.loadExponent[0] = tyres.longitudinalLoadExponent;
    }
    if (tyres.lateralLoadExponent > 0.0f)
    {
        parameters.loadExponent[1] = tyres.lateralLoadExponent;
    }
    // A tyre with the game's own data: its ROLLING_RESISTANCE_0 (per mille) as the rolling resistance, and its
    // camber thrust from CAMBER_GAIN alone (ComputeTyreStepTerms), not also from the tread's turn slip.
    if (tyres.rollingResistance0 > 0.0f)
    {
        parameters.rollingResistance = 0.001 * tyres.rollingResistance0;
    }
    if (tyres.camberGain != 0.0f)
    {
        parameters.camberSpinShare = 0.0;
    }
    if (settings.brushTyreRibs > 0)
    {
        parameters.ribs = std::clamp(settings.brushTyreRibs, 1, tyre::kBrushMaxRibs);
    }
    if (settings.brushTyreSegments > 0)
    {
        parameters.segmentsPerRib = std::clamp(settings.brushTyreSegments, 2, tyre::kBrushMaxSegments);
    }
    return parameters;
}

JPH::Ref<JPH::VehicleConstraintSettings> BuildVehicleConstraintSettings(const VehicleSettings& settings)
{
    JPH::Ref<JPH::VehicleConstraintSettings> vehicle = new JPH::VehicleConstraintSettings();
    vehicle->mUp = JPH::Vec3::sAxisY();
    vehicle->mForward = JPH::Vec3::sAxisZ();
    vehicle->mMaxPitchRollAngle = JPH::DegreesToRadians(std::clamp(settings.maxPitchRollDegrees, 0.0f, 180.0f));

    // Front left, front right, rear left, rear right; +X is the car's left.
    const bool multibody = HasSuspensionGeometry(settings);
    for (size_t index = 0; index < kVehicleWheelCount; ++index)
    {
        const bool front = index < 2;
        const VehicleWheelGeometry mount = GetVehicleWheelMount(settings, index);
        JPH::WheelSettingsWV* wheel = new JPH::WheelSettingsWV();
        wheel->mPosition = ToJolt(mount.center);
        wheel->mSuspensionDirection = JPH::Vec3(0.0f, -1.0f, 0.0f);
        wheel->mSteeringAxis = JPH::Vec3::sAxisY();
        wheel->mWheelUp = JPH::Vec3::sAxisY();
        wheel->mWheelForward = JPH::Vec3::sAxisZ();
        wheel->mSuspensionMinLength = std::max(settings.suspensionMinLength, 0.0f);
        wheel->mSuspensionMaxLength = std::max(settings.suspensionMaxLength, wheel->mSuspensionMinLength);
        // A fixed stiffness for a quarter of the car's weight. The frequency mode would scale the
        // spring by the body's effective mass at the wheel, which its inertia makes 10 to 30% off a
        // quarter, and the car would not settle at the ride height ComputeRestSuspensionLength gives.
        const float quarterMass = std::max(settings.massKg, 1.0f) * 0.25f;
        const float omega = 2.0f * std::numbers::pi_v<float> * std::max(settings.suspensionFrequencyHz, 0.01f);
        wheel->mSuspensionSpring.mMode = JPH::ESpringMode::StiffnessAndDamping;
        wheel->mSuspensionSpring.mStiffness = quarterMass * omega * omega;
        wheel->mSuspensionSpring.mDamping = 2.0f * std::max(settings.suspensionDamping, 0.0f) * quarterMass * omega;
        // The physics engine's wheels lose 0.2 of their spin each second, a drag of 0.2 I w at the
        // axle: on the R34 some 330 N at 100 km/h, as much as its air drag, and rising with the speed.
        // The tyres have their own rolling resistance.
        wheel->mAngularDamping = 0.0f;
        wheel->mRadius = std::max(mount.radius, 0.01f);
        wheel->mWidth = std::max(mount.width, 0.01f);
        wheel->mMaxSteerAngle = front ? JPH::DegreesToRadians(std::clamp(settings.maxSteerAngleDegrees, 0.0f, 89.0f)) : 0.0f;
        // The axles share the four wheels' total by the front's share, so an even 0.5 leaves each
        // wheel at the brake torque.
        const float axleShare = std::clamp(front ? settings.frontBrakeShare : 1.0f - settings.frontBrakeShare, 0.0f, 1.0f);
        ApplyTyres(*wheel, settings.tyres[index]);
        wheel->mMaxBrakeTorque = ComputeBrakeTorquePerWheel(settings) * 2.0f * axleShare;
        wheel->mMaxHandBrakeTorque = front ? 0.0f : std::max(settings.maxHandBrakeTorque, 0.0f);
        if (multibody)
        {
            // The multibody suspension sets the spring each step and steers through the rack: the
            // straight spring starts at its design length, and the wheels' own steering is off.
            const VehicleSuspensionAxle& axle = front ? settings.frontSuspension : settings.rearSuspension;
            const float bump = MultibodyBumpTravel(axle);
            const float droop = axle.reboundStopTravel > 0.0f ? axle.reboundStopTravel : 0.08f;
            const float designLength = MultibodyDesignLength(settings);
            const glm::vec3 center = MultibodyDesignCenter(settings, index);
            wheel->mPosition = ToJolt(center + glm::vec3(0.0f, designLength, 0.0f));
            wheel->mSuspensionMinLength = designLength - bump;
            wheel->mSuspensionMaxLength = designLength + droop;
            wheel->mMaxSteerAngle = 0.0f;
            if (HasUnsprungMass(axle))
            {
                // The hub's end stops and the rim's are its own springs (StepUnsprungCorner); the physics
                // engine's rigid stop lies past them, for what they cannot hold.
                wheel->mSuspensionMinLength = std::max(designLength - bump - TyreDeflectionRoom(settings, index), 0.0f);
                wheel->mSuspensionSpring.mStiffness = kTyreCarrierStiffness;
                wheel->mSuspensionSpring.mDamping = 0.0f;
            }
        }
        vehicle->mWheels.push_back(wheel);
    }

    if (settings.antiRollBars && !multibody)
    {
        vehicle->mAntiRollBars.resize(2);
        vehicle->mAntiRollBars[0].mLeftWheel = 0;
        vehicle->mAntiRollBars[0].mRightWheel = 1;
        vehicle->mAntiRollBars[1].mLeftWheel = 2;
        vehicle->mAntiRollBars[1].mRightWheel = 3;
    }

    JPH::WheeledVehicleControllerSettings* controller = new JPH::WheeledVehicleControllerSettings();
    controller->mEngine.mMaxTorque = std::max(settings.maxEngineTorque, 0.0f);
    controller->mEngine.mMinRPM = std::max(settings.minRpm, 1.0f);
    controller->mEngine.mMaxRPM = std::max(settings.maxRpm, controller->mEngine.mMinRPM + 1.0f);
    if (settings.engineInertia > 0.0f)
    {
        controller->mEngine.mInertia = settings.engineInertia;
    }
    if (settings.engineCoastTorque > 0.0f)
    {
        // The data's engine braking (ApplyEngineCoast) instead of the physics engine's drag.
        controller->mEngine.mAngularDamping = 0.0f;
    }
    if (settings.torqueCurve.size() >= 2 && settings.maxEngineTorque > 0.0f)
    {
        // The physics reads the curve at the engine's rpm over its maximum.
        controller->mEngine.mNormalizedTorque.Clear();
        controller->mEngine.mNormalizedTorque.Reserve(static_cast<JPH::uint>(settings.torqueCurve.size()));
        for (const glm::vec2& point : settings.torqueCurve)
        {
            controller->mEngine.mNormalizedTorque.AddPoint(
                point.x / controller->mEngine.mMaxRPM, std::clamp(point.y / settings.maxEngineTorque, 0.0f, 1.0f));
        }
        controller->mEngine.mNormalizedTorque.Sort();
    }

    if (!settings.gearRatios.empty())
    {
        controller->mTransmission.mGearRatios.assign(settings.gearRatios.begin(), settings.gearRatios.end());
        if (settings.reverseGearRatio != 0.0f)
        {
            controller->mTransmission.mReverseGearRatios = {settings.reverseGearRatio};
        }
    }
    if (settings.clutchStrength > 0.0f)
    {
        controller->mTransmission.mClutchStrength = settings.clutchStrength;
    }
    if (settings.gearSwitchSeconds > 0.0f)
    {
        controller->mTransmission.mSwitchTime = settings.gearSwitchSeconds;
    }
    if (settings.clutchReleaseSeconds >= 0.0f)
    {
        controller->mTransmission.mClutchReleaseTime = settings.clutchReleaseSeconds;
    }
    // The vehicle's own automatic gearbox picks the gear and works the clutch (UpdateAutomaticGearbox):
    // the physics engine's shifts by the engine's revs, which flare while the clutch slips and never
    // fall below the idle. Its shift points only draw the rev counter.
    const VehicleShiftPoints shiftPoints = ComputeVehicleShiftPoints(settings);
    controller->mTransmission.mMode = JPH::ETransmissionMode::Manual;
    controller->mTransmission.mShiftUpRPM = shiftPoints.upFull;
    controller->mTransmission.mShiftDownRPM = shiftPoints.downClosed;

    const float limitedSlipRatio = settings.limitedSlipDifferentials ? 1.4f : FLT_MAX;
    auto addDifferential = [&](int leftWheel, int rightWheel, float torqueRatio)
    {
        JPH::VehicleDifferentialSettings differential;
        differential.mLeftWheel = leftWheel;
        differential.mRightWheel = rightWheel;
        differential.mEngineTorqueRatio = torqueRatio;
        // A limited-slip differential is a clutch pack that the vehicle applies each step
        // (CoupleDifferentialWheels); the physics engine's own would snap all the torque to one wheel.
        differential.mLimitedSlipRatio = FLT_MAX;
        if (settings.finalDriveRatio > 0.0f)
        {
            differential.mDifferentialRatio = settings.finalDriveRatio;
        }
        controller->mDifferentials.push_back(differential);
    };
    switch (settings.drive)
    {
    case VehicleDrive::FrontWheel:
        addDifferential(0, 1, 1.0f);
        break;
    case VehicleDrive::AllWheel:
        if (settings.centreDrive == VehicleCentreDrive::Coupling)
        {
            // The engine drives the rear; the front gets what the centre coupling passes (ApplyCentreCoupling).
            addDifferential(2, 3, 1.0f);
        }
        else
        {
            const float front = std::clamp(settings.frontTorqueShare, 0.0f, 1.0f);
            if (front > 0.0f)
            {
                addDifferential(0, 1, front);
            }
            if (front < 1.0f)
            {
                addDifferential(2, 3, 1.0f - front);
            }
        }
        break;
    case VehicleDrive::RearWheel:
    default:
        addDifferential(2, 3, 1.0f);
        break;
    }
    controller->mDifferentialLimitedSlipRatio = limitedSlipRatio;
    vehicle->mController = controller;
    return vehicle;
}

bool HasInertiaBox(const VehicleSettings& settings)
{
    return settings.inertiaBox.x > 0.0f && settings.inertiaBox.y > 0.0f && settings.inertiaBox.z > 0.0f;
}

// The rigid body the physics engine moves: the car's body with the hubs that ride on their own tyre springs
// fixed at their wheels' rest centres (their motion along the travel reaches it as a force,
// StepUnsprungCorner). Its centre of mass (vehicle space, in outCenter) is theirs together, and its inertia
// the body's (the data's box of the body's mass, else the shape's) with the hubs' as point masses.
JPH::MassProperties VehicleBodyMassProperties(const VehicleSettings& settings, const JPH::Shape& chassis, glm::vec3& outCenter)
{
    const glm::vec3 body = settings.chassisCenter + settings.centerOfMassOffset;
    const double sprung = SprungMass(settings);
    double mass = sprung;
    glm::dvec3 moment = glm::dvec3(body) * sprung;
    for (size_t index = 0; index < kVehicleWheelCount; ++index)
    {
        const double hub = SeparateHubMass(settings, index);
        mass += hub;
        moment += glm::dvec3(MultibodyRestCenter(settings, index)) * hub;
    }
    outCenter = glm::vec3(moment / mass);

    JPH::MassProperties properties;
    if (HasInertiaBox(settings))
    {
        properties.SetMassAndInertiaOfSolidBox(ToJolt(settings.inertiaBox), 1.0f);
    }
    else
    {
        properties = chassis.GetMassProperties();
    }
    properties.ScaleToMass(static_cast<float>(sprung));
    properties.Translate(ToJolt(body - outCenter));
    for (size_t index = 0; index < kVehicleWheelCount; ++index)
    {
        const float hub = static_cast<float>(SeparateHubMass(settings, index));
        const JPH::Vec3 arm = ToJolt(MultibodyRestCenter(settings, index) - outCenter);
        properties.mInertia += hub * (JPH::Mat44::sScale(arm.Dot(arm)) - JPH::Mat44::sOuterProduct(arm, arm));
    }
    properties.mInertia.SetColumn4(3, JPH::Vec4(0, 0, 0, 1));
    properties.mMass = static_cast<float>(mass);
    return properties;
}
}

namespace
{
// A car in water: buoyant at first (air in the cabin and the sills holds it up with about 70% of its
// shape under), less so as it fills, until it sinks. It fills over some seconds, faster the deeper it
// sits, and the engine drowns once the water covers more than half of it.
constexpr float kAfloatBuoyancy = 1.4f;
constexpr float kSunkBuoyancy = 0.35f;
constexpr float kFloodSeconds = 10.0f;
constexpr float kEngineDrownShare = 0.5f;
constexpr float kWaterLinearDrag = 1.5f;
constexpr float kWaterAngularDrag = 0.2f;
// Tunnels run under the games' water (III's Porter Tunnel, 20 m under the harbour; the water covers whole
// 32 m blocks over them). As the games do, water more than this far over a car that is not already in it
// is not its water: the car is in a tunnel, and stays there until it is out from under the water or
// above its surface.
constexpr float kTunnelDepth = 3.0f;

float FloodedShare(float floodSeconds)
{
    return 1.0f - std::exp(-floodSeconds / kFloodSeconds);
}
}

struct PhysicsWorld::Impl
{
    GroundEdgeContactFilter groundEdgeFilter;
    struct Vehicle
    {
        JPH::Body* body = nullptr;
        JPH::Ref<JPH::VehicleConstraint> constraint;
        JPH::Ref<JPH::VehicleCollisionTester> collisionTester;
        VehicleCollisionTesterDisc* discTester = nullptr; // the same, owned by collisionTester
        std::unique_ptr<WheelBodyFilter> wheelFilter;
        // The constraint's state (the wheels' spin, the engine, the gearbox, its solver's impulses) and the
        // wheels' settings (which the suspension and brakes change each step) as the car was made:
        // ResetVehicle puts them back, so a reset car drives as a new one.
        std::string madeConstraintState;
        std::vector<JPH::WheelSettingsWV> madeWheelSettings;
        VehicleControls controls;
        // In water (ApplyWater): the share of the body's shape under the surface, the seconds it has
        // spent filling (weighted by that share), and whether the engine has drowned.
        float submergedShare = 0.0f;
        float floodSeconds = 0.0f;
        bool engineDrowned = false;
        bool underWaterInTunnel = false; // see kTunnelDepth
        // The air acting on the car: where, and how much drag and downforce per square metre of dynamic pressure.
        std::vector<VehicleAeroSurface> aeroSurfaces;
        float direction = 1.0f; // the gearbox's drive or reverse, see ResolveVehicleDriverInput
        // The automatic gearbox, which selects the physics engine's gear and clutch each step (see
        // UpdateAutomaticGearbox), and the gearbox output's rpm per m/s of the car's speed.
        VehicleGearbox gearbox;
        VehicleGearboxState gearboxState;
        float outputRpmPerSpeed = 0.0f;
        // The driver's gear changes asked for (VehicleControls::gearShifts) and not yet made.
        int pendingGearShifts = 0;
        // The brakes: the wheels' settings whose torque is set each step, the torque of each wheel as the
        // fixed front/rear split has it, and (when dynamicBrakeBias) the loads that share the total.
        std::vector<JPH::WheelSettingsWV*> wheelSettings;
        std::array<float, kVehicleWheelCount> staticBrakeTorque{};
        // The anti-lock brakes (ApplyAntiLock): which wheels' brakes it has let off, and the time since it
        // last looked.
        std::array<bool, kVehicleWheelCount> absReleased{};
        float absClock = 0.0f;
        // The game's traction control (ApplyTractionControl): whether it has the throttle cut, and the time
        // since it last looked.
        bool tcCut = false;
        float tcClock = 0.0f;
        // Each turbo's boost now (SpoolTurbos), and the throttle pedal the engine had in the last step.
        std::vector<float> turboBoost;
        float pedal = 0.0f;
        std::array<float, kVehicleWheelCount> filteredLoad{};
        bool dynamicBrakeBias = false;
        bool filteredLoadValid = false;
        // Traction control, see LimitClutchTorque.
        float tractionControlGrip = 0.0f;
        VehicleDrive drive = VehicleDrive::RearWheel;
        // The share of the drive torque a limited-slip differential can move between the wheels, see
        // CoupleDifferentialWheels; 0 for an open one.
        float limitedSlipLock = 0.0f;
        // A four-wheel drive's centre coupling: the torque it passed in the last step (Nm at the transfer case,
        // rear to front positive), see ApplyCentreCoupling.
        float centreCouplingTorque = 0.0f;
        // The power the drivetrain lost in the last step (W): in the gears' meshes (ApplyDrivetrainLoss and the
        // coupling's transfer case) and to churning and bearings (ApplyDrivetrainSpin).
        float drivetrainMeshLoss = 0.0f;
        float drivetrainSpinLoss = 0.0f;
        // Rear-wheel steering (settings.rearSteerControllers): each controller's filtered value, the rear
        // wheels' angle (radians, with the front positive), the rear wheels' forward axes before it, and the
        // body's velocity a step ago for the lateral acceleration it reads. See ApplyRearSteer.
        std::vector<float> rearSteerFiltered;
        float rearSteerAngle = 0.0f;
        std::array<JPH::Vec3, 2> rearWheelForward{};
        JPH::Vec3 controllerLastVelocity = JPH::Vec3::sZero();
        bool controllerLastVelocityValid = false;
        // The clutch's own strength, which traction control lowers while it slips it.
        float defaultClutchStrength = 10.0f;
        float weightNewtons = 0.0f;
        VehicleSnapshot previous;
        VehicleSnapshot current;

        // The multibody suspension, when the settings have one: a corner per wheel.
        struct Corner
        {
            glm::vec3 designCenter{0.0f}; // vehicle space
            float designLength = 0.0f;    // the straight spring's length at the design position
            double travel = 0.0;
            double antiRollBarRate = 0.0;
            float camberDegrees = 0.0f;
            float toeDegrees = 0.0f;
            float springForce = 0.0f;
            float antiRollBarForce = 0.0f;
            bool front = true;

            // With an unsprung mass the wheel's travel is a state of its own, and the physics engine's
            // spring is the tyre between the ground and the hub.
            bool unsprung = false;
            double hubMass = 0.0;
            double tyreRate = 0.0;
            double tyreDamping = 0.0;
            double travelRate = 0.0;
            double bumpTravel = 0.0;
            double droopTravel = 0.0;
            // The stiff springs past the travel's ends and past the rim's contact (kEndStopTyreRates):
            // N/m and N s/m at the wheel, and the tyre's deflection where the rim meets the ground (m).
            double endStopRate = 0.0;
            double endStopDamping = 0.0;
            double rimDeflection = 0.0;
            double designContactHeight = 0.0; // the unloaded tyre's lowest point at the design position
            double tyreDeflection = 0.0;
            glm::vec3 hubCenter{0.0f}; // vehicle space
            // Where the mount was when the physics engine last looked for the ground.
            JPH::RVec3 lastMount = JPH::RVec3::sZero();
            bool lastMountValid = false;
        };
        std::vector<Corner> corners;
        // Front and rear: each axle's two wheels (independent corners or a solid axle), stepped together.
        std::array<std::unique_ptr<suspension::AxleSuspension>, 2> axles;
        // A solid axle's propshaft torque reaction on its housing in the last step (N m, about the
        // car's length, positive lifting its left side), see ApplyAxleTorqueReaction.
        std::array<float, 2> axleTorqueReaction{};
        double rackAtLock = 0.0;
        // The body's velocities a step ago, for the acceleration the hubs ride on.
        JPH::Vec3 lastLinearVelocity = JPH::Vec3::sZero();
        JPH::Vec3 lastAngularVelocity = JPH::Vec3::sZero();
        bool lastVelocityValid = false;
        VehicleSettings settings;

        // The brush tyres (VehicleTyreModel::Brush), one per wheel, stepped inside the physics engine's
        // step once it has found the ground (ApplyBrushTyres), and what each did in the last step: its
        // output, the force it put on the body (world space) and the load it worked with.
        std::vector<tyre::BrushTyre> brushTyres;
        // Their temperatures and pressure (VehicleSettings::tyreTemperatures), for the wheels whose tyre has the
        // game's thermal data; stepped after the tyres, so a step's grip and pressure are the last step's.
        std::array<std::optional<tyre::TyreThermalModel>, kVehicleWheelCount> thermals{};
        // Their wear (VehicleSettings::tyreWear), for the wheels whose tyre has a WEAR_CURVE.
        std::array<std::optional<tyre::TyreWearModel>, kVehicleWheelCount> wears{};
        // The slip over its peak each tyre had in the last step (VehicleTyreStepTerms::slip).
        std::array<float, kVehicleWheelCount> tyreSlips{};
        struct BrushWheel
        {
            tyre::BrushTyreOutput out;
            JPH::Vec3 force = JPH::Vec3::sZero();
            float load = 0.0f;
            bool contact = false;
        };
        std::array<BrushWheel, kVehicleWheelCount> brushWheels{};
        // The turns the physics engine left out (ApplyDroppedRotation), rad about world axes.
        std::array<double, 3> droppedRotation{};
        // Each wheel's roll angle before the step (RollWheelsBySpinAfterStep), rad.
        std::array<float, kVehicleWheelCount> spinAngleBefore{};
    };

    void BuildCorners(Vehicle& vehicle) const
    {
        vehicle.corners.clear();
        const VehicleSettings& settings = vehicle.settings;
        if (!HasSuspensionGeometry(settings))
        {
            return;
        }
        std::array<VehicleCornerSetup, kVehicleWheelCount> setups{};
        for (size_t index = 0; index < kVehicleWheelCount; ++index)
        {
            const bool front = index < 2;
            const VehicleSuspensionAxle& axle = front ? settings.frontSuspension : settings.rearSuspension;
            const bool unsprung = HasUnsprungMass(axle);
            // Each wheel's static load from where the centre of mass sits between the axles; the spring
            // holds the body, the hub's own weight goes straight to the tyre.
            const double load = MultibodySpringLoad(settings, index);
            setups[index] = BuildVehicleCorner(settings, index, load);
            const VehicleCornerSetup& setup = setups[index];
            Vehicle::Corner corner;
            corner.designCenter = MultibodyDesignCenter(settings, index);
            // The wheel starts where the car rests (the design position, or with a rod length its rest travel).
            corner.travel = VehicleRestTravel(axle, load);
            corner.designLength = MultibodyDesignLength(settings);
            corner.antiRollBarRate = setup.antiRollBarRate;
            corner.front = front;
            corner.unsprung = unsprung;
            corner.hubMass = axle.hubMass;
            corner.tyreRate = TyreVerticalRate(settings, index);
            corner.tyreDamping = TyreVerticalDamping(settings, index);
            corner.bumpTravel = setup.bumpTravel;
            corner.droopTravel = setup.droopTravel;
            corner.endStopRate = kEndStopTyreRates * corner.tyreRate;
            corner.endStopDamping = 2.0 * kEndStopDampingRatio * std::sqrt(corner.endStopRate * load / 9.81);
            corner.rimDeflection = RimDeflection(settings, index);
            corner.hubCenter = MultibodyRestCenter(settings, index);
            if (axle.type == VehicleSuspensionType::SolidAxle)
            {
                corner.designContactHeight = -std::max(GetVehicleWheelMount(settings, index).radius, 0.05f);
            }
            else
            {
                suspension::Kinematics kinematics(suspension::Compile(setup.definition));
                suspension::KinematicOutputs design;
                suspension::ComputeOutputs(kinematics, design);
                corner.designContactHeight = design.contactPoint.z;
            }
            vehicle.corners.push_back(std::move(corner));
        }
        for (size_t axle = 0; axle < 2; ++axle)
        {
            const VehicleSuspensionAxle& data = axle == 0 ? settings.frontSuspension : settings.rearSuspension;
            const VehicleCornerSetup& left = setups[2 * axle];
            const VehicleCornerSetup& right = setups[2 * axle + 1];
            if (data.type == VehicleSuspensionType::SolidAxle)
            {
                const double radius = std::max(GetVehicleWheelMount(settings, 2 * axle).radius, 0.05f);
                vehicle.axles[axle] = std::make_unique<suspension::AxleSuspension>(
                    std::make_unique<suspension::SolidAxle>(BuildSolidAxle(data, radius), MakeVehicleCornerUnit(left), MakeVehicleCornerUnit(right)));
            }
            else
            {
                vehicle.axles[axle] = std::make_unique<suspension::AxleSuspension>(
                    std::make_unique<suspension::SuspensionCorner>(left.definition, MakeVehicleCornerUnit(left), suspension::SuspensionCorner::kWheelTravel),
                    std::make_unique<suspension::SuspensionCorner>(right.definition, MakeVehicleCornerUnit(right), suspension::SuspensionCorner::kWheelTravel));
            }
        }
        vehicle.rackAtLock = FitSteeringRackTravel(settings);
    }

    // Before each step: the multibody suspension reads where the physics engine left each wheel, moves
    // the wheel along its linkage (the mount's horizontal place, camber, toe and steering), and hands
    // the physics engine's straight spring the stiffness, damping and preload that make its force, near
    // the current state, the suspension's: springs, damper, stops and anti-roll bar at the wheel, plus
    // the tyre's horizontal forces through the linkage's geometry (anti-dive, anti-squat, jacking).
    //
    // A wheel with an unsprung mass (StepUnsprungCorner) instead moves on its own travel state, and the
    // physics engine's spring carries the tyre's vertical force between the ground and the hub.
    void UpdateCorners(Vehicle& vehicle, float steering)
    {
        if (vehicle.corners.empty())
        {
            return;
        }
        if (!vehicle.body->IsActive())
        {
            // Asleep, the physics engine does not step the car: nothing moves.
            vehicle.lastVelocityValid = false;
            return;
        }
        const double dt = stepSeconds;
        const JPH::Array<JPH::Wheel*>& wheels = vehicle.constraint->GetWheels();
        const JPH::Quat toBody = vehicle.body->GetRotation().Conjugated();
        const double rack = vehicle.rackAtLock * std::clamp(static_cast<double>(steering), -1.0, 1.0);
        const auto* controller = static_cast<const JPH::WheeledVehicleController*>(vehicle.constraint->GetController());

        std::array<double, kVehicleWheelCount> travel{};
        for (size_t index = 0; index < vehicle.corners.size(); ++index)
        {
            const Vehicle::Corner& c = vehicle.corners[index];
            travel[index] = c.unsprung ? c.travel : c.designLength - wheels[static_cast<JPH::uint>(index)]->GetSuspensionLength();
        }
        std::array<suspension::CornerInput, kVehicleWheelCount> inputs{};
        std::array<float, kVehicleWheelCount> cosines{};
        for (size_t index = 0; index < vehicle.corners.size(); ++index)
        {
            const Vehicle::Corner& c = vehicle.corners[index];
            const JPH::Wheel& wheel = *wheels[static_cast<JPH::uint>(index)];
            suspension::CornerInput& in = inputs[index];
            in.dt = dt;
            in.travel = travel[index];
            in.travelRate = c.unsprung ? c.travelRate : (travel[index] - c.travel) / dt;
            in.rack = c.front ? rack : 0.0;
            float cosine = 1.0f;
            if (wheel.HasContact())
            {
                // The road's horizontal push on the tyre over the last step, in the corner's frame.
                const JPH::Vec3 longitudinal = toBody * wheel.GetContactLongitudinal();
                const JPH::Vec3 lateral = toBody * wheel.GetContactLateral();
                const JPH::Vec3 normal = toBody * wheel.GetContactNormal();
                JPH::Vec3 force = longitudinal * (wheel.GetLongitudinalLambda() / static_cast<float>(dt)) + lateral * (wheel.GetLateralLambda() / static_cast<float>(dt));
                if (!vehicle.brushTyres.empty())
                {
                    force = toBody * vehicle.brushWheels[index].force;
                }
                in.load.force = VehicleToCorner(FromJolt(force));
                in.contactNormal = VehicleToCorner(FromJolt(normal));
                cosine = std::max(0.1f, normal.GetY());
                in.load.moment = KnuckleSpinMoment(vehicle, *controller, index, in.load.force);
            }
            cosines[index] = cosine;
        }
        ApplyAxleTorqueReaction(vehicle);
        for (size_t axle = 0; axle < 2; ++axle)
        {
            vehicle.axles[axle]->Step({inputs[2 * axle], inputs[2 * axle + 1]});
        }
        for (size_t index = 0; index < vehicle.corners.size(); ++index)
        {
            Vehicle::Corner& c = vehicle.corners[index];
            const JPH::Wheel& wheel = *wheels[static_cast<JPH::uint>(index)];
            JPH::WheelSettingsWV& settings = *vehicle.wheelSettings[index];
            const suspension::CornerInput& in = inputs[index];
            const float cosine = cosines[index];
            const suspension::CornerOutput& out = vehicle.axles[index / 2]->Output(static_cast<int>(index % 2));

            // The anti-roll bar from the two wheels' travel difference.
            const size_t other = index ^ 1u;
            const double arb = -c.antiRollBarRate * (travel[index] - travel[other]);

            if (c.unsprung)
            {
                StepUnsprungCorner(vehicle, index, wheel, settings, out, in, arb, cosine);
                if (std::abs(c.travelRate) > kHubSettledRate)
                {
                    // The physics engine judges sleep by the body alone: not while a hub still moves.
                    vehicle.body->ResetSleepTimer();
                }
            }
            else
            {
                c.travel = travel[index];
                // The road's normal force that balances the travel: F_n * dPn/dz + G = 0.
                const double perTravel = std::max(out.normalPerTravel, 0.2);
                const double generalised = out.strutTravelForce + arb + out.loadTravelForce;
                const double normalForce = -generalised / perTravel;
                const double stiffness = std::max(-(out.strutTravelStiffness - c.antiRollBarRate) / perTravel, 1000.0);
                const double damping = std::max(-out.strutTravelDamping / perTravel, 0.0);
                // The physics engine's spring is k (Lmax + preload - L) - c dL/dt along the normal (its k
                // and c divided by the cosine between the suspension and the normal): match force and slopes.
                settings.mSuspensionSpring.mMode = JPH::ESpringMode::StiffnessAndDamping;
                settings.mSuspensionSpring.mStiffness = static_cast<float>(stiffness) * cosine;
                settings.mSuspensionSpring.mDamping = static_cast<float>(damping) * cosine;
                settings.mSuspensionPreloadLength = static_cast<float>((normalForce - stiffness * in.travel - damping * in.travelRate) / stiffness) - settings.mSuspensionMaxLength + c.designLength;
            }

            // The wheel where the linkage has it: the mount above its centre, turned by camber and toe.
            const glm::vec3 center = c.designCenter + CornerToVehicle(out.geometry.wheelCenter);
            settings.mPosition = JPH::Vec3(center.x, c.designCenter.y + c.designLength, center.z);
            const suspension::Vec3 axis = out.geometry.spinAxis;
            const suspension::Vec3 leftward = axis * (index % 2 == 0 ? 1.0 : -1.0);
            const suspension::Vec3 forward = glm::normalize(glm::cross(leftward, suspension::Vec3(0.0, 0.0, 1.0)));
            const suspension::Vec3 up = glm::cross(forward, leftward);
            settings.mWheelForward = ToJolt(CornerToVehicle(forward));
            settings.mWheelUp = ToJolt(CornerToVehicle(up));
            if (c.unsprung)
            {
                // Where the physics engine will look for the ground from in the coming step.
                c.lastMount = vehicle.body->GetWorldTransform() * settings.mPosition;
                c.lastMountValid = true;
                // The hub drawn where this step left it.
                c.hubCenter = center + CornerToVehicle(out.wheelCenterPerTravel) * static_cast<float>(c.travel - in.travel);
            }

            c.camberDegrees = static_cast<float>(out.geometry.camber * 180.0 / std::numbers::pi);
            c.toeDegrees = static_cast<float>(out.geometry.toe * 180.0 / std::numbers::pi);
            c.springForce = static_cast<float>(out.strutForce);
            c.antiRollBarForce = static_cast<float>(-arb);
        }
        vehicle.lastLinearVelocity = vehicle.body->GetLinearVelocity();
        vehicle.lastAngularVelocity = vehicle.body->GetAngularVelocity();
        vehicle.lastVelocityValid = true;
    }

    // What a wheel's knuckle does not take of its road force's moment about the spin axis
    // (KnuckleSpinMomentRelief): an independent wheel's brakes hold up to their torque, a solid axle
    // carries its differential in its housing and takes it all. The linkage's pose is the last step's,
    // as the force is.
    suspension::Vec3 KnuckleSpinMoment(const Vehicle& vehicle, const JPH::WheeledVehicleController& controller, size_t index, const suspension::Vec3& force) const
    {
        const suspension::AxleSuspension& axle = *vehicle.axles[index / 2];
        if (axle.Solid() != nullptr)
        {
            return suspension::Vec3(0.0);
        }
        const JPH::WheelSettingsWV& settings = *vehicle.wheelSettings[index];
        const double brakes = controller.GetBrakeInput() * settings.mMaxBrakeTorque + controller.GetHandBrakeInput() * settings.mMaxHandBrakeTorque;
        return KnuckleSpinMomentRelief(axle.Output(static_cast<int>(index % 2)).geometry, force, brakes);
    }

    // A driven solid axle's propshaft torque reaction (Assetto Corsa's TORQUE_REACTION, a signed share
    // of the propshaft's torque taken as a moment about the car's length): on the axle housing, and the
    // opposite on the body. The propshaft's torque is the engine's at the throttle, through the clutch's
    // grip and the gear, times the axle's differential's share of it.
    void ApplyAxleTorqueReaction(Vehicle& vehicle)
    {
        for (size_t axle = 0; axle < 2; ++axle)
        {
            suspension::SolidAxle* solid = vehicle.axles[axle]->Solid();
            const VehicleSuspensionAxle& data = axle == 0 ? vehicle.settings.frontSuspension : vehicle.settings.rearSuspension;
            if (solid == nullptr || data.axleTorqueReaction == 0.0f)
            {
                continue;
            }
            const auto* controller = static_cast<const JPH::WheeledVehicleController*>(vehicle.constraint->GetController());
            double share = 0.0;
            for (const JPH::VehicleDifferentialSettings& differential : controller->GetDifferentials())
            {
                if (differential.mLeftWheel == static_cast<int>(2 * axle) || differential.mRightWheel == static_cast<int>(2 * axle))
                {
                    share += differential.mEngineTorqueRatio;
                }
            }
            const JPH::VehicleTransmission& transmission = controller->GetTransmission();
            const double propshaft = controller->GetEngine().GetTorque(std::max(controller->GetForwardInput(), 0.0f)) * transmission.GetClutchFriction() *
                                     transmission.GetCurrentRatio() * share * StepDrivetrainEfficiency(vehicle, *controller);
            const double moment = data.axleTorqueReaction * propshaft;
            solid->SetHousingMoment(suspension::Vec3(moment, 0.0, 0.0));
            // The body takes the other end of it, about its own length.
            const JPH::Vec3 onBody = vehicle.body->GetRotation() * JPH::Vec3(0.0f, 0.0f, static_cast<float>(-moment));
            physicsSystem.GetBodyInterfaceNoLock().AddTorque(vehicle.body->GetID(), onBody, JPH::EActivation::DontActivate);
            vehicle.axleTorqueReaction[axle] = static_cast<float>(moment);
        }
    }

    // One step of a wheel with its own mass. The travel z (bump positive) moves by
    //
    //   m_z z'' = F_t (n.dP/dz) + F_h.dP/dz + G(z, z') + G_arb - m_hub f.dW/dz
    //
    // with m_z = m_hub |dW/dz|^2 (W the wheel centre, P the contact point), F_t the tyre's vertical
    // force (rate and damping on its deflection, pushing only), G the springs, damper and stops, and f
    // the specific force (acceleration less gravity) of the body where the hub rides. It is stepped
    // linearly implicit (the trapezoidal rule on the slopes). Its travel's ends and the rim's contact
    // with the ground are stiff springs, not rigid stops: a rigid stop took the hub's (or, through the
    // physics engine's, the whole car's) speed away within a step, some 100 g on a 0.5 m drop.
    //
    // The physics engine's body is the whole car (hubs included). It receives the tyre's force through
    // its spring, set by the preload to exactly the force the hub took, and the hub's motion relative
    // to the body as -m_hub z'' dW/dz at the hub: together they leave the sprung mass with the
    // suspension's force, and the tyre's friction with the tyre's load.
    void StepUnsprungCorner(Vehicle& vehicle, size_t index, const JPH::Wheel& wheel, JPH::WheelSettingsWV& settings, const suspension::CornerOutput& out,
                            const suspension::CornerInput& in, double arb, float cosine)
    {
        Vehicle::Corner& c = vehicle.corners[index];
        const double dt = stepSeconds;
        const JPH::Body& body = *vehicle.body;
        const JPH::Quat rotation = body.GetRotation();
        const JPH::RMat44 transform = body.GetWorldTransform();
        const double z = in.travel;
        const double v = in.travelRate;
        const double perTravel = std::max(out.normalPerTravel, 0.2);

        // The tyre: its deflection from where the ground is now along the suspension. The physics
        // engine found the ground at the start of the last step; the mount has moved since.
        double tyre = 0.0;
        double tyreSlope = 0.0;
        double tyreRateSlope = 0.0;
        double length = settings.mSuspensionMaxLength;
        c.tyreDeflection = 0.0;
        // Where the unloaded tyre would touch: the design length, less how far its lowest point has
        // risen with the travel. The rim meets the ground its deflection closer: from there on a
        // kerb's face, not its edge, is pushed along (VehicleCollisionTesterDisc).
        const double rise = out.geometry.contactPoint.z - c.designContactHeight;
        vehicle.discTester->SetFaceLength(index, static_cast<float>(std::max(c.designLength - rise - c.rimDeflection, 0.0)));
        if (wheel.HasContact())
        {
            const JPH::Vec3 normal = wheel.GetContactNormal();
            const JPH::Vec3 direction = rotation * settings.mSuspensionDirection;
            const float along = std::min(normal.Dot(direction), -0.1f);
            const JPH::RVec3 mount = transform * settings.mPosition;
            length = wheel.GetSuspensionLength();
            if (c.lastMountValid)
            {
                length -= normal.Dot(JPH::Vec3(mount - c.lastMount)) / along;
            }
            const double groundRate = -normal.Dot(body.GetPointVelocity(mount) - wheel.GetContactPointVelocity()) / along;
            const double deflection = (c.designLength - rise) - length;
            if (deflection > 0.0)
            {
                c.tyreDeflection = deflection;
                const double closing = perTravel * v + groundRate;
                double rate = c.tyreRate;
                double damping = c.tyreDamping;
                double push = rate * deflection - damping * closing;
                if (deflection > c.rimDeflection)
                {
                    // The rim meets the ground through the pinched tyre: a stiff spring of its own.
                    const EndStopPush rim = PushOfEndStop(c.endStopRate, c.endStopDamping, deflection - c.rimDeflection, -closing);
                    push += rim.force;
                    rate += rim.stiffness;
                    damping += rim.damping;
                }
                if (push > 0.0)
                {
                    tyre = push;
                    tyreSlope = -rate * perTravel;
                    tyreRateSlope = -damping * perTravel;
                }
            }
        }

        // The body's specific force where the hub rides, in the vehicle's frame.
        const JPH::Vec3 hub = JPH::Vec3(transform * ToJolt(c.hubCenter) - body.GetCenterOfMassPosition());
        const JPH::Vec3 angular = body.GetAngularVelocity();
        JPH::Vec3 acceleration = angular.Cross(angular.Cross(hub));
        if (vehicle.lastVelocityValid)
        {
            acceleration += (body.GetLinearVelocity() - vehicle.lastLinearVelocity) / static_cast<float>(dt) +
                            ((angular - vehicle.lastAngularVelocity) / static_cast<float>(dt)).Cross(hub);
        }
        const JPH::Vec3 specific = rotation.Conjugated() * (acceleration - physicsSystem.GetGravity());
        const suspension::Vec3 hubPerTravel = out.wheelCenterPerTravel;
        const glm::vec3 hubPerTravelVehicle = CornerToVehicle(hubPerTravel);
        const double inertia = -c.hubMass * glm::dot(FromJolt(specific), hubPerTravelVehicle);
        const double mass = std::max(c.hubMass * glm::dot(hubPerTravel, hubPerTravel), 0.5 * c.hubMass);

        // Past either end of its travel the hub meets a stiff spring (the bump rubber pressed solid, the
        // droop limit), which pushes it back but never holds it there.
        double stop = 0.0;
        double stopSlope = 0.0;
        double stopRateSlope = 0.0;
        if (z > c.bumpTravel || z < -c.droopTravel)
        {
            const bool bump = z > c.bumpTravel;
            const EndStopPush end = bump ? PushOfEndStop(c.endStopRate, c.endStopDamping, z - c.bumpTravel, v)
                                         : PushOfEndStop(c.endStopRate, c.endStopDamping, -c.droopTravel - z, -v);
            stop = bump ? -end.force : end.force;
            stopSlope = -end.stiffness;
            stopRateSlope = -end.damping;
        }

        const double force = tyre * perTravel + out.loadTravelForce + out.strutTravelForce + arb + inertia + stop;
        const double stiffness = out.strutTravelStiffness - c.antiRollBarRate + tyreSlope * perTravel + stopSlope;
        const double damping = out.strutTravelDamping + tyreRateSlope * perTravel + stopRateSlope;
        // The trapezoidal rule on the slopes: second order and without backward Euler's numerical damping
        // (which took some 15 % off a lightly damped wheel hop's peak at the 1 ms step); stable for any step,
        // and at 1 ms the stiffest hub mode (tyre, spring and bump stop together) is far from where its
        // lack of L-stability would show (omega dt about 0.1; some 0.5 with the hub on both end stops).
        const double change = dt * (force + 0.5 * dt * stiffness * v) / (mass - 0.5 * dt * damping - 0.25 * dt * dt * stiffness);
        const double rate = v + change;
        const double next = z + dt * (v + 0.5 * change);
        const double travelAccel = (rate - v) / dt;
        c.travel = next;
        c.travelRate = rate;

        // The tyre's force the hub took, for the physics engine to apply to the body (and to bound the
        // tyre's friction): k (Lmax + preload - L) with L where the ground is now. Its mean over the step
        // the hub was just stepped through, as the trapezoidal step has it, since the physics engine now
        // moves the body through that same step: the force at the step's end would reach the body half a
        // step early (7 % off its response at 1 ms on the rig, 1.2 % with the mean).
        const double applied = std::max(tyre + 0.5 * (tyreSlope * (next - z) + tyreRateSlope * (rate - v)), 0.0);
        settings.mSuspensionSpring.mMode = JPH::ESpringMode::StiffnessAndDamping;
        settings.mSuspensionSpring.mStiffness = kTyreCarrierStiffness * cosine;
        settings.mSuspensionSpring.mDamping = 0.0f;
        settings.mSuspensionPreloadLength = std::max(static_cast<float>(applied / kTyreCarrierStiffness + length) - settings.mSuspensionMaxLength, 0.0f);

        // The hub's motion relative to the body, as a force on the body at the hub.
        const JPH::Vec3 relative = rotation * ToJolt(hubPerTravelVehicle) * static_cast<float>(-c.hubMass * travelAccel);
        physicsSystem.GetBodyInterfaceNoLock().AddForce(body.GetID(), relative, transform * ToJolt(c.hubCenter), JPH::EActivation::DontActivate);
    }

    // The game's traction control, after the gearbox: above its minimum speed, at its rate, it looks at
    // the driven wheels and cuts the throttle while any turns faster than the road by more than the
    // limit, giving it back once all are under. (Assetto Corsa's electronics.ini [TRACTION_CONTROL].)
    void ApplyTractionControl(Vehicle& vehicle, VehicleDriverInput& input, float forwardSpeed) const
    {
        const VehicleSettings& settings = vehicle.settings;
        if (!settings.useTractionControl || settings.tcSlipRatioLimit <= 0.0f || input.forward == 0.0f ||
            std::abs(forwardSpeed) * 3.6f < settings.tcMinSpeedKmh)
        {
            vehicle.tcCut = false;
            vehicle.tcClock = 0.0f;
            return;
        }
        vehicle.tcClock += stepSeconds;
        const float period = settings.tcRateHz > 0.0f ? 1.0f / settings.tcRateHz : 0.0f;
        if (vehicle.tcClock >= period)
        {
            vehicle.tcClock = 0.0f;
            constexpr float kMinRoadSpeed = 1.0f;
            bool spinning = false;
            const JPH::Array<JPH::Wheel*>& wheels = vehicle.constraint->GetWheels();
            for (size_t index = 0; index < wheels.size(); ++index)
            {
                const bool front = index < 2;
                const bool driven = vehicle.drive == VehicleDrive::AllWheel || (vehicle.drive == VehicleDrive::FrontWheel) == front;
                const JPH::Wheel& wheel = *wheels[static_cast<JPH::uint>(index)];
                if (!driven || !wheel.HasContact())
                {
                    continue;
                }
                const float road = (vehicle.body->GetPointVelocity(wheel.GetContactPosition()) - wheel.GetContactPointVelocity()).Dot(wheel.GetContactLongitudinal());
                const float tread = wheel.GetAngularVelocity() * wheel.GetSettings()->mRadius;
                if (std::abs(road) > kMinRoadSpeed && road * tread > 0.0f && (std::abs(tread) - std::abs(road)) / std::abs(road) > settings.tcSlipRatioLimit)
                {
                    spinning = true;
                }
            }
            vehicle.tcCut = spinning;
        }
        if (vehicle.tcCut)
        {
            input.forward = 0.0f;
        }
    }

    // The anti-lock brakes, after DistributeBrakeTorque: at the data's rate the controller looks at each
    // wheel's slip and lets the brake off a wheel that turns slower than the road by more
    // than the limit, on again once it is back under; between looks the wheels keep what it decided. The
    // hand brake is not its business. Under 2 m/s, where a slip ratio says little, it leaves the brakes on.
    void ApplyAntiLock(Vehicle& vehicle, float brake, float forwardSpeed) const
    {
        const VehicleSettings& settings = vehicle.settings;
        if (!settings.useAbs || settings.absSlipRatioLimit <= 0.0f || brake <= 0.0f)
        {
            vehicle.absReleased.fill(false);
            vehicle.absClock = 0.0f;
            return;
        }
        constexpr float kMinSpeed = 2.0f;
        vehicle.absClock += stepSeconds;
        const float period = settings.absRateHz > 0.0f ? 1.0f / settings.absRateHz : 0.0f;
        if (vehicle.absClock >= period)
        {
            vehicle.absClock = 0.0f;
            const JPH::Array<JPH::Wheel*>& wheels = vehicle.constraint->GetWheels();
            for (size_t index = 0; index < wheels.size() && index < vehicle.absReleased.size(); ++index)
            {
                const JPH::Wheel& wheel = *wheels[static_cast<JPH::uint>(index)];
                bool release = false;
                if (wheel.HasContact() && std::abs(forwardSpeed) > kMinSpeed)
                {
                    // How much slower than the road the tread turns, as a share of the road's speed.
                    const float road = (vehicle.body->GetPointVelocity(wheel.GetContactPosition()) - wheel.GetContactPointVelocity()).Dot(wheel.GetContactLongitudinal());
                    const float tread = wheel.GetAngularVelocity() * wheel.GetSettings()->mRadius;
                    release = std::abs(road) > kMinSpeed && (road - tread) / road > settings.absSlipRatioLimit;
                }
                vehicle.absReleased[index] = release;
            }
        }
        for (size_t index = 0; index < vehicle.wheelSettings.size() && index < vehicle.absReleased.size(); ++index)
        {
            if (vehicle.absReleased[index])
            {
                vehicle.wheelSettings[index]->mMaxBrakeTorque = 0.0f;
            }
        }
    }

    // Sets each wheel's brake torque for the coming step. Fixed, it is the front/rear split's. Dynamic, the
    // four wheels share the same total by the load they carried in the last step, a little smoothed, so a
    // wheel that weight has left brakes less and one it has moved to brakes more, all wheels reaching
    // their grip together.
    void DistributeBrakeTorque(Vehicle& vehicle) const
    {
        const size_t count = std::min(vehicle.wheelSettings.size(), vehicle.staticBrakeTorque.size());
        const auto useFixedSplit = [&]
        {
            for (size_t index = 0; index < count; ++index)
            {
                vehicle.wheelSettings[index]->mMaxBrakeTorque = vehicle.staticBrakeTorque[index];
            }
        };
        if (!vehicle.dynamicBrakeBias || vehicle.current.wheels.size() < count)
        {
            useFixedSplit();
            return;
        }

        // The loads over about three steps: the solver's force on a wheel jumps about from step to step.
        constexpr float kLoadFilterSeconds = 0.05f;
        // No wheel takes more than half the total, so one wheel on the ground does not carry the whole
        // car's brakes.
        constexpr float kMaxShareOfTotal = 0.5f;
        const float blend = vehicle.filteredLoadValid ? 1.0f - std::exp(-stepSeconds / kLoadFilterSeconds) : 1.0f;
        float total = 0.0f;
        float loadSum = 0.0f;
        for (size_t index = 0; index < count; ++index)
        {
            const VehicleWheelState& wheel = vehicle.current.wheels[index];
            const float load = wheel.inContact ? std::max(wheel.suspensionForce, 0.0f) : 0.0f;
            vehicle.filteredLoad[index] += (load - vehicle.filteredLoad[index]) * blend;
            loadSum += vehicle.filteredLoad[index];
            total += vehicle.staticBrakeTorque[index];
        }
        vehicle.filteredLoadValid = true;

        // With hardly any weight on the ground (just dropped, or launched off a jump) the loads say nothing.
        if (loadSum < vehicle.weightNewtons * 0.1f)
        {
            useFixedSplit();
            return;
        }
        for (size_t index = 0; index < count; ++index)
        {
            vehicle.wheelSettings[index]->mMaxBrakeTorque = total * std::min(vehicle.filteredLoad[index] / loadSum, kMaxShareOfTotal);
        }
    }

    // The automatic gearbox: picks the gear by the car's speed and the throttle and hands the physics
    // engine the gear and the clutch. While the gears change the engine is cut (the throttle goes
    // through the clutch) and its revs follow the new gear, as a dual-clutch box blips or cuts them.
    void ShiftGears(Vehicle& vehicle, VehicleDriverInput& input, float forwardSpeed) const
    {
        auto* controller = static_cast<JPH::WheeledVehicleController*>(vehicle.constraint->GetController());
        // The gearbox output turns with the driven wheels; by the car's speed when they turn slower (locked
        // under the brakes), so the box never picks a gear the engine could not take once they roll again.
        const JPH::VehicleDifferentialSettings& differential = controller->GetDifferentials().front();
        float wheelSpeed = 0.0f;
        int driven = 0;
        for (const JPH::VehicleDifferentialSettings& each : controller->GetDifferentials())
        {
            for (const int index : {each.mLeftWheel, each.mRightWheel})
            {
                if (index >= 0)
                {
                    wheelSpeed += std::abs(vehicle.constraint->GetWheel(static_cast<JPH::uint>(index))->GetAngularVelocity());
                    ++driven;
                }
            }
        }
        const float wheelRpm = wheelSpeed / static_cast<float>(std::max(driven, 1)) * differential.mDifferentialRatio * JPH::VehicleEngine::cAngularVelocityToRPM;
        const float outputRpm = std::max(std::abs(forwardSpeed) * vehicle.outputRpmPerSpeed, wheelRpm);
        VehicleGearboxState& state = vehicle.gearboxState;
        const float engineRpm = controller->GetEngine().GetCurrentRPM();
        const bool manual = vehicle.controls.manualGearbox;
        if (manual)
        {
            // The driver's changes since the last step; the clutch pedal and the hand brake declutch.
            UpdateManualGearbox(vehicle.gearbox, state, std::exchange(vehicle.pendingGearShifts, 0), input.forward, outputRpm, stepSeconds,
                                engineRpm, vehicle.controls.clutchPedal || input.handBrake > 0.0f);
            vehicle.direction = state.gear < 0 ? -1.0f : 1.0f;
        }
        else
        {
            // The driver's changes, if any, as a tiptronic's: the box holds the gear a while after each.
            UpdateAutomaticGearbox(vehicle.gearbox, state, input.forward, outputRpm, stepSeconds, engineRpm,
                                   std::exchange(vehicle.pendingGearShifts, 0));
        }
        JPH::VehicleTransmission& transmission = controller->GetTransmission();
        transmission.Set(state.gear, state.clutch);
        if (state.revMatch)
        {
            controller->GetEngine().SetCurrentRPM(VehicleGearRpm(vehicle.gearbox, state.gear, outputRpm));
        }
        // The engine is cut while a change opens the clutch, and on an upshift for the data's cut time;
        // launching, the clutch slips on full revs. The manual box's open clutch (neutral, the pedal, the hand brake)
        // leaves the engine free to rev.
        if (!state.launching && !(manual && state.idling))
        {
            input.forward *= state.cutLeft > 0.0f ? 0.0f : state.clutch;
        }
    }

    // Traction control by the clutch: it slips once the engine asks the driven wheels for more torque than their
    // tyres can hold (their peak grip on the load they carry, times the setting), so that the wheels get no more
    // than they can pass to the ground while the engine keeps its revs and its throttle, as a driver slips the
    // clutch on a start. The clutch's torque is its strength times the gap between the engine's speed and the
    // wheels', so the strength is set to the torque allowed over the gap, up to its own.
    void LimitClutchTorque(Vehicle& vehicle, JPH::WheeledVehicleController& controller) const
    {
        SetClutchStrength(vehicle, controller);
        CapClutchTorque(vehicle, controller);
    }

    // The data's clutch torque limit (CLUTCH MAX_TORQUE): the physics engine's clutch passes its
    // strength times the gap between the engine's speed and the driven wheels' (geared), with no end,
    // so the strength is held to what passes the limit at the gap now. Explicit, as traction control's.
    void CapClutchTorque(const Vehicle& vehicle, JPH::WheeledVehicleController& controller) const
    {
        const float limit = vehicle.settings.clutchMaxTorque;
        JPH::VehicleTransmission& transmission = controller.GetTransmission();
        const float ratio = std::abs(transmission.GetCurrentRatio());
        if (limit <= 0.0f || ratio < 1e-3f || controller.GetDifferentials().empty())
        {
            return;
        }
        float wheelSpeed = 0.0f;
        int driven = 0;
        for (const JPH::VehicleDifferentialSettings& differential : controller.GetDifferentials())
        {
            for (const int index : {differential.mLeftWheel, differential.mRightWheel})
            {
                if (index >= 0)
                {
                    wheelSpeed += vehicle.constraint->GetWheel(static_cast<JPH::uint>(index))->GetAngularVelocity();
                    ++driven;
                }
            }
        }
        const float overall = ratio * controller.GetDifferentials()[0].mDifferentialRatio;
        const float engineSpeed = controller.GetEngine().GetAngularVelocity();
        const float gap = std::abs(engineSpeed - std::abs(wheelSpeed) / static_cast<float>(std::max(driven, 1)) * overall);
        const float friction = std::max(transmission.GetClutchFriction(), 1e-3f);
        transmission.mClutchStrength = std::min(transmission.mClutchStrength, limit / (friction * std::max(gap, 1e-3f)));
    }

    void SetClutchStrength(Vehicle& vehicle, JPH::WheeledVehicleController& controller) const
    {
        JPH::VehicleTransmission& transmission = controller.GetTransmission();
        const float ratio = std::abs(transmission.GetCurrentRatio());
        if (vehicle.tractionControlGrip <= 0.0f || ratio < 1e-3f || controller.GetDifferentials().empty() ||
            vehicle.current.wheels.size() < kVehicleWheelCount)
        {
            transmission.mClutchStrength = vehicle.defaultClutchStrength;
            return;
        }
        float holdTorque = 0.0f;
        float wheelSpeed = 0.0f;
        int driven = 0;
        float worstSlip = 0.0f;
        bool brush = false;
        const float carSpeed = std::abs((vehicle.body->GetRotation().Conjugated() * vehicle.body->GetLinearVelocity()).GetZ());
        // A coupled four-wheel drive holds on all four tyres, but the engine turns with the rear alone.
        const bool coupled = vehicle.drive == VehicleDrive::AllWheel && vehicle.settings.centreDrive == VehicleCentreDrive::Coupling;
        for (size_t index = 0; index < kVehicleWheelCount; ++index)
        {
            const bool front = index < 2;
            const bool isDriven = vehicle.drive == VehicleDrive::AllWheel || (vehicle.drive == VehicleDrive::FrontWheel) == front;
            const VehicleWheelState& wheel = vehicle.current.wheels[index];
            if (!isDriven)
            {
                continue;
            }
            if (!(coupled && front))
            {
                ++driven;
                wheelSpeed += std::abs(wheel.angularVelocity);
            }
            if (wheel.inContact)
            {
                const float load = std::max(wheel.suspensionForce, 0.0f);
                float hold = wheel.longitudinalPeakFriction * load;
                if (wheel.brushTyre)
                {
                    // The brush tyre shares one friction ellipse between the two directions: what
                    // cornering takes is not there for the drive.
                    const float cornering = wheel.lateralPeakFriction * load > 0.0f ? wheel.lateralForce / (wheel.lateralPeakFriction * load) : 1.0f;
                    hold *= std::sqrt(std::max(1.0f - cornering * cornering, 0.0f));
                    brush = true;
                    worstSlip = std::max(worstSlip, (std::abs(wheel.angularVelocity) * wheel.radius - carSpeed) / std::max(carSpeed, 3.0f));
                }
                holdTorque += hold * wheel.radius;
            }
        }
        const float overall = ratio * controller.GetDifferentials()[0].mDifferentialRatio;
        float engineSide = holdTorque * vehicle.tractionControlGrip / overall;
        if (brush)
        {
            // And past the slip where the brush tyre's drive peaks the clutch lets go further: traction
            // control watching the wheels spin up, as the game's does (SLIP_RATIO_LIMIT).
            constexpr float kSlipLimit = 0.1f;
            engineSide *= std::clamp(2.0f - worstSlip / kSlipLimit, 0.2f, 1.0f);
        }
        const float engineSpeed = controller.GetEngine().GetCurrentRPM() * 2.0f * std::numbers::pi_v<float> / 60.0f;
        const float gap = std::abs(engineSpeed - wheelSpeed / static_cast<float>(std::max(driven, 1)) * overall);
        transmission.mClutchStrength = std::clamp(engineSide / std::max(gap, 1.0f), 0.05f, vehicle.defaultClutchStrength);
    }

    // A limited-slip differential as a clutch pack between the two wheels of an axle: a torque, growing with
    // how much faster one turns than the other and held to a limit that follows the drive torque, is taken
    // from the faster wheel and given to the slower, so that the pair is kept turning together and the drive
    // goes where the grip is. The physics engine's own gives all the torque to the slower wheel once the gap
    // passes a ratio: that wheel then spins up past the other and they take turns, each spinning several times
    // the ground's speed in turn.
    //
    // Each axle's pack locks with the torque through its differential: the engine's share for one it drives,
    // and for the front of a coupled four-wheel drive what the centre coupling passed. A four-wheel drive's
    // data may give each axle its own lock and preload (settings.axleDifferentials). The engine's torque is
    // what it makes less its braking (EngineCoastTorque), through the clutch as far as it grips: driving, the
    // pack locks by the power share, on the overrun by the coast share (Assetto Corsa's POWER and COAST).
    void CoupleDifferentialWheels(Vehicle& vehicle, JPH::WheeledVehicleController& controller) const
    {
        if (vehicle.limitedSlipLock <= 0.0f)
        {
            return;
        }
        // Holds the wheels together even when coasting, for a car whose data gives no preload.
        constexpr float kDefaultPreloadTorque = 40.0f;
        const VehicleSettings& settings = vehicle.settings;
        const JPH::VehicleEngine& engine = controller.GetEngine();
        const JPH::VehicleTransmission& transmission = controller.GetTransmission();
        const float netTorque = engine.GetTorque(std::abs(controller.GetForwardInput())) - EngineCoastTorque(settings, engine.GetCurrentRPM(), vehicle.pedal);
        // What the drivetrain's losses (ApplyDrivetrainLoss) leave of it.
        const float engineTorque = netTorque + DrivetrainLossTorque(netTorque, StepDrivetrainEfficiency(vehicle, controller));
        const bool overrun = engineTorque < 0.0f;
        const float driveTorque = std::abs(engineTorque) * transmission.GetClutchFriction() * std::abs(transmission.GetCurrentRatio());
        const float carCoast = settings.limitedSlipCoast >= 0.0f ? settings.limitedSlipCoast : vehicle.limitedSlipLock;
        const float carPreload = settings.limitedSlipPreload >= 0.0f ? settings.limitedSlipPreload : kDefaultPreloadTorque;
        const float finalDrive = controller.GetDifferentials().empty() ? 1.0f : controller.GetDifferentials()[0].mDifferentialRatio;
        const bool coupled = vehicle.drive == VehicleDrive::AllWheel && vehicle.settings.centreDrive == VehicleCentreDrive::Coupling;
        for (int axle = 0; axle < 2; ++axle)
        {
            const int leftIndex = 2 * axle;
            float axleTorque = 0.0f; // through this axle's differential, at the wheels
            bool hasDifferential = false;
            for (const JPH::VehicleDifferentialSettings& differential : controller.GetDifferentials())
            {
                if (differential.mLeftWheel == leftIndex && differential.mRightWheel == leftIndex + 1)
                {
                    axleTorque += driveTorque * differential.mDifferentialRatio * differential.mEngineTorqueRatio;
                    hasDifferential = true;
                }
            }
            if (coupled && axle == 0)
            {
                axleTorque = std::abs(vehicle.centreCouplingTorque) * finalDrive;
                hasDifferential = true;
            }
            if (!hasDifferential)
            {
                continue;
            }
            const VehicleAxleDifferential& own = vehicle.settings.axleDifferentials[static_cast<size_t>(axle)];
            const float power = own.lock >= 0.0f ? own.lock : vehicle.limitedSlipLock;
            const float coast = own.coast >= 0.0f ? own.coast : carCoast;
            const float lock = overrun ? coast : power;
            const float preload = own.preload >= 0.0f ? own.preload : carPreload;
            auto* left = static_cast<JPH::WheelWV*>(vehicle.constraint->GetWheels()[static_cast<JPH::uint>(leftIndex)]);
            auto* right = static_cast<JPH::WheelWV*>(vehicle.constraint->GetWheels()[static_cast<JPH::uint>(leftIndex + 1)]);
            const float limit = preload + lock * axleTorque * 0.5f;
            // Stiff enough that a wheel is pulled to the other's speed within a few steps, and no stiffer than
            // the step can integrate.
            const float inertia = 0.5f * (left->GetSettings()->mInertia + right->GetSettings()->mInertia);
            const float stiffness = 0.25f * inertia / stepSeconds;
            const float torque = std::clamp(stiffness * (left->GetAngularVelocity() - right->GetAngularVelocity()), -limit, limit);
            left->ApplyTorque(-torque, stepSeconds);
            right->ApplyTorque(torque, stepSeconds);
        }
    }

    // A coupled four-wheel drive's centre (Assetto Corsa's AWD2): a viscous coupling at the transfer case
    // between the driven rear and the front, passing ComputeCentreCouplingTorque on the axles' mean wheel
    // speeds. At the wheels the front axle gains the shaft torque times the final drive and the rear loses
    // as much, half to each wheel (the axles' packs then share it out). The coupling acts as a damper on
    // the axles' speed difference, applied explicitly: its rate is held to what one step takes without
    // overshooting, 1 / (dt (1/(2 I_f) + 1/(2 I_r))).
    void ApplyCentreCoupling(Vehicle& vehicle, const JPH::WheeledVehicleController& controller)
    {
        vehicle.centreCouplingTorque = 0.0f;
        const VehicleSettings& settings = vehicle.settings;
        if (vehicle.drive != VehicleDrive::AllWheel || settings.centreDrive != VehicleCentreDrive::Coupling || controller.GetDifferentials().empty())
        {
            return;
        }
        const JPH::Array<JPH::Wheel*>& wheels = vehicle.constraint->GetWheels();
        auto* frontLeft = static_cast<JPH::WheelWV*>(wheels[0]);
        auto* frontRight = static_cast<JPH::WheelWV*>(wheels[1]);
        auto* rearLeft = static_cast<JPH::WheelWV*>(wheels[2]);
        auto* rearRight = static_cast<JPH::WheelWV*>(wheels[3]);
        const float finalDrive = controller.GetDifferentials()[0].mDifferentialRatio;
        const float frontSpeed = 0.5f * (frontLeft->GetAngularVelocity() + frontRight->GetAngularVelocity());
        const float rearSpeed = 0.5f * (rearLeft->GetAngularVelocity() + rearRight->GetAngularVelocity());
        const float frontInertia = 0.5f * (frontLeft->GetSettings()->mInertia + frontRight->GetSettings()->mInertia);
        const float rearInertia = 0.5f * (rearLeft->GetSettings()->mInertia + rearRight->GetSettings()->mInertia);
        // The coupling's rate at the wheels (Nm per rad/s of the axles' mean speed difference), and the most the
        // step integrates.
        const float rate = settings.centreCouplingRampTorque * finalDrive * finalDrive;
        const float stableRate = 1.0f / (stepSeconds * (0.5f / std::max(frontInertia, 0.01f) + 0.5f / std::max(rearInertia, 0.01f)));
        const float ramp = rate > stableRate ? settings.centreCouplingRampTorque * stableRate / rate : settings.centreCouplingRampTorque;
        const float shaftTorque = ComputeCentreCouplingTorque(ramp, settings.centreCouplingMaxTorque, finalDrive, rearSpeed, frontSpeed);
        const float wheelTorque = 0.5f * shaftTorque * finalDrive;
        // The transfer case's gears lose their share of what goes through them, on the side the power goes to:
        // the front's when the coupling drives it, the rear's when the front drives the rear.
        float frontTorque = wheelTorque;
        float rearTorque = -wheelTorque;
        if (settings.drivetrainLosses.enabled)
        {
            const float transfer = std::clamp(settings.drivetrainLosses.transferEfficiency, 0.05f, 1.0f);
            const bool toFront = wheelTorque * frontSpeed >= 0.0f;
            (toFront ? frontTorque : rearTorque) *= transfer;
            vehicle.drivetrainMeshLoss += (1.0f - transfer) * std::abs(2.0f * wheelTorque * (toFront ? frontSpeed : rearSpeed));
        }
        frontLeft->ApplyTorque(frontTorque, stepSeconds);
        frontRight->ApplyTorque(frontTorque, stepSeconds);
        rearLeft->ApplyTorque(rearTorque, stepSeconds);
        rearRight->ApplyTorque(rearTorque, stepSeconds);
        vehicle.centreCouplingTorque = shaftTorque;
    }

    // Rear-wheel steering (settings.rearSteerControllers, Assetto Corsa's ctrl_4ws.ini): the controllers read
    // the driver and the car, their output gives the rear wheels' angle (ComputeRearSteerAngle), and the
    // rear wheels' forward axes are turned by it about the body's vertical. A multibody suspension has just
    // set those axes (UpdateCorners); a car on straight springs keeps its own from the start. The angle
    // does not go through the linkage: it is a fraction of a degree.
    void ApplyRearSteer(Vehicle& vehicle, const VehicleDriverInput& input)
    {
        const VehicleSettings& settings = vehicle.settings;
        if (settings.rearSteerControllers.empty() || vehicle.wheelSettings.size() < kVehicleWheelCount)
        {
            vehicle.rearSteerAngle = 0.0f;
            return;
        }
        const JPH::Quat toBody = vehicle.body->GetRotation().Conjugated();
        const JPH::Vec3 velocity = toBody * vehicle.body->GetLinearVelocity();
        VehicleControllerInputs inputs;
        const float lock = settings.steeringWheelLockDegrees > 0.0f ? settings.steeringWheelLockDegrees : 450.0f;
        inputs.steerDegrees = std::clamp(input.right, -1.0f, 1.0f) * lock;
        inputs.speedKmh = std::abs(velocity.GetZ()) * 3.6f;
        inputs.gas = std::max(input.forward, 0.0f);
        inputs.brake = std::clamp(input.brake, 0.0f, 1.0f);
        const auto* controller = static_cast<const JPH::WheeledVehicleController*>(vehicle.constraint->GetController());
        inputs.gear = static_cast<float>(controller->GetTransmission().GetCurrentGear());
        if (vehicle.controllerLastVelocityValid)
        {
            const JPH::Vec3 acceleration = toBody * ((vehicle.body->GetLinearVelocity() - vehicle.controllerLastVelocity) / stepSeconds);
            inputs.lateralG = acceleration.GetX() / 9.81f;
        }
        vehicle.controllerLastVelocity = vehicle.body->GetLinearVelocity();
        vehicle.controllerLastVelocityValid = true;
        if (vehicle.current.wheels.size() >= kVehicleWheelCount)
        {
            const auto slip = [&](size_t index)
            {
                return std::abs(vehicle.current.wheels[index].slipAngleDegrees);
            };
            inputs.slipAngleFrontAverage = 0.5f * (slip(0) + slip(1));
            inputs.slipAngleFrontMax = std::max(slip(0), slip(1));
            inputs.slipAngleRearAverage = 0.5f * (slip(2) + slip(3));
            inputs.slipAngleRearMax = std::max(slip(2), slip(3));
            inputs.oversteerFactor = inputs.slipAngleRearAverage - inputs.slipAngleFrontAverage;
        }
        vehicle.rearSteerAngle = ComputeRearSteerAngle(EvaluateVehicleControllers(settings.rearSteerControllers, inputs, vehicle.rearSteerFiltered, stepSeconds));
        // Right is a turn about -Y in the vehicle's frame (+X left, +Z forward).
        const JPH::Quat turn = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), -vehicle.rearSteerAngle);
        for (size_t side = 0; side < 2; ++side)
        {
            JPH::WheelSettingsWV& wheel = *vehicle.wheelSettings[2 + side];
            const JPH::Vec3 base = vehicle.corners.empty() ? vehicle.rearWheelForward[side] : wheel.mWheelForward;
            wheel.mWheelForward = turn * base;
        }
    }

    // The normal forces the wheels' suspensions will push with in the coming step, as the physics engine's
    // solver finds them (VehicleConstraint::SetupVelocityConstraint, solved to convergence). Each spring is
    // a soft constraint (Catto, "Soft constraints", GDC 2011), implicit in the step:
    //
    //   F_i = k_i x_i + (c_i + dt k_i)(v_i - b_i - dt sum_j W_ij F_j),
    //
    // with x the spring's compression at the length the step's collision just found, b the anti-roll bar's
    // bias, v the closing speed along the contact normal once the step has pulled the body by gravity and
    // the forces on it (the other parts' already added, the tyres' as they pushed in the last step) and W
    // the body's inverse mass matrix between the force points along their normals, through which the four
    // springs push on one another. The four equations are solved together, a spring that would pull let go.
    // The last step's impulse instead is a step old; it stands in where the solver will also push the wheel
    // against its bump stop, which cannot be foretold, and wherever the ground cannot be read.
    std::array<float, kVehicleWheelCount> PredictSuspensionForces(const Vehicle& vehicle, const JPH::VehicleConstraint& constraint,
                                                                  const JPH::PhysicsStepListenerContext& context) const
    {
        const float dt = context.mDeltaTime;
        const JPH::Array<JPH::Wheel*>& wheels = constraint.GetWheels();
        const size_t count = std::min<size_t>(wheels.size(), kVehicleWheelCount);
        std::array<float, kVehicleWheelCount> loads{};
        for (size_t index = 0; index < count; ++index)
        {
            loads[index] = wheels[index]->HasContact() ? std::max(wheels[index]->GetSuspensionLambda() / dt, 0.0f) : 0.0f;
        }
        const JPH::Body& body = *constraint.GetVehicleBody();
        if (!body.IsDynamic())
        {
            return loads;
        }
        const JPH::RMat44 transform = body.GetWorldTransform();
        const JPH::RVec3 com = body.GetCenterOfMassPosition();
        const JPH::MotionProperties& motion = *body.GetMotionProperties();
        const JPH::Mat44 inverseInertia = body.GetInverseInertia();
        const float inverseMass = motion.GetInverseMass();

        // The body's velocity once the step has applied gravity and the forces on it, as the solver starts.
        JPH::Vec3 force = body.GetAccumulatedForce();
        JPH::Vec3 torque = body.GetAccumulatedTorque();
        for (size_t index = 0; index < count && index < vehicle.brushTyres.size(); ++index)
        {
            const Vehicle::BrushWheel& last = vehicle.brushWheels[index];
            if (last.contact && wheels[index]->HasContact())
            {
                force += last.force;
                torque += JPH::Vec3(wheels[index]->GetContactPosition() - com).Cross(last.force) + wheels[index]->GetContactNormal() * static_cast<float>(last.out.Mz);
            }
        }
        const JPH::Vec3 linear = body.GetLinearVelocity() + (context.mPhysicsSystem->GetGravity() * motion.GetGravityFactor() + force * inverseMass) * dt;
        const JPH::Vec3 angular = body.GetAngularVelocity() + inverseInertia.Multiply3x3(torque) * dt;

        // A spring off the ground pushes nothing; one whose force is known (the last step's) or let go
        // (nothing) still pushes on the others through the body; the rest are solved for.
        enum class Push
        {
            None,
            Known,
            Solved,
        };
        struct Spring
        {
            Push push = Push::None;
            JPH::Vec3 normal, arm; // the contact normal and the force point's arm about the centre of mass
            float soft = 0.0f, inverseGround = 0.0f, rightSide = 0.0f;
        };
        std::array<Spring, kVehicleWheelCount> springs{};
        for (size_t index = 0; index < count; ++index)
        {
            const JPH::Wheel& wheel = *wheels[index];
            const JPH::WheelSettings& settings = *wheel.GetSettings();
            if (!wheel.HasContact())
            {
                continue;
            }
            Spring& spring = springs[index];
            spring.push = Push::Known;
            spring.normal = wheel.GetContactNormal();
            const JPH::RVec3 point = settings.mEnableSuspensionForcePoint ? transform * settings.mSuspensionForcePoint : wheel.GetContactPosition();
            spring.arm = JPH::Vec3(point - com);
            if (!(settings.mSuspensionMaxLength > settings.mSuspensionMinLength) || wheel.GetSuspensionLength() < settings.mSuspensionMinLength)
            {
                continue;
            }

            float stiffness = settings.mSuspensionSpring.mStiffness;
            float damping = settings.mSuspensionSpring.mDamping;
            if (settings.mSuspensionSpring.mMode == JPH::ESpringMode::FrequencyAndDamping)
            {
                const JPH::Vec3 forcePoint = settings.mEnableSuspensionForcePoint
                                                 ? settings.mSuspensionForcePoint
                                                 : settings.mPosition + 0.5f * (settings.mSuspensionMinLength + settings.mSuspensionMaxLength) * settings.mSuspensionDirection;
                const JPH::Vec3 arm = forcePoint.Cross(-constraint.GetLocalUp());
                const float mass = 1.0f / (inverseMass + arm.Dot(motion.GetLocalSpaceInverseInertia().Multiply3x3(arm)));
                const float omega = 2.0f * JPH::JPH_PI * settings.mSuspensionSpring.mFrequency;
                stiffness = mass * omega * omega;
                damping = 2.0f * mass * settings.mSuspensionSpring.mDamping * omega;
            }
            const float cosine = std::max(0.1f, transform.Multiply3x3(settings.mSuspensionDirection).Dot(-spring.normal));
            stiffness /= cosine;
            damping /= cosine;
            if (!(stiffness > 0.0f))
            {
                continue;
            }
            spring.soft = damping + dt * stiffness;

            // The ground's velocity at the point and, when it moves, its own give.
            JPH::Vec3 groundVelocity = wheel.GetContactPointVelocity();
            {
                JPH::BodyLockRead lock(context.mPhysicsSystem->GetBodyLockInterfaceNoLock(), wheel.GetContactBodyID());
                if (!lock.Succeeded())
                {
                    continue;
                }
                const JPH::Body& ground = lock.GetBody();
                groundVelocity = ground.GetPointVelocity(point);
                if (ground.IsDynamic())
                {
                    const JPH::Vec3 groundArm = JPH::Vec3(point - ground.GetCenterOfMassPosition()).Cross(spring.normal);
                    spring.inverseGround = ground.GetMotionProperties()->GetInverseMass() + groundArm.Dot(ground.GetInverseInertia().Multiply3x3(groundArm));
                }
            }
            const float closing = -spring.normal.Dot(linear + angular.Cross(spring.arm) - groundVelocity);

            // The anti-roll bar's bias, as the physics engine is about to set it from the two wheels' lengths.
            float bias = 0.0f;
            for (const JPH::VehicleAntiRollBar& bar : constraint.GetAntiRollBars())
            {
                if (bar.mLeftWheel != index && bar.mRightWheel != index)
                {
                    continue;
                }
                const JPH::Wheel& left = *wheels[bar.mLeftWheel];
                const JPH::Wheel& right = *wheels[bar.mRightWheel];
                if (left.HasContact() && right.HasContact())
                {
                    const float impulse = (right.GetSuspensionLength() - left.GetSuspensionLength()) * bar.mStiffness * dt;
                    bias += bar.mLeftWheel == index ? -impulse : impulse;
                }
            }
            const float compression = settings.mSuspensionMaxLength + settings.mSuspensionPreloadLength - wheel.GetSuspensionLength();
            spring.rightSide = stiffness * compression + spring.soft * (closing - bias);
            spring.push = Push::Solved;
        }

        // The body's inverse mass between two springs' force points along their normals.
        const auto coupling = [&](const Spring& a, const Spring& b)
        {
            const JPH::Vec3 armA = a.arm.Cross(a.normal);
            const JPH::Vec3 armB = b.arm.Cross(b.normal);
            return inverseMass * a.normal.Dot(b.normal) + armA.Dot(inverseInertia.Multiply3x3(armB));
        };
        // (1 + dt soft_i W_ii) F_i + dt soft_i sum_j W_ij F_j = right side, springs that would pull let go
        // and the rest solved again.
        for (int round = 0; round < static_cast<int>(kVehicleWheelCount); ++round)
        {
            std::array<size_t, kVehicleWheelCount> unknown{};
            size_t n = 0;
            for (size_t index = 0; index < count; ++index)
            {
                if (springs[index].push == Push::Solved)
                {
                    unknown[n++] = index;
                }
            }
            if (n == 0)
            {
                break;
            }
            std::array<std::array<double, kVehicleWheelCount + 1>, kVehicleWheelCount> m{};
            for (size_t r = 0; r < n; ++r)
            {
                const Spring& a = springs[unknown[r]];
                const double scale = static_cast<double>(dt) * a.soft;
                double right = a.rightSide;
                for (size_t index = 0; index < count; ++index)
                {
                    if (springs[index].push == Push::Known)
                    {
                        right -= scale * coupling(a, springs[index]) * loads[index];
                    }
                }
                for (size_t c = 0; c < n; ++c)
                {
                    const Spring& b = springs[unknown[c]];
                    m[r][c] = scale * (coupling(a, b) + (r == c ? a.inverseGround : 0.0f)) + (r == c ? 1.0 : 0.0);
                }
                m[r][n] = right;
            }
            // Gaussian elimination with partial pivoting.
            for (size_t col = 0; col < n; ++col)
            {
                size_t pivot = col;
                for (size_t r = col + 1; r < n; ++r)
                {
                    if (std::abs(m[r][col]) > std::abs(m[pivot][col]))
                    {
                        pivot = r;
                    }
                }
                std::swap(m[col], m[pivot]);
                if (!(std::abs(m[col][col]) > 1e-12))
                {
                    return loads;
                }
                for (size_t r = col + 1; r < n; ++r)
                {
                    const double factor = m[r][col] / m[col][col];
                    for (size_t c = col; c <= n; ++c)
                    {
                        m[r][c] -= factor * m[col][c];
                    }
                }
            }
            std::array<double, kVehicleWheelCount> solution{};
            for (size_t r = n; r-- > 0;)
            {
                double value = m[r][n];
                for (size_t c = r + 1; c < n; ++c)
                {
                    value -= m[r][c] * solution[c];
                }
                solution[r] = value / m[r][r];
            }
            bool pulling = false;
            for (size_t r = 0; r < n; ++r)
            {
                if (solution[r] < 0.0)
                {
                    springs[unknown[r]].push = Push::Known; // let go, at nothing
                    loads[unknown[r]] = 0.0f;
                    pulling = true;
                }
                else
                {
                    loads[unknown[r]] = static_cast<float>(solution[r]);
                }
            }
            if (!pulling)
            {
                break;
            }
        }
        return loads;
    }

    // The brush tyres, called by the vehicle constraint once it has found the ground and before the
    // controller turns the engine and brakes the wheels. Each tyre reads the wheel's motion over the
    // ground in the contact frame, the load the suspension will carry in this step and the camber to
    // the road, steps its carcass, and puts the road's force and aligning moment on the body at the
    // contact and the force's moment about the axle on the wheel's spin. The physics engine's own tyre
    // friction is off (its impulse limits are zero).
    void ApplyBrushTyres(Vehicle& vehicle, JPH::VehicleConstraint& constraint, const JPH::PhysicsStepListenerContext& context)
    {
        const float dt = context.mDeltaTime;
        if (vehicle.brushTyres.empty() || dt <= 0.0f)
        {
            return;
        }
        JPH::Body& body = *constraint.GetVehicleBody();
        JPH::BodyInterface& bodies = context.mPhysicsSystem->GetBodyInterfaceNoLock();
        const JPH::Quat rotation = body.GetRotation();
        const JPH::Array<JPH::Wheel*>& wheels = constraint.GetWheels();
        // All four loads before any tyre pushes: each spring's depends on the others' through the body.
        const std::array<float, kVehicleWheelCount> loads = PredictSuspensionForces(vehicle, constraint, context);
        const size_t count = std::min(vehicle.brushTyres.size(), static_cast<size_t>(wheels.size()));
        // Each tyre's input first, then the tyres stepped side by side (each touches only its own state),
        // then their forces put on the body and the wheels one after another.
        std::array<tyre::BrushTyreInput, kVehicleWheelCount> inputs{};
        // The contact's sideways speed before camber's thrust is folded into it (what the tread slides at).
        std::array<float, kVehicleWheelCount> lateralSlides{};
        std::array<JPH::Vec3, kVehicleWheelCount> normals{}, longitudinals{}, lefts{};
        std::array<JPH::RVec3, kVehicleWheelCount> positions{};
        for (size_t index = 0; index < count; ++index)
        {
            auto& wheel = *static_cast<JPH::WheelWV*>(wheels[static_cast<JPH::uint>(index)]);
            tyre::BrushTyreInput& in = inputs[index];
            in.wheelSpeed = wheel.GetAngularVelocity();
            if (!wheel.HasContact())
            {
                continue;
            }
            const JPH::Vec3 normal = normals[index] = wheel.GetContactNormal();
            const JPH::Vec3 longitudinal = longitudinals[index] = wheel.GetContactLongitudinal();
            const JPH::Vec3 left = lefts[index] = -wheel.GetContactLateral();
            const JPH::RVec3 position = positions[index] = wheel.GetContactPosition();
            const JPH::Vec3 velocity = body.GetPointVelocity(position) - wheel.GetContactPointVelocity();
            in.forwardVelocity = velocity.Dot(longitudinal);
            in.lateralVelocity = velocity.Dot(left);
            in.yawRate = body.GetAngularVelocity().Dot(normal);
            in.load = index < loads.size() ? loads[index] : std::max(wheel.GetSuspensionLambda() / dt, 0.0f);
            // Camber to the road from the wheel's axle (its right, steered): positive top right.
            JPH::Vec3 forward, up, right;
            constraint.GetWheelLocalBasis(&wheel, forward, up, right);
            in.camber = std::asin(std::clamp(static_cast<double>(-(rotation * right).Dot(normal)), -1.0, 1.0));
            {
                JPH::BodyLockRead lock(context.mPhysicsSystem->GetBodyLockInterfaceNoLock(), wheel.GetContactBodyID());
                if (lock.Succeeded())
                {
                    const SurfaceGrip grip = GripOf(lock.GetBody());
                    in.frictionScale = SurfaceFriction(grip, velocity.Length());
                    in.frictionCap = grip.frictionCap;
                    in.slidingShare = grip.slidingShare;
                    in.extraRollingResistance = grip.rollingResistance;
                }
            }
            // What the tyre's own data adds: camber thrust, grip by camber, sliding speed and braking, rolling
            // resistance by speed, slip and pressure, and the tyre's growth with spin.
            lateralSlides[index] = static_cast<float>(in.lateralVelocity);
            VehicleTyreMotion motion;
            motion.forwardVelocity = static_cast<float>(in.forwardVelocity);
            motion.lateralVelocity = static_cast<float>(in.lateralVelocity);
            motion.wheelSpeed = static_cast<float>(in.wheelSpeed);
            motion.radius = wheel.GetSettings()->mRadius;
            motion.camber = static_cast<float>(in.camber);
            motion.load = static_cast<float>(in.load);
            const VehicleTyreSettings& tyreData = vehicle.settings.tyres[index];
            double thermalGrip = 1.0;
            if (const std::optional<tyre::TyreThermalModel>& thermal = vehicle.thermals[index])
            {
                // The temperature's grip (the performance curve) over the pressure's loss off its ideal, the
                // pressure's rolling factor, and the vertical rate at this pressure.
                const double pressure = thermal->Pressure();
                const double ideal = thermal->Parameters().idealPressure;
                thermalGrip = thermal->Performance() / (1.0 + std::abs(pressure - ideal) * tyreData.pressureGripGain);
                motion.pressureFactor = static_cast<float>(thermal->PressureFactor());
                if (tyreData.verticalRate > 0.0f && tyreData.pressureSpringGain != 0.0f)
                {
                    const double rate =
                        std::max(tyreData.verticalRate + (pressure - thermal->Parameters().staticPressure) * tyreData.pressureSpringGain, 1000.0);
                    in.verticalRate = rate;
                    if (index < vehicle.corners.size())
                    {
                        vehicle.corners[index].tyreRate = rate;
                    }
                }
            }
            // Wear and blistering.
            if (const std::optional<tyre::TyreWearModel>& wear = vehicle.wears[index])
            {
                thermalGrip *= wear->Grip();
            }
            const VehicleTyreStepTerms terms = ComputeTyreStepTerms(tyreData, motion);
            vehicle.tyreSlips[index] = terms.slip;
            in.lateralVelocity = terms.lateralVelocity;
            in.axisFrictionScale = {terms.axisFrictionScale[0] * thermalGrip, terms.axisFrictionScale[1] * thermalGrip};
            in.extraRollingResistance += terms.extraRollingResistance;
            in.radiusGrowth = terms.radiusGrowth;
        }
        std::array<tyre::BrushTyreOutput, kVehicleWheelCount> outputs{};
        const auto step = [&](uint32_t begin, uint32_t end)
        {
            for (uint32_t index = begin; index < end; ++index)
            {
                outputs[index] = vehicle.brushTyres[index].Step(inputs[index], dt);
            }
        };
        // Finely cut tyres (the default 50 x 20 among them) are worth the tasks' wake-up; coarse cuts step inline.
        constexpr int kParallelSegments = 400;
        const tyre::BrushTyreParameters& cut = vehicle.brushTyres.front().Parameters();
        if (count > 1 && cut.ribs * cut.segmentsPerRib >= kParallelSegments)
        {
            TaskSystem::ParallelFor(static_cast<uint32_t>(count), 1, step);
        }
        else
        {
            step(0, static_cast<uint32_t>(count));
        }
        for (size_t index = 0; index < count; ++index)
        {
            auto& wheel = *static_cast<JPH::WheelWV*>(wheels[static_cast<JPH::uint>(index)]);
            Vehicle::BrushWheel& state = vehicle.brushWheels[index];
            const tyre::BrushTyreOutput& out = outputs[index];
            state = {};
            state.out = out;
            if (!wheel.HasContact())
            {
                continue;
            }
            const JPH::Vec3 force = longitudinals[index] * static_cast<float>(out.Fx) + lefts[index] * static_cast<float>(out.Fy);
            bodies.AddForce(body.GetID(), force, positions[index], JPH::EActivation::DontActivate);
            bodies.AddTorque(body.GetID(), normals[index] * static_cast<float>(out.Mz), JPH::EActivation::DontActivate);
            const float torque = static_cast<float>(-out.Fx * out.effectiveRadius);
            const float rolling = RollingResistanceTorque(wheel.GetAngularVelocity(), torque, wheel.GetSettings()->mInertia, static_cast<float>(out.rollingResistanceLimit), dt);
            wheel.ApplyTorque(torque + rolling, dt);
            state.force = force;
            state.load = static_cast<float>(inputs[index].load);
            state.contact = true;
        }
        // The tyres' temperatures, from what they did in this step: the tread's sliding speed and load, its spin
        // and camber, the road's grip and the car's speed (which cools it).
        const float carSpeed = body.GetLinearVelocity().Length();
        for (size_t index = 0; index < count; ++index)
        {
            std::optional<tyre::TyreThermalModel>& thermal = vehicle.thermals[index];
            if (!thermal)
            {
                continue;
            }
            const auto& wheel = *static_cast<const JPH::WheelWV*>(wheels[static_cast<JPH::uint>(index)]);
            const tyre::BrushTyreInput& in = inputs[index];
            const VehicleTyreSettings& tyreData = vehicle.settings.tyres[index];
            tyre::TyreThermalInput heat;
            heat.dt = dt;
            heat.wheelSpeed = wheel.GetAngularVelocity();
            heat.camber = in.camber;
            heat.carSpeed = carSpeed;
            if (wheel.HasContact() && in.load > 0.0)
            {
                const double treadSpeed = heat.wheelSpeed * outputs[index].effectiveRadius;
                heat.slideSpeed = std::hypot(static_cast<double>(lateralSlides[index]), treadSpeed - in.forwardVelocity);
                heat.load = in.load;
                // The lateral grip at this load before temperature and pressure (the game's static DY).
                heat.grip = tyreData.lateralReference > 0.0f && tyreData.referenceLoad > 0.0f && tyreData.lateralLoadExponent > 0.0f
                                ? tyreData.lateralReference * std::pow(in.load / tyreData.referenceLoad, tyreData.lateralLoadExponent - 1.0)
                                : tyreData.lateralGrip;
                heat.surfaceGrip = in.frictionScale;
            }
            thermal->Step(heat);
        }
        // And their wear, from the same step.
        for (size_t index = 0; index < count; ++index)
        {
            std::optional<tyre::TyreWearModel>& wear = vehicle.wears[index];
            if (!wear)
            {
                continue;
            }
            const auto& wheel = *static_cast<const JPH::WheelWV*>(wheels[static_cast<JPH::uint>(index)]);
            const tyre::BrushTyreInput& in = inputs[index];
            tyre::TyreWearInput rub;
            rub.dt = dt;
            if (wheel.HasContact() && in.load > 0.0)
            {
                const double treadSpeed = wheel.GetAngularVelocity() * outputs[index].effectiveRadius;
                rub.slideSpeed = std::hypot(static_cast<double>(lateralSlides[index]), treadSpeed - in.forwardVelocity);
                rub.contactSpeed = std::hypot(in.forwardVelocity, static_cast<double>(lateralSlides[index]));
                rub.load = in.load;
                rub.slip = vehicle.tyreSlips[index];
                rub.surfaceGrip = in.frictionScale;
            }
            if (const std::optional<tyre::TyreThermalModel>& thermal = vehicle.thermals[index])
            {
                rub.temperatures = true;
                rub.coreTemperature = thermal->CoreTemperature();
            }
            wear->Step(rub);
        }
    }

    // The physics engine does not turn a body through a step's rotation of 1e-6 rad or less
    // (Body::AddRotationStep), so at the 1 ms step a car turning slower than 1e-3 rad/s never turns while it
    // still moves. Standing braked on a slope the car then rolled over its tyres as a ball would: their
    // contacts stood still (the body turning about them) and the tyres held nothing back, while the turn
    // never came and the whole car moved up the slope instead, 0.2 mm/s. The turns left out are summed
    // here and made about the centre of mass, as the physics engine would, once they add up past its limit.
    void ApplyDroppedRotation(Vehicle& vehicle)
    {
        constexpr float kSmallestTurn = 1.0e-6f;
        const JPH::Body& body = *vehicle.body;
        if (!body.IsActive())
        {
            return;
        }
        const JPH::Vec3 turn = body.GetAngularVelocity() * stepSeconds;
        if (turn.Length() > kSmallestTurn)
        {
            return;
        }
        std::array<double, 3>& dropped = vehicle.droppedRotation;
        dropped[0] += turn.GetX();
        dropped[1] += turn.GetY();
        dropped[2] += turn.GetZ();
        const double angle = std::sqrt(dropped[0] * dropped[0] + dropped[1] * dropped[1] + dropped[2] * dropped[2]);
        if (angle <= kSmallestTurn)
        {
            return;
        }
        const JPH::Vec3 axis(static_cast<float>(dropped[0] / angle), static_cast<float>(dropped[1] / angle), static_cast<float>(dropped[2] / angle));
        const JPH::Quat rotation = (JPH::Quat::sRotation(axis, static_cast<float>(angle)) * body.GetRotation()).Normalized();
        const JPH::RVec3 origin = body.GetCenterOfMassPosition() - rotation * body.GetShape()->GetCenterOfMass();
        physicsSystem.GetBodyInterface().SetPositionAndRotation(body.GetID(), origin, rotation, JPH::EActivation::DontActivate);
        dropped = {};
    }

    // The physics engine turns each wheel through its roll angle by the spin it has after the tyre's
    // torque and before the brakes', which then stop it: a braked wheel whose tyre pulls on it (the rear
    // tyres holding the car on a slope with the hand brake on, or the tread's bend left from stopping)
    // turned on, some 7 deg/s on the R34 on a 5 deg slope, while its speed read zero. Turned instead by
    // the spin it ends the step with, a wheel the brakes hold stands still, and one that rolls turns as
    // the body moves, by its velocity at the step's end.
    void RememberWheelAngles(Vehicle& vehicle) const
    {
        const JPH::Array<JPH::Wheel*>& wheels = vehicle.constraint->GetWheels();
        for (size_t index = 0; index < wheels.size() && index < vehicle.spinAngleBefore.size(); ++index)
        {
            vehicle.spinAngleBefore[index] = wheels[static_cast<JPH::uint>(index)]->GetRotationAngle();
        }
    }

    void RollWheelsBySpinAfterStep(Vehicle& vehicle) const
    {
        constexpr float kTurn = 2.0f * std::numbers::pi_v<float>;
        const JPH::Array<JPH::Wheel*>& wheels = vehicle.constraint->GetWheels();
        for (size_t index = 0; index < wheels.size() && index < vehicle.spinAngleBefore.size(); ++index)
        {
            JPH::Wheel& wheel = *wheels[static_cast<JPH::uint>(index)];
            wheel.SetRotationAngle(std::fmod(vehicle.spinAngleBefore[index] + wheel.GetAngularVelocity() * stepSeconds, kTurn));
        }
    }

    // The turbos' boost follows the steady level the revs and throttle ask of each, by the game's lag (a
    // share of the distance kept each of its 333 Hz steps, here per our step), and the throttle the
    // physics engine's engine gets is scaled so its torque is the curve's (which has the full boost) times
    // (1 + boost) / (1 + full boost). The physics engine's torque is linear in that input.
    float SpoolTurbos(Vehicle& vehicle, const JPH::WheeledVehicleController& controller, float forward) const
    {
        const VehicleSettings& settings = vehicle.settings;
        if (settings.turbos.empty())
        {
            return forward;
        }
        constexpr float kGameStepsPerSecond = 333.0f;
        vehicle.turboBoost.resize(settings.turbos.size(), 0.0f);
        const float rpm = controller.GetEngine().GetCurrentRPM();
        const float throttle = std::abs(forward);
        float boost = 0.0f;
        for (size_t index = 0; index < settings.turbos.size(); ++index)
        {
            const VehicleTurbo& turbo = settings.turbos[index];
            float& now = vehicle.turboBoost[index];
            const float target = VehicleTurboBoost(turbo, rpm, throttle);
            const float lag = std::clamp(target > now ? turbo.lagUp : turbo.lagDown, 0.0f, 1.0f);
            const float kept = lag > 0.0f ? std::pow(lag, kGameStepsPerSecond * stepSeconds) : 0.0f;
            now = target + (now - target) * kept;
            boost += now;
        }
        return std::clamp(forward * VehicleTurboTorqueScale(settings, rpm, boost), -1.0f, 1.0f);
    }

    // Engine braking, which the physics engine's engine lacks (its torque is the throttle's share of the
    // curve, nothing with the throttle shut): Assetto Corsa's COAST_REF torque at its rpm, in proportion
    // to the rpm (its NON_LINEARITY not read) and to the throttle left closed, against the engine's
    // turning before the step couples it to the wheels through the clutch.
    static float EngineCoastTorque(const VehicleSettings& settings, float rpm, float pedal)
    {
        if (settings.engineCoastTorque <= 0.0f || settings.engineCoastRpm <= 0.0f)
        {
            return 0.0f;
        }
        const float closed = 1.0f - std::clamp(std::abs(pedal), 0.0f, 1.0f);
        return settings.engineCoastTorque * rpm / settings.engineCoastRpm * closed;
    }

    void ApplyEngineCoast(Vehicle& vehicle, JPH::WheeledVehicleController& controller, float forward) const
    {
        vehicle.pedal = forward;
        JPH::VehicleEngine& engine = controller.GetEngine();
        const float torque = EngineCoastTorque(vehicle.settings, engine.GetCurrentRPM(), forward);
        if (torque > 0.0f)
        {
            engine.ApplyTorque(-torque, stepSeconds);
        }
    }

    // The share of the engine's torque that reaches the wheels in this step (settings.drivetrainLosses): the
    // gear's, through the clutch as far as it is shut (an open clutch passes nothing to lose).
    float StepDrivetrainEfficiency(const Vehicle& vehicle, const JPH::WheeledVehicleController& controller) const
    {
        const JPH::VehicleTransmission& transmission = controller.GetTransmission();
        const float efficiency = DrivetrainEfficiency(vehicle.settings, transmission.GetCurrentRatio());
        return 1.0f - (1.0f - efficiency) * std::clamp(transmission.GetClutchFriction(), 0.0f, 1.0f);
    }

    // The gears' meshes' loss, taken at the engine (the physics engine's drivetrain passes everything): the engine's
    // torque less its drag is what would go through the clutch, and the torque added here makes the wheels get the
    // efficiency's share of it driving, and give it over the efficiency on the overrun (DrivetrainLossTorque).
    void ApplyDrivetrainLoss(Vehicle& vehicle, JPH::WheeledVehicleController& controller) const
    {
        if (!vehicle.settings.drivetrainLosses.enabled)
        {
            return;
        }
        JPH::VehicleEngine& engine = controller.GetEngine();
        const float net = engine.GetTorque(std::abs(controller.GetForwardInput())) - EngineCoastTorque(vehicle.settings, engine.GetCurrentRPM(), vehicle.pedal);
        const float loss = DrivetrainLossTorque(net, StepDrivetrainEfficiency(vehicle, controller));
        if (loss == 0.0f)
        {
            return;
        }
        engine.ApplyTorque(loss, stepSeconds);
        vehicle.drivetrainMeshLoss += std::abs(loss) * engine.GetCurrentRPM() * 2.0f * std::numbers::pi_v<float> / 60.0f;
    }

    // Churning and bearing drag on each axle the drivetrain turns (DrivetrainSpinTorque), shared by its two wheels and,
    // as dry friction is, never turning a wheel back.
    void ApplyDrivetrainSpin(Vehicle& vehicle)
    {
        const VehicleDrivetrainLosses& losses = vehicle.settings.drivetrainLosses;
        if (!losses.enabled)
        {
            return;
        }
        const JPH::Array<JPH::Wheel*>& wheels = vehicle.constraint->GetWheels();
        for (int axle = 0; axle < 2; ++axle)
        {
            const bool driven = vehicle.drive == VehicleDrive::AllWheel || (axle == 0) == (vehicle.drive == VehicleDrive::FrontWheel);
            if (!driven)
            {
                continue;
            }
            auto* left = static_cast<JPH::WheelWV*>(wheels[static_cast<JPH::uint>(2 * axle)]);
            auto* right = static_cast<JPH::WheelWV*>(wheels[static_cast<JPH::uint>(2 * axle + 1)]);
            const float speed = 0.5f * (left->GetAngularVelocity() + right->GetAngularVelocity());
            const float limit = 0.5f * DrivetrainSpinTorque(losses, speed);
            for (JPH::WheelWV* wheel : {left, right})
            {
                const float drag = RollingResistanceTorque(wheel->GetAngularVelocity(), 0.0f, wheel->GetSettings()->mInertia, limit, stepSeconds);
                vehicle.drivetrainSpinLoss += std::abs(drag * wheel->GetAngularVelocity());
                wheel->ApplyTorque(drag, stepSeconds);
            }
        }
    }

    // Soft ground's rolling resistance on the physics engine's tyres (the brush tyre has its own): a
    // moment against each wheel's roll of the surface's coefficient times the wheel's load and radius.
    void ApplySurfaceRollingResistance(Vehicle& vehicle)
    {
        if (!vehicle.brushTyres.empty())
        {
            return;
        }
        for (JPH::Wheel* wheel : vehicle.constraint->GetWheels())
        {
            if (!wheel->HasContact())
            {
                continue;
            }
            JPH::BodyLockRead lock(physicsSystem.GetBodyLockInterface(), wheel->GetContactBodyID());
            if (!lock.Succeeded())
            {
                continue;
            }
            const float coefficient = GripOf(lock.GetBody()).rollingResistance;
            if (coefficient <= 0.0f)
            {
                continue;
            }
            const float load = std::max(wheel->GetSuspensionLambda() / stepSeconds, 0.0f);
            const float radius = wheel->GetSettings()->mRadius;
            auto* driven = static_cast<JPH::WheelWV*>(wheel);
            const float rolling = RollingResistanceTorque(driven->GetAngularVelocity(), 0.0f, driven->GetSettings()->mInertia, coefficient * load * radius, stepSeconds);
            driven->ApplyTorque(rolling, stepSeconds);
        }
    }

    // Drag against the car's velocity and downforce along its down, on each surface where it sits.
    // Buoyancy and drag where the body is under the water's surface (see AddWaterSurface).
    void ApplyWater(Vehicle& vehicle, float deltaSeconds)
    {
        const JPH::RVec3 centre = vehicle.body->GetCenterOfMassPosition();
        const std::optional<float> surface = water.HeightAt(static_cast<float>(centre.GetX()), static_cast<float>(centre.GetZ()));
        if (!surface.has_value() || static_cast<float>(centre.GetY()) > *surface)
        {
            vehicle.underWaterInTunnel = false;
        }
        else if (vehicle.submergedShare <= 0.0f && *surface - static_cast<float>(centre.GetY()) > kTunnelDepth)
        {
            vehicle.underWaterInTunnel = true;
        }
        if (!surface.has_value() || vehicle.underWaterInTunnel)
        {
            vehicle.submergedShare = 0.0f;
            return;
        }
        float totalVolume = 0.0f;
        float submergedVolume = 0.0f;
        JPH::Vec3 centreOfBuoyancy = JPH::Vec3::sZero();
        const JPH::RVec3 surfacePoint(centre.GetX(), *surface, centre.GetZ());
        vehicle.body->GetSubmergedVolume(surfacePoint, JPH::Vec3::sAxisY(), totalVolume, submergedVolume, centreOfBuoyancy);
        vehicle.submergedShare = totalVolume > 0.0f ? std::clamp(submergedVolume / totalVolume, 0.0f, 1.0f) : 0.0f;
        if (vehicle.submergedShare <= 0.0f)
        {
            return;
        }
        vehicle.floodSeconds += deltaSeconds * vehicle.submergedShare;
        const float buoyancy = kSunkBuoyancy + (kAfloatBuoyancy - kSunkBuoyancy) * (1.0f - FloodedShare(vehicle.floodSeconds));
        physicsSystem.GetBodyInterfaceNoLock().ActivateBody(vehicle.body->GetID());
        vehicle.body->ApplyBuoyancyImpulse(
            totalVolume,
            submergedVolume,
            centreOfBuoyancy,
            buoyancy,
            kWaterLinearDrag,
            kWaterAngularDrag,
            JPH::Vec3::sZero(),
            physicsSystem.GetGravity(),
            deltaSeconds);
        if (vehicle.submergedShare > kEngineDrownShare)
        {
            vehicle.engineDrowned = true;
        }
    }

    void ApplyAerodynamics(const Vehicle& vehicle)
    {
        if (vehicle.aeroSurfaces.empty())
        {
            return;
        }
        const JPH::Vec3 velocity = vehicle.body->GetLinearVelocity();
        const float speed = velocity.Length();
        if (speed < 0.1f)
        {
            return;
        }
        constexpr float kAirDensity = 1.2f; // kg/m^3, as the game uses
        const float dynamicPressure = 0.5f * kAirDensity * speed * speed;
        const JPH::Quat rotation = vehicle.body->GetRotation();
        const JPH::Vec3 down = rotation * JPH::Vec3(0.0f, -1.0f, 0.0f);
        JPH::BodyInterface& bodies = physicsSystem.GetBodyInterface();
        for (const VehicleAeroSurface& surface : vehicle.aeroSurfaces)
        {
            const JPH::Vec3 force = -velocity / speed * (dynamicPressure * surface.dragArea) + down * (dynamicPressure * surface.downforceArea);
            const JPH::RVec3 point = vehicle.body->GetCenterOfMassPosition() + rotation * ToJolt(surface.position);
            bodies.AddForce(vehicle.body->GetID(), force, point);
        }
    }

    Impl()
        : tempAllocator(kTempAllocatorBytes),
          jobSystem(MakePhysicsJobSystem())
    {
        // The filter table reads the layer mappings when it is built, so they go in first.
        broadPhaseLayers = std::make_unique<JPH::BroadPhaseLayerInterfaceTable>(ObjectLayers::kCount, BroadPhaseLayers::kCount);
        broadPhaseLayers->MapObjectToBroadPhaseLayer(ObjectLayers::kStatic, BroadPhaseLayers::kStatic);
        broadPhaseLayers->MapObjectToBroadPhaseLayer(ObjectLayers::kMoving, BroadPhaseLayers::kMoving);
        objectLayerPairs = std::make_unique<JPH::ObjectLayerPairFilterTable>(ObjectLayers::kCount);
        objectLayerPairs->EnableCollision(ObjectLayers::kMoving, ObjectLayers::kStatic);
        objectLayerPairs->EnableCollision(ObjectLayers::kMoving, ObjectLayers::kMoving);
        objectVsBroadPhase = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
            *broadPhaseLayers, BroadPhaseLayers::kCount, *objectLayerPairs, ObjectLayers::kCount);

        physicsSystem.Init(
            kMaxBodies,
            kBodyMutexCount,
            kMaxBodyPairs,
            kMaxContactConstraints,
            *broadPhaseLayers,
            *objectVsBroadPhase,
            *objectLayerPairs);
        physicsSystem.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        physicsSystem.SetContactListener(&groundEdgeFilter);
    }

    ~Impl()
    {
        JPH::BodyInterface& bodies = physicsSystem.GetBodyInterface();
        for (Vehicle& vehicle : vehicles)
        {
            physicsSystem.RemoveStepListener(vehicle.constraint);
            physicsSystem.RemoveConstraint(vehicle.constraint);
            bodies.RemoveBody(vehicle.body->GetID());
            bodies.DestroyBody(vehicle.body->GetID());
        }
        for (const JPH::BodyID& body : staticBodies)
        {
            bodies.RemoveBody(body);
            bodies.DestroyBody(body);
        }
    }

    Vehicle& GetVehicle(VehicleId id)
    {
        if (id >= vehicles.size())
        {
            throw std::out_of_range("PhysicsWorld: unknown vehicle " + std::to_string(id));
        }
        return vehicles[id];
    }

    const Vehicle& GetVehicle(VehicleId id) const
    {
        return const_cast<Impl*>(this)->GetVehicle(id);
    }

    VehicleSnapshot Capture(const Vehicle& vehicle) const
    {
        VehicleSnapshot snapshot;
        snapshot.chassis.position = FromJoltPosition(vehicle.body->GetPosition());
        snapshot.chassis.rotation = FromJolt(vehicle.body->GetRotation());
        const JPH::Array<JPH::Wheel*>& wheels = vehicle.constraint->GetWheels();
        snapshot.wheels.reserve(wheels.size());
        for (JPH::uint index = 0; index < wheels.size(); ++index)
        {
            const JPH::Wheel& wheel = *wheels[index];
            VehicleWheelState state;
            // The wheel model's right is -X, the car's right; its up is +Y.
            state.pose = FromJolt(vehicle.constraint->GetWheelWorldTransform(index, -JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY()));
            // The roll is kept apart from the pose (GetVehicleWheels puts it back), for it is a turn about
            // the axle by an angle that the pose alone cannot follow past half a turn per step.
            constexpr float kTurn = 2.0f * std::numbers::pi_v<float>;
            state.spinAngle = wheel.GetRotationAngle();
            if (vehicle.previous.wheels.size() > index)
            {
                // The change in the physics engine's angle, folded to a half turn either way and then
                // moved by whole turns to the one nearest what the wheel's speed says it rolled.
                float step = state.spinAngle - vehicle.previous.wheels[index].spinAngle;
                step -= kTurn * std::round(step / kTurn);
                const float expected = wheel.GetAngularVelocity() * stepSeconds;
                step += kTurn * std::round((expected - step) / kTurn);
                state.spinStep = step;
            }
            state.pose.rotation = state.pose.rotation * glm::angleAxis(-state.spinAngle, glm::vec3(1.0f, 0.0f, 0.0f));
            state.inContact = wheel.HasContact();
            state.suspensionLength = wheel.GetSuspensionLength();
            state.angularVelocity = wheel.GetAngularVelocity();

            const JPH::WheelSettings& wheelSettings = *wheel.GetSettings();
            const JPH::Quat bodyRotation = vehicle.body->GetRotation();
            state.mount = FromJoltPosition(vehicle.body->GetPosition() + bodyRotation * wheelSettings.mPosition);
            state.suspensionAxis = FromJolt(bodyRotation * wheelSettings.mSuspensionDirection.Normalized());
            state.suspensionMinLength = wheelSettings.mSuspensionMinLength;
            state.suspensionMaxLength = wheelSettings.mSuspensionMaxLength;
            state.radius = wheelSettings.mRadius;
            state.width = wheelSettings.mWidth;
            state.brakeTorque = static_cast<const JPH::WheelWV&>(wheel).GetSettings()->mMaxBrakeTorque;
            if (index < vehicle.corners.size())
            {
                const Vehicle::Corner& corner = vehicle.corners[index];
                state.multibody = true;
                state.travel = static_cast<float>(corner.travel);
                state.camberDegrees = corner.camberDegrees;
                state.toeDegrees = corner.toeDegrees;
                state.springForce = corner.springForce;
                state.antiRollBarForce = corner.antiRollBarForce;
                if (corner.unsprung)
                {
                    // The physics engine's wheel sits on the ground; the hub is where its own motion
                    // has it, the tyre squashed between them.
                    state.unsprungMass = true;
                    state.tyreDeflection = static_cast<float>(corner.tyreDeflection);
                    state.pose.position = FromJoltPosition(vehicle.body->GetWorldTransform() * ToJolt(corner.hubCenter));
                    state.suspensionLength = corner.designLength - (corner.hubCenter.y - corner.designCenter.y);
                }
            }
            if (state.inContact)
            {
                const auto& wheelWV = static_cast<const JPH::WheelWV&>(wheel);
                // The solver's impulses over the step it ran are the step's mean forces.
                state.contactPosition = FromJoltPosition(wheel.GetContactPosition());
                state.contactNormal = FromJolt(wheel.GetContactNormal());
                state.contactLongitudinal = FromJolt(wheel.GetContactLongitudinal());
                state.contactLateral = FromJolt(wheel.GetContactLateral());
                state.suspensionForce = wheel.GetSuspensionLambda() / stepSeconds;
                state.longitudinalForce = wheel.GetLongitudinalLambda() / stepSeconds;
                state.lateralForce = wheel.GetLateralLambda() / stepSeconds;
                state.slipRatio = wheelWV.mLongitudinalSlip;
                state.slipAngleDegrees = JPH::RadiansToDegrees(wheelWV.mLateralSlip);
                state.longitudinalFriction = wheelWV.mCombinedLongitudinalFriction;
                state.lateralFriction = wheelWV.mCombinedLateralFriction;
                // The ground's share is what the combined value is of the curve's at this slip; where
                // that is nothing (no slip) the ground counts for what it does by default.
                const auto peakOf = [](const JPH::LinearCurve& curve, float slip, float combined)
                {
                    float peak = 0.0f;
                    for (const JPH::LinearCurve::Point& point : curve.mPoints)
                    {
                        peak = std::max(peak, point.mY);
                    }
                    const float atSlip = curve.GetValue(std::abs(slip));
                    return atSlip > 1e-3f ? peak * combined / atSlip : peak;
                };
                const JPH::WheelSettingsWV& tyre = *wheelWV.GetSettings();
                state.longitudinalPeakFriction = peakOf(tyre.mLongitudinalFriction, wheelWV.mLongitudinalSlip, wheelWV.mCombinedLongitudinalFriction);
                state.lateralPeakFriction = peakOf(tyre.mLateralFriction, JPH::RadiansToDegrees(wheelWV.mLateralSlip), wheelWV.mCombinedLateralFriction);
            }
            if (index < vehicle.brushTyres.size())
            {
                // The brush tyre's forces and slips replace the physics engine's (contactLateral is the right).
                const Vehicle::BrushWheel& brush = vehicle.brushWheels[index];
                const tyre::BrushTyreState& tyreState = vehicle.brushTyres[index].State();
                state.brushTyre = true;
                state.carcassDeflection = glm::vec3(static_cast<float>(tyreState.carcass[0]), static_cast<float>(tyreState.carcass[1]), static_cast<float>(tyreState.carcass[2]));
                state.carcassBendingShape = static_cast<float>(vehicle.brushTyres[index].Parameters().bendingShape);
                if (state.inContact && brush.contact)
                {
                    static_assert(VehicleWheelState::kMaxBrushRibs == tyre::kBrushMaxRibs);
                    state.treadRollingForward = brush.out.rollingForward;
                    state.brushRibCount = brush.out.ribCount;
                    for (int rib = 0; rib < brush.out.ribCount; ++rib)
                    {
                        const tyre::BrushRibContact& contact = brush.out.ribs[static_cast<size_t>(rib)];
                        state.brushRibs[static_cast<size_t>(rib)] = {static_cast<float>(contact.y), static_cast<float>(contact.length), static_cast<float>(contact.stuckLength)};
                    }
                    const float load = std::max(brush.load, 1.0f);
                    state.tyreLoad = brush.load;
                    state.longitudinalForce = static_cast<float>(brush.out.Fx);
                    state.lateralForce = static_cast<float>(-brush.out.Fy);
                    state.aligningTorque = static_cast<float>(brush.out.Mz);
                    state.slidingShare = static_cast<float>(brush.out.slidingShare);
                    state.slipRatio = static_cast<float>(brush.out.slipRatio);
                    state.slipAngleDegrees = static_cast<float>(brush.out.slipAngle * 180.0 / std::numbers::pi);
                    state.longitudinalFriction = static_cast<float>(std::abs(brush.out.Fx)) / load;
                    state.lateralFriction = static_cast<float>(std::abs(brush.out.Fy)) / load;
                    state.longitudinalPeakFriction = static_cast<float>(brush.out.peakFrictionX);
                    state.lateralPeakFriction = static_cast<float>(brush.out.peakFrictionY);
                }
            }
            snapshot.wheels.push_back(state);
        }
        return snapshot;
    }

    float Alpha() const
    {
        return std::clamp(accumulatedSeconds / stepSeconds, 0.0f, 1.0f);
    }

    // Declared before the physics system, so they outlive it.
    JPH::TempAllocatorImpl tempAllocator;
    WaterSurface water;
    std::unique_ptr<JPH::JobSystem> jobSystem;
    std::unique_ptr<JPH::BroadPhaseLayerInterfaceTable> broadPhaseLayers;
    std::unique_ptr<JPH::ObjectLayerPairFilterTable> objectLayerPairs;
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> objectVsBroadPhase;
    JPH::PhysicsSystem physicsSystem;

    std::vector<JPH::BodyID> staticBodies;
    size_t staticTriangleCount = 0;
    // Each static body's surface by its ID; a body not here (a car's chassis) grips by its friction alone.
    std::unordered_map<JPH::uint32, SurfaceGrip> surfaceGrips;

    SurfaceGrip GripOf(const JPH::Body& body) const
    {
        const auto found = surfaceGrips.find(body.GetID().GetIndexAndSequenceNumber());
        if (found != surfaceGrips.end())
        {
            return found->second;
        }
        SurfaceGrip grip;
        grip.friction = body.GetFriction();
        return grip;
    }

    // The friction ratio a surface gives at a speed over it, and its cap applied to a coefficient.
    static float SurfaceFriction(const SurfaceGrip& grip, float speed)
    {
        return grip.friction * std::exp(-grip.wetSpeedFalloff * speed);
    }
    static float CappedFriction(const SurfaceGrip& grip, float coefficient)
    {
        return grip.frictionCap > 0.0f ? std::min(coefficient, grip.frictionCap) : coefficient;
    }
    std::vector<Vehicle> vehicles;
    float stepSeconds = kDefaultStepSeconds;
    float accumulatedSeconds = 0.0f;
    bool broadPhaseDirty = false;
};

PhysicsWorld::PhysicsWorld()
{
    AcquireJolt();
    try
    {
        m_impl = std::make_unique<Impl>();
    }
    catch (...)
    {
        ReleaseJolt();
        throw;
    }
}

PhysicsWorld::~PhysicsWorld()
{
    m_impl.reset();
    ReleaseJolt();
}

bool PhysicsWorld::AddStaticMesh(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices, float friction)
{
    SurfaceGrip grip;
    grip.friction = friction;
    return AddStaticMesh(vertices, indices, grip);
}

bool PhysicsWorld::AddStaticMesh(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices, const SurfaceGrip& grip, const glm::dvec3& origin)
{
    const float friction = grip.friction;
    // Vertices at the same place become one. The physics engine finds a triangle's neighbours by the
    // vertices they share, and a mesh that gives each triangle or quad its own (for its normals or UVs,
    // as generated ground and imported tracks do) leaves every edge open: an open edge counts as a real
    // one (active), so the physics engine keeps the edge's own contact normal there. The car's body,
    // gliding a few millimetres over the ground, met the seam ahead of it with a normal leaning back as
    // far as GroundEdgeContactFilter lets through (25 degrees): at 90 km/h stopping the approach along it
    // threw the R34 up at 5 m/s. Welded, a seam between faces that meet almost flat (within 5 degrees) is
    // inactive, and its contacts take the face's normal.
    JPH::VertexList joltVertices;
    joltVertices.reserve(vertices.size());
    std::vector<uint32_t> welded(vertices.size());
    {
        // Mixed through every bit: grid coordinates (0.5 m steps) leave a float's low bits all zero.
        struct PositionHash
        {
            size_t operator()(const std::array<uint32_t, 3>& key) const noexcept
            {
                uint64_t hash = (static_cast<uint64_t>(key[0]) << 32 | key[1]) ^ (static_cast<uint64_t>(key[2]) * 0x9E3779B97F4A7C15ull);
                hash = (hash ^ (hash >> 30)) * 0xBF58476D1CE4E5B9ull;
                hash = (hash ^ (hash >> 27)) * 0x94D049BB133111EBull;
                return static_cast<size_t>(hash ^ (hash >> 31));
            }
        };
        std::unordered_map<std::array<uint32_t, 3>, uint32_t, PositionHash> unique;
        unique.reserve(vertices.size());
        for (size_t index = 0; index < vertices.size(); ++index)
        {
            const glm::vec3 vertex = vertices[index] + glm::vec3(0.0f); // -0 as +0
            const std::array<uint32_t, 3> key = {std::bit_cast<uint32_t>(vertex.x), std::bit_cast<uint32_t>(vertex.y), std::bit_cast<uint32_t>(vertex.z)};
            const auto [entry, added] = unique.try_emplace(key, static_cast<uint32_t>(joltVertices.size()));
            if (added)
            {
                joltVertices.push_back(JPH::Float3(vertex.x, vertex.y, vertex.z));
            }
            welded[index] = entry->second;
        }
    }

    JPH::IndexedTriangleList triangles;
    triangles.reserve(indices.size() / 3);
    for (size_t index = 0; index + 2 < indices.size(); index += 3)
    {
        if (indices[index] >= vertices.size() || indices[index + 1] >= vertices.size() || indices[index + 2] >= vertices.size())
        {
            continue;
        }
        const uint32_t a = welded[indices[index]];
        const uint32_t b = welded[indices[index + 1]];
        const uint32_t c = welded[indices[index + 2]];
        if (a == b || b == c || a == c)
        {
            continue;
        }
        triangles.push_back(JPH::IndexedTriangle(a, b, c));
    }
    if (triangles.empty())
    {
        return false;
    }

    // The car's wheels roll on the walkable triangles; the steep ones (walls) only stop the chassis.
    JPH::IndexedTriangleList walkable;
    JPH::IndexedTriangleList steep;
    for (const JPH::IndexedTriangle& triangle : triangles)
    {
        const JPH::Vec3 a(joltVertices[triangle.mIdx[0]]);
        const JPH::Vec3 b(joltVertices[triangle.mIdx[1]]);
        const JPH::Vec3 c(joltVertices[triangle.mIdx[2]]);
        const JPH::Vec3 normal = (b - a).Cross(c - a);
        const float length = normal.Length();
        if (!(length > 1e-12f))
        {
            continue; // degenerate
        }
        (normal.GetY() >= kMinWheelSurfaceNormalY * length ? walkable : steep).push_back(triangle);
    }

    // Each tile is a body of its own, stored to its own bounds.
    std::vector<JPH::IndexedTriangleList> tiles = CutIntoTiles(joltVertices, std::move(walkable));
    const size_t walkableTiles = tiles.size();
    for (JPH::IndexedTriangleList& tile : CutIntoTiles(joltVertices, std::move(steep)))
    {
        tiles.push_back(std::move(tile));
    }
    for (JPH::IndexedTriangleList& tile : tiles)
    {
        DropTrianglesFlatOnceStored(joltVertices, tile);
    }
    // The body's contacts with the walkable ground read them (GroundEdgeContactFilter).
    GroundEdgeTurns::Store(joltVertices, tiles);

    const float bodyFriction = std::max(friction, 0.0f);
    const auto addBody = [&](JPH::IndexedTriangleList list, uint64_t userData) -> size_t
    {
        if (list.empty())
        {
            return 0;
        }
        JPH::MeshShapeSettings shapeSettings(joltVertices, std::move(list));
        shapeSettings.mPerTriangleUserData = userData != kWheelsIgnoreBody;
        const size_t triangleCount = shapeSettings.mIndexedTriangles.size();
        const JPH::ShapeSettings::ShapeResult shape = shapeSettings.Create();
        if (shape.HasError() || triangleCount == 0)
        {
            return 0;
        }
        JPH::BodyCreationSettings bodySettings(
            shape.Get(), ToJoltPosition(origin), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ObjectLayers::kStatic);
        bodySettings.mFriction = bodyFriction;
        bodySettings.mUserData = userData;
        const JPH::BodyID body = m_impl->physicsSystem.GetBodyInterface().CreateAndAddBody(bodySettings, JPH::EActivation::DontActivate);
        if (body.IsInvalid())
        {
            throw std::runtime_error("PhysicsWorld: out of bodies for static geometry");
        }
        m_impl->staticBodies.push_back(body);
        m_impl->surfaceGrips[body.GetIndexAndSequenceNumber()] = grip;
        m_impl->staticTriangleCount += triangleCount;
        m_impl->broadPhaseDirty = true;
        return triangleCount;
    };
    size_t added = 0;
    for (size_t tile = 0; tile < tiles.size(); ++tile)
    {
        added += addBody(std::move(tiles[tile]), tile < walkableTiles ? 0 : kWheelsIgnoreBody);
    }
    return added > 0;
}

void PhysicsWorld::AddStaticBox(const glm::dvec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, float friction)
{
    const JPH::Vec3 extents = JPH::Vec3::sMax(ToJolt(halfExtents), JPH::Vec3::sReplicate(0.01f));
    JPH::BodyCreationSettings bodySettings(
        new JPH::BoxShape(extents), ToJoltPosition(center), ToJolt(rotation), JPH::EMotionType::Static, ObjectLayers::kStatic);
    bodySettings.mFriction = std::max(friction, 0.0f);
    const JPH::BodyID body = m_impl->physicsSystem.GetBodyInterface().CreateAndAddBody(bodySettings, JPH::EActivation::DontActivate);
    if (body.IsInvalid())
    {
        throw std::runtime_error("PhysicsWorld: out of bodies for static geometry");
    }
    m_impl->staticBodies.push_back(body);
    m_impl->broadPhaseDirty = true;
}

size_t PhysicsWorld::GetStaticBodyCount() const
{
    return m_impl->staticBodies.size();
}

size_t PhysicsWorld::GetStaticTriangleCount() const
{
    return m_impl->staticTriangleCount;
}

VehicleId PhysicsWorld::AddVehicle(const VehicleSettings& settings, const PhysicsPose& pose)
{
    Impl& impl = *m_impl;

    // The box sits where the settings put it in vehicle space, or the car's own body does (its boxes and
    // shell, BuildChassisParts); the centre of mass is moved from the shape's own to the body's and its
    // hubs' together (VehicleBodyMassProperties), the body's being chassisCenter + centerOfMassOffset.
    JPH::RefConst<JPH::Shape> chassis;
    const VehicleChassisParts parts = BuildChassisParts(settings);
    if (!parts.boxes.empty())
    {
        JPH::StaticCompoundShapeSettings compound;
        for (const VehicleChassisBox& part : parts.boxes)
        {
            const JPH::Vec3 half = JPH::Vec3::sMax(ToJolt(part.halfExtents), JPH::Vec3::sReplicate(0.01f));
            compound.AddShape(ToJolt(part.center), JPH::Quat::sIdentity(), new JPH::BoxShape(half, std::min(0.02f, half.ReduceMin() * 0.5f)));
        }
        if (parts.hull.size() >= 4)
        {
            JPH::Array<JPH::Vec3> points;
            points.reserve(parts.hull.size());
            for (const glm::vec3& point : parts.hull)
            {
                points.push_back(ToJolt(point));
            }
            const JPH::ConvexHullShapeSettings hull(points, 0.01f);
            if (const JPH::ShapeSettings::ShapeResult made = hull.Create(); made.IsValid())
            {
                compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), made.Get());
            }
        }
        const JPH::ShapeSettings::ShapeResult made = compound.Create();
        if (made.HasError())
        {
            throw std::runtime_error(std::string("PhysicsWorld: failed to build the vehicle's body: ") + made.GetError().c_str());
        }
        chassis = made.Get();
    }
    else
    {
        const JPH::Vec3 halfExtents = JPH::Vec3::sMax(ToJolt(settings.chassisHalfExtents), JPH::Vec3::sReplicate(0.05f));
        const JPH::RefConst<JPH::Shape> box = new JPH::BoxShape(halfExtents, std::min(0.05f, halfExtents.ReduceMin() * 0.5f));
        chassis = JPH::RotatedTranslatedShapeSettings(ToJolt(settings.chassisCenter), JPH::Quat::sIdentity(), box).Create().Get();
    }
    glm::vec3 centerOfMass{0.0f};
    const JPH::MassProperties massProperties = VehicleBodyMassProperties(settings, *chassis, centerOfMass);
    const JPH::OffsetCenterOfMassShapeSettings chassisShape(ToJolt(centerOfMass) - chassis->GetCenterOfMass(), chassis);
    const JPH::ShapeSettings::ShapeResult shape = chassisShape.Create();
    if (shape.HasError())
    {
        throw std::runtime_error(std::string("PhysicsWorld: failed to build the vehicle's chassis: ") + shape.GetError().c_str());
    }

    JPH::BodyCreationSettings bodySettings(
        shape.Get(), ToJoltPosition(pose.position), ToJolt(pose.rotation), JPH::EMotionType::Dynamic, ObjectLayers::kMoving);
    bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bodySettings.mMassPropertiesOverride.mMass = std::max(settings.massKg, 1.0f);
    if (HasInertiaBox(settings) || SprungMass(settings) < settings.massKg)
    {
        // The car's data gives the body's inertia as a uniform box (width, height, length) of its mass about
        // its centre of mass, not the collision box's; hubs of their own add theirs.
        bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        bodySettings.mMassPropertiesOverride = massProperties;
    }
    bodySettings.mLinearDamping = std::max(settings.linearDamping, 0.0f);
    // The physics engine would also take 0.05 of the body's turning each second, a yaw and roll damping
    // no car has: the tyres, dampers and air do that.
    bodySettings.mAngularDamping = 0.0f;
    // A fast car against a thin wall would otherwise pass through it between two steps.
    bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;

    Impl::Vehicle vehicle;
    vehicle.body = impl.physicsSystem.GetBodyInterface().CreateBody(bodySettings);
    if (vehicle.body == nullptr)
    {
        throw std::runtime_error("PhysicsWorld: out of bodies for the vehicle");
    }
    impl.physicsSystem.GetBodyInterface().AddBody(vehicle.body->GetID(), JPH::EActivation::Activate);

    vehicle.aeroSurfaces = settings.aeroSurfaces;
    vehicle.constraint = new JPH::VehicleConstraint(*vehicle.body, *BuildVehicleConstraintSettings(settings));
    {
        // The tyre's grip times the surface's ratio (as the game measures both), held to the surface's cap,
        // rather than the physics engine's square root of the two, which leaves ice with a third of tarmac's.
        const VehicleId id = static_cast<VehicleId>(impl.vehicles.size());
        vehicle.constraint->SetCombineFriction(
            [&impl, id](JPH::uint, float& ioLongitudinal, float& ioLateral, const JPH::Body& body, const JPH::SubShapeID&)
            {
                const SurfaceGrip grip = impl.GripOf(body);
                const float scale = Impl::SurfaceFriction(grip, impl.vehicles[id].body->GetLinearVelocity().Length());
                ioLongitudinal = Impl::CappedFriction(grip, ioLongitudinal * scale);
                ioLateral = Impl::CappedFriction(grip, ioLateral * scale);
            });
    }
    // Casting the wheels' cylinders rolls them over kerbs and seams a ray would catch on; they touch
    // the ground as discs (VehicleCollisionTesterDisc).
    vehicle.discTester = new VehicleCollisionTesterDisc(ObjectLayers::kMoving);
    vehicle.collisionTester = vehicle.discTester;
    vehicle.wheelFilter = std::make_unique<WheelBodyFilter>(vehicle.body->GetID());
    vehicle.collisionTester->SetBodyFilter(vehicle.wheelFilter.get());
    vehicle.constraint->SetVehicleCollisionTester(vehicle.collisionTester);
    impl.physicsSystem.AddConstraint(vehicle.constraint);
    impl.physicsSystem.AddStepListener(vehicle.constraint);

    // The wheels' settings were made above and are ours to change: the controller reads the brake torque
    // from them each step.
    for (size_t index = 0; index < kVehicleWheelCount && index < vehicle.constraint->GetWheels().size(); ++index)
    {
        auto* wheelSettings = const_cast<JPH::WheelSettingsWV*>(static_cast<const JPH::WheelWV*>(vehicle.constraint->GetWheels()[static_cast<JPH::uint>(index)])->GetSettings());
        vehicle.wheelSettings.push_back(wheelSettings);
        vehicle.staticBrakeTorque[index] = wheelSettings->mMaxBrakeTorque;
        if (index >= 2)
        {
            // The rear wheels' own forward axes, which rear-wheel steering turns from (ApplyRearSteer).
            vehicle.rearWheelForward[index - 2] = wheelSettings->mWheelForward;
        }
    }
    vehicle.dynamicBrakeBias = settings.dynamicBrakeBias;
    vehicle.tractionControlGrip = settings.useTractionControl ? std::max(settings.tractionControlGrip, 0.0f) : 0.0f;
    vehicle.defaultClutchStrength = settings.clutchStrength > 0.0f ? settings.clutchStrength : 10.0f;
    vehicle.limitedSlipLock = settings.limitedSlipDifferentials ? std::max(settings.limitedSlipLock, 0.0f) : 0.0f;
    vehicle.drive = settings.drive;
    vehicle.weightNewtons = std::max(settings.massKg, 1.0f) * 9.81f;
    {
        auto* controller = static_cast<JPH::WheeledVehicleController*>(vehicle.constraint->GetController());
        const JPH::VehicleTransmission& transmission = controller->GetTransmission();
        VehicleGearbox& gearbox = vehicle.gearbox;
        gearbox.forwardRatios.assign(transmission.mGearRatios.begin(), transmission.mGearRatios.end());
        gearbox.reverseRatio = transmission.mReverseGearRatios.empty() ? 0.0f : std::abs(transmission.mReverseGearRatios[0]);
        gearbox.shiftPoints = ComputeVehicleShiftPoints(settings);
        gearbox.idleRpm = controller->GetEngine().mMinRPM;
        gearbox.switchSeconds = transmission.mSwitchTime;
        gearbox.switchDownSeconds = std::max(settings.gearSwitchDownSeconds, 0.0f);
        gearbox.upshiftCutSeconds = std::max(settings.upshiftCutSeconds, 0.0f);
        gearbox.releaseSeconds = transmission.mClutchReleaseTime;
        gearbox.latencySeconds = transmission.mSwitchLatency;
        gearbox.limiterRpm = controller->GetEngine().mMaxRPM;
        // A launch rpm within the engine's range: no higher than 60 % of the way from the idle to the limiter.
        if (settings.launchRpm > 0.0f)
        {
            const float lowest = gearbox.idleRpm * 1.2f;
            const float highest = std::max(gearbox.idleRpm + 0.6f * std::max(settings.maxRpm - gearbox.idleRpm, 0.0f), lowest);
            gearbox.launchRpm = std::clamp(settings.launchRpm, lowest, highest);
        }
        // The driven wheels' radius and the final drive turn the car's speed into the gearbox output's.
        const JPH::VehicleDifferentialSettings& differential = controller->GetDifferentials().front();
        const float radius = static_cast<const JPH::WheelWV*>(vehicle.constraint->GetWheels()[static_cast<JPH::uint>(differential.mLeftWheel)])->GetSettings()->mRadius;
        vehicle.outputRpmPerSpeed = differential.mDifferentialRatio / std::max(radius, 0.01f) * JPH::VehicleEngine::cAngularVelocityToRPM;
        controller->GetTransmission().Set(vehicle.gearboxState.gear, vehicle.gearboxState.clutch);
    }
    vehicle.settings = settings;
    impl.BuildCorners(vehicle);
    if (settings.tyreModel == VehicleTyreModel::Brush)
    {
        for (size_t index = 0; index < kVehicleWheelCount; ++index)
        {
            vehicle.brushTyres.emplace_back(BuildBrushTyreParameters(settings, index));
            if (settings.tyreTemperatures && settings.tyres[index].thermal.surfaceTransfer > 0.0)
            {
                vehicle.thermals[index].emplace(settings.tyres[index].thermal, settings.tyreStartTemperature);
            }
            if (settings.tyreWear && !settings.tyres[index].wear.wearCurve.empty())
            {
                vehicle.wears[index].emplace(settings.tyres[index].wear);
            }
        }
        // The physics engine's tyre friction off: the brush tyres push instead.
        auto* controller = static_cast<JPH::WheeledVehicleController*>(vehicle.constraint->GetController());
        controller->SetTireMaxImpulseCallback(
            [](JPH::uint, float& outLongitudinalImpulse, float& outLateralImpulse, float, float, float, float, float, float)
            {
                outLongitudinalImpulse = 0.0f;
                outLateralImpulse = 0.0f;
            });
        const VehicleId id = static_cast<VehicleId>(impl.vehicles.size());
        vehicle.constraint->SetPostCollideCallback(
            [&impl, id](JPH::VehicleConstraint& constraint, const JPH::PhysicsStepListenerContext& context)
            {
                impl.ApplyBrushTyres(impl.vehicles[id], constraint, context);
            });
    }

    {
        JPH::StateRecorderImpl made;
        vehicle.constraint->SaveState(made);
        vehicle.madeConstraintState = made.GetData();
        for (const JPH::WheelSettingsWV* wheelSettings : vehicle.wheelSettings)
        {
            vehicle.madeWheelSettings.push_back(*wheelSettings);
        }
    }
    vehicle.current = impl.Capture(vehicle);
    vehicle.previous = vehicle.current;
    impl.vehicles.push_back(std::move(vehicle));
    return static_cast<VehicleId>(impl.vehicles.size() - 1);
}

void PhysicsWorld::SetVehicleControls(VehicleId id, const VehicleControls& controls)
{
    Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    vehicle.controls = controls;
    vehicle.pendingGearShifts += controls.gearShifts;
}

void PhysicsWorld::SetVehicleBrushTyreBristles(VehicleId id, int ribs, int segmentsPerRib)
{
    Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    vehicle.settings.brushTyreRibs = std::max(ribs, 0);
    vehicle.settings.brushTyreSegments = std::max(segmentsPerRib, 0);
    for (size_t index = 0; index < vehicle.brushTyres.size(); ++index)
    {
        vehicle.brushTyres[index] = tyre::BrushTyre(BuildBrushTyreParameters(vehicle.settings, index));
    }
}

void PhysicsWorld::SetVehicleDriverAids(VehicleId id, bool abs, bool tractionControl)
{
    // ApplyAntiLock, ApplyTractionControl and SetClutchStrength let go of what they hold at the next step.
    Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    vehicle.settings.useAbs = abs;
    vehicle.settings.useTractionControl = tractionControl;
    vehicle.tractionControlGrip = tractionControl ? std::max(vehicle.settings.tractionControlGrip, 0.0f) : 0.0f;
}

void PhysicsWorld::ResetVehicle(VehicleId id, const PhysicsPose& pose, bool keepTyres)
{
    Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    JPH::BodyInterface& bodies = m_impl->physicsSystem.GetBodyInterface();
    const JPH::BodyID body = vehicle.body->GetID();
    bodies.SetPositionAndRotation(body, ToJoltPosition(pose.position), ToJolt(pose.rotation), JPH::EActivation::Activate);
    bodies.SetLinearAndAngularVelocity(body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    // Nothing the car did before carries over: a replayed drive, given only the recording's controls,
    // drives the line it did only from the very state it started in. The contacts the body had are
    // forgotten, and the physics engine's side of the car goes back to as it was made.
    bodies.InvalidateContactCache(body);
    {
        JPH::StateRecorderImpl made;
        made.WriteBytes(vehicle.madeConstraintState.data(), vehicle.madeConstraintState.size());
        made.Rewind();
        vehicle.constraint->RestoreState(made);
    }
    for (size_t index = 0; index < vehicle.wheelSettings.size() && index < vehicle.madeWheelSettings.size(); ++index)
    {
        *vehicle.wheelSettings[index] = vehicle.madeWheelSettings[index];
    }
    if (vehicle.discTester != nullptr)
    {
        for (size_t index = 0; index < kVehicleWheelCount; ++index)
        {
            vehicle.discTester->SetFaceLength(index, 0.0f);
        }
    }
    for (JPH::Wheel* wheel : vehicle.constraint->GetWheels())
    {
        wheel->SetAngularVelocity(0.0f);
    }
    auto* controller = static_cast<JPH::WheeledVehicleController*>(vehicle.constraint->GetController());
    controller->GetEngine().SetCurrentRPM(controller->GetEngine().mMinRPM);
    controller->SetDriverInput(0.0f, 0.0f, 0.0f, 0.0f);
    controller->GetTransmission().mClutchStrength = vehicle.defaultClutchStrength;
    vehicle.controls = {};
    vehicle.direction = 1.0f;
    vehicle.gearboxState = {};
    vehicle.pendingGearShifts = 0;
    vehicle.turboBoost.clear();
    vehicle.pedal = 0.0f;
    vehicle.filteredLoad = {};
    vehicle.filteredLoadValid = false;
    vehicle.absReleased = {};
    vehicle.absClock = 0.0f;
    vehicle.tcCut = false;
    vehicle.tcClock = 0.0f;
    vehicle.centreCouplingTorque = 0.0f;
    vehicle.drivetrainMeshLoss = 0.0f;
    vehicle.drivetrainSpinLoss = 0.0f;
    vehicle.rearSteerFiltered.clear();
    vehicle.rearSteerAngle = 0.0f;
    vehicle.controllerLastVelocity = JPH::Vec3::sZero();
    vehicle.controllerLastVelocityValid = false;
    vehicle.axleTorqueReaction = {};
    vehicle.lastLinearVelocity = JPH::Vec3::sZero();
    vehicle.lastAngularVelocity = JPH::Vec3::sZero();
    vehicle.lastVelocityValid = false;
    // The tyres' bristles at rest; new tyres, cold and unworn, unless they are kept.
    for (size_t index = 0; index < vehicle.brushTyres.size(); ++index)
    {
        vehicle.brushTyres[index] = tyre::BrushTyre(BuildBrushTyreParameters(vehicle.settings, index));
        if (keepTyres)
        {
            continue;
        }
        if (vehicle.thermals[index].has_value())
        {
            vehicle.thermals[index].emplace(vehicle.settings.tyres[index].thermal, vehicle.settings.tyreStartTemperature);
        }
        if (vehicle.wears[index].has_value())
        {
            vehicle.wears[index].emplace(vehicle.settings.tyres[index].wear);
        }
    }
    vehicle.tyreSlips = {};
    vehicle.brushWheels = {};
    vehicle.droppedRotation = {};
    vehicle.spinAngleBefore = {};
    vehicle.submergedShare = 0.0f;
    vehicle.floodSeconds = 0.0f;
    vehicle.engineDrowned = false;
    vehicle.underWaterInTunnel = false;
    m_impl->BuildCorners(vehicle);
    vehicle.current = m_impl->Capture(vehicle);
    // Nothing has rolled: the capture compared the wheels with the step before the reset.
    for (VehicleWheelState& wheel : vehicle.current.wheels)
    {
        wheel.spinStep = 0.0f;
    }
    vehicle.previous = vehicle.current;
}

std::optional<double> PhysicsWorld::FindGroundBelow(const glm::dvec3& from, double maxDistance) const
{
    const JPH::RRayCast ray{ToJoltPosition(from), JPH::Vec3(0.0f, static_cast<float>(-maxDistance), 0.0f)};
    JPH::RayCastResult hit;
    const JPH::SpecifiedBroadPhaseLayerFilter broadPhaseFilter(BroadPhaseLayers::kStatic);
    const JPH::SpecifiedObjectLayerFilter objectFilter(ObjectLayers::kStatic);
    if (!m_impl->physicsSystem.GetNarrowPhaseQuery().CastRay(ray, hit, broadPhaseFilter, objectFilter))
    {
        return std::nullopt;
    }
    return from.y - static_cast<double>(hit.mFraction) * maxDistance;
}

PhysicsPose PhysicsWorld::GetVehiclePose(VehicleId id) const
{
    const Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    return Interpolate(vehicle.previous.chassis, vehicle.current.chassis, m_impl->Alpha());
}

VehicleLinkage PhysicsWorld::GetVehicleLinkage(VehicleId id) const
{
    const Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    VehicleLinkage linkage;
    if (vehicle.corners.size() < kVehicleWheelCount)
    {
        return linkage;
    }
    const JPH::RMat44 transform = vehicle.body->GetWorldTransform();
    for (size_t axle = 0; axle < 2; ++axle)
    {
        // The axle's frame from its left wheel's: that wheel's design centre is half a track to the left.
        const Impl::Vehicle::Corner& left = vehicle.corners[2 * axle];
        const Impl::Vehicle::Corner& right = vehicle.corners[2 * axle + 1];
        const double halfTrack = vehicle.axles[axle]->SketchHalfTrack(0.5 * std::abs(left.designCenter.x - right.designCenter.x));
        suspension::LinkageSketch sketch;
        vehicle.axles[axle]->Sketch(halfTrack, sketch);
        const auto toWorld = [&](const suspension::Vec3& p) {
            const glm::vec3 inVehicle = left.designCenter + CornerToVehicle(p - suspension::Vec3(0.0, halfTrack, 0.0));
            return FromJoltPosition(transform * ToJolt(inVehicle));
        };
        for (const auto& [a, b] : sketch.links)
        {
            linkage.links.push_back({toWorld(a), toWorld(b)});
        }
        for (const auto& [a, b] : sketch.carriers)
        {
            linkage.carriers.push_back({toWorld(a), toWorld(b)});
        }
        for (const suspension::Vec3& p : sketch.chassis)
        {
            linkage.chassis.push_back(toWorld(p));
        }
        for (const suspension::Vec3& p : sketch.joints)
        {
            linkage.joints.push_back(toWorld(p));
        }
    }
    return linkage;
}

std::vector<VehicleWheelState> PhysicsWorld::GetVehicleWheels(VehicleId id) const
{
    const Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    const float alpha = m_impl->Alpha();
    std::vector<VehicleWheelState> wheels = vehicle.current.wheels;
    for (size_t index = 0; index < wheels.size() && index < vehicle.previous.wheels.size(); ++index)
    {
        const VehicleWheelState& before = vehicle.previous.wheels[index];
        wheels[index].pose = Interpolate(before.pose, wheels[index].pose, alpha);
        // The roll runs on from where it was before the step by the step's share, straight through
        // however many turns that is.
        wheels[index].spinAngle -= (1.0f - alpha) * wheels[index].spinStep;
        wheels[index].pose.rotation = wheels[index].pose.rotation * glm::angleAxis(wheels[index].spinAngle, glm::vec3(1.0f, 0.0f, 0.0f));
        // The points drawn on the wheel move with it; the forces are the last step's.
        wheels[index].mount = glm::mix(before.mount, wheels[index].mount, static_cast<double>(alpha));
        if (before.inContact && wheels[index].inContact)
        {
            wheels[index].contactPosition = glm::mix(before.contactPosition, wheels[index].contactPosition, static_cast<double>(alpha));
        }
    }
    return wheels;
}

std::pair<glm::vec3, glm::vec3> PhysicsWorld::GetVehicleBodyBounds(VehicleId id) const
{
    const Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    const JPH::Shape& shape = *vehicle.body->GetShape();
    // The shape's local bounds are about its centre of mass; the body's origin is vehicle space's.
    const JPH::AABox bounds = shape.GetLocalBounds();
    const JPH::Vec3 centre = shape.GetCenterOfMass();
    return {FromJolt(bounds.mMin + centre), FromJolt(bounds.mMax + centre)};
}

VehicleTelemetry PhysicsWorld::GetVehicleTelemetry(VehicleId id) const
{
    const Impl::Vehicle& vehicle = m_impl->GetVehicle(id);
    const auto* controller = static_cast<const JPH::WheeledVehicleController*>(vehicle.constraint->GetController());

    VehicleTelemetry telemetry;
    const JPH::Vec3 localVelocity = vehicle.body->GetRotation().Conjugated() * vehicle.body->GetLinearVelocity();
    telemetry.forwardSpeed = localVelocity.GetZ();
    telemetry.rightSpeed = -localVelocity.GetX();
    const glm::vec3 frontAxle = 0.5f * (GetVehicleWheelMount(vehicle.settings, 0).center + GetVehicleWheelMount(vehicle.settings, 1).center);
    const JPH::Vec3 frontVelocity =
        vehicle.body->GetRotation().Conjugated() * vehicle.body->GetPointVelocity(vehicle.body->GetWorldTransform() * ToJolt(frontAxle));
    telemetry.frontAxleRightSpeed = -frontVelocity.GetX();
    telemetry.engineRpm = controller->GetEngine().GetCurrentRPM();
    telemetry.gear = controller->GetTransmission().GetCurrentGear();
    telemetry.clutch = vehicle.gearboxState.clutch;
    telemetry.manualHold = !vehicle.controls.manualGearbox && vehicle.gearboxState.manualHoldLeft > 0.0f;
    for (const JPH::Wheel* wheel : vehicle.constraint->GetWheels())
    {
        telemetry.wheelsInContact += wheel->HasContact() ? 1u : 0u;
    }
    telemetry.centreCouplingTorque = vehicle.centreCouplingTorque;
    telemetry.drivetrainMeshLossKw = vehicle.drivetrainMeshLoss * 0.001f;
    telemetry.drivetrainSpinLossKw = vehicle.drivetrainSpinLoss * 0.001f;
    telemetry.submergedShare = vehicle.submergedShare;
    telemetry.flooded = FloodedShare(vehicle.floodSeconds);
    telemetry.engineDrowned = vehicle.engineDrowned;
    telemetry.rearSteerDegrees = vehicle.rearSteerAngle * 180.0f / std::numbers::pi_v<float>;
    for (const float boost : vehicle.turboBoost)
    {
        telemetry.turboBoost += boost;
    }
    for (size_t index = 0; index < kVehicleWheelCount; ++index)
    {
        const std::optional<tyre::TyreThermalModel>& thermal = vehicle.thermals[index];
        if (!thermal)
        {
            continue;
        }
        VehicleTelemetry::TyreTemperatures& out = telemetry.tyres[index];
        out.simulated = true;
        // Lane 0 is the side camber to the right loads: a left wheel's inside, a right wheel's outside.
        const std::array<double, tyre::kThermalLanes> lanes = thermal->LaneTemperatures();
        const bool left = index % 2 == 0;
        out.tread = {static_cast<float>(left ? lanes[0] : lanes[2]), static_cast<float>(lanes[1]), static_cast<float>(left ? lanes[2] : lanes[0])};
        out.core = static_cast<float>(thermal->CoreTemperature());
        out.pressure = static_cast<float>(thermal->Pressure());
        out.grip = static_cast<float>(
            thermal->Performance() / (1.0 + std::abs(thermal->Pressure() - thermal->Parameters().idealPressure) * vehicle.settings.tyres[index].pressureGripGain));
    }
    for (size_t index = 0; index < kVehicleWheelCount; ++index)
    {
        if (const std::optional<tyre::TyreWearModel>& wear = vehicle.wears[index])
        {
            VehicleTelemetry::TyreWear& out = telemetry.wear[index];
            out.simulated = true;
            out.virtualKm = static_cast<float>(wear->VirtualKm());
            out.grain = static_cast<float>(wear->Grain());
            out.blister = static_cast<float>(wear->Blister());
            out.grip = static_cast<float>(wear->Grip());
        }
    }
    telemetry.absActive = std::ranges::any_of(vehicle.absReleased, [](bool released)
                                              {
                                                  return released;
                                              });
    telemetry.tractionControlCut = vehicle.tcCut;
    constexpr float kSlipMinSpeed = 2.0f;
    if (std::abs(telemetry.forwardSpeed) >= kSlipMinSpeed)
    {
        for (const VehicleWheelState& wheel : vehicle.current.wheels)
        {
            if (wheel.inContact)
            {
                telemetry.lockSlip = std::max(telemetry.lockSlip, -wheel.slipRatio);
                telemetry.spinSlip = std::max(telemetry.spinSlip, wheel.slipRatio);
            }
        }
    }
    return telemetry;
}

void PhysicsWorld::AddWaterSurface(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices)
{
    m_impl->water.Add(vertices, indices);
}

size_t PhysicsWorld::GetWaterTriangleCount() const
{
    return m_impl->water.TriangleCount();
}

void PhysicsWorld::SetStepSeconds(float seconds)
{
    Impl& impl = *m_impl;
    const float step = std::clamp(seconds, kMinStepSeconds, kMaxStepSeconds);
    // The carried remainder stays under a step, so the interpolation between the last two steps holds.
    impl.accumulatedSeconds = std::min(impl.accumulatedSeconds, step);
    impl.stepSeconds = step;
}

float PhysicsWorld::GetStepSeconds() const
{
    return m_impl->stepSeconds;
}

int PhysicsWorld::MaxStepsPerUpdate() const
{
    return std::max(static_cast<int>(std::lround(kMaxCatchUpSeconds / m_impl->stepSeconds)), 1);
}

int PhysicsWorld::Update(float deltaSeconds, float wallBudgetSeconds)
{
    const auto started = std::chrono::steady_clock::now();
    bool outOfTime = false;
    Impl& impl = *m_impl;
    if (impl.broadPhaseDirty)
    {
        // Static geometry added one body at a time leaves the broad phase tree unbalanced.
        impl.physicsSystem.OptimizeBroadPhase();
        impl.broadPhaseDirty = false;
    }

    const float step = impl.stepSeconds;
    const int maxSteps = MaxStepsPerUpdate();
    impl.accumulatedSeconds += std::max(deltaSeconds, 0.0f);
    int steps = 0;
    while (impl.accumulatedSeconds >= step && steps < maxSteps)
    {
        JPH::BodyInterface& bodies = impl.physicsSystem.GetBodyInterface();
        for (Impl::Vehicle& vehicle : impl.vehicles)
        {
            if (!impl.water.Empty())
            {
                impl.ApplyWater(vehicle, step);
            }
            const JPH::Vec3 localVelocity = vehicle.body->GetRotation().Conjugated() * vehicle.body->GetLinearVelocity();
            VehicleDriverInput input = vehicle.controls.manualGearbox
                                           ? ResolveManualDriverInput(vehicle.controls, vehicle.gearboxState.gear)
                                           : ResolveVehicleDriverInput(vehicle.controls, localVelocity.GetZ(), vehicle.direction);
            if (vehicle.engineDrowned)
            {
                input.forward = 0.0f;
            }
            impl.ShiftGears(vehicle, input, localVelocity.GetZ());
            impl.ApplyTractionControl(vehicle, input, localVelocity.GetZ());
            impl.ApplyAerodynamics(vehicle);
            impl.ApplySurfaceRollingResistance(vehicle);
            vehicle.drivetrainMeshLoss = 0.0f;
            vehicle.drivetrainSpinLoss = 0.0f;
            impl.ApplyDrivetrainSpin(vehicle);
            impl.DistributeBrakeTorque(vehicle);
            impl.ApplyAntiLock(vehicle, input.brake, localVelocity.GetZ());
            if (input.forward != 0.0f || input.right != 0.0f || input.brake != 0.0f || input.handBrake != 0.0f)
            {
                // A car that came to rest is asleep and would ignore the driver.
                bodies.ActivateBody(vehicle.body->GetID());
            }
            auto* controller = static_cast<JPH::WheeledVehicleController*>(vehicle.constraint->GetController());
            impl.ApplyCentreCoupling(vehicle, *controller);
            impl.CoupleDifferentialWheels(vehicle, *controller);
            impl.LimitClutchTorque(vehicle, *controller);
            impl.UpdateCorners(vehicle, input.right);
            impl.ApplyRearSteer(vehicle, input);
            controller->SetDriverInput(impl.SpoolTurbos(vehicle, *controller, input.forward), input.right, input.brake, input.handBrake);
            impl.ApplyEngineCoast(vehicle, *controller, input.forward);
            impl.ApplyDrivetrainLoss(vehicle, *controller);
            impl.RememberWheelAngles(vehicle);
        }

        const JPH::EPhysicsUpdateError error =
            impl.physicsSystem.Update(step, 1, &impl.tempAllocator, impl.jobSystem.get());
        if (error != JPH::EPhysicsUpdateError::None)
        {
            JoltTrace("PhysicsSystem::Update reported error flags 0x%x", static_cast<unsigned>(error));
        }

        for (Impl::Vehicle& vehicle : impl.vehicles)
        {
            impl.ApplyDroppedRotation(vehicle);
            impl.RollWheelsBySpinAfterStep(vehicle);
            vehicle.previous = std::move(vehicle.current);
            vehicle.current = impl.Capture(vehicle);
        }
        impl.accumulatedSeconds -= step;
        ++steps;
        if (wallBudgetSeconds > 0.0f && std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count() > wallBudgetSeconds)
        {
            outOfTime = true;
            break;
        }
    }
    if (steps == maxSteps || outOfTime)
    {
        impl.accumulatedSeconds = std::min(impl.accumulatedSeconds, step);
    }
    return steps;
}

int PhysicsWorld::RunSteps(int count, float carrySeconds)
{
    // Half a step over the count, so no rounding in the accumulator's subtractions can lose a step.
    const int steps = std::clamp(count, 0, MaxStepsPerUpdate());
    m_impl->accumulatedSeconds = (static_cast<float>(steps) + 0.5f) * m_impl->stepSeconds;
    const int ran = Update(0.0f);
    m_impl->accumulatedSeconds = std::clamp(carrySeconds, 0.0f, m_impl->stepSeconds);
    return ran;
}
}
