#include <engine/asset/model_tyre.h>
#include <engine/renderer/tyre_deformation.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
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

constexpr float kRadius = 0.326f;
constexpr float kBead = 0.25f;
constexpr float kHalfWidth = 0.125f;
const glm::vec3 kCenter(0.5f, 0.33f, 1.2f);
const glm::vec3 kAxle(1.0f, 0.0f, 0.0f);

// A point of the wheel at angle (0 straight down, positive towards +Z), radius and place along the axle.
glm::vec3 WheelPoint(float angle, float radius, float axial)
{
    return kCenter + kAxle * axial + glm::vec3(0.0f, -std::cos(angle), std::sin(angle)) * radius;
}

// The wheel standing on flat ground (y = kCenter.y - kRadius + deflection) and pressed `deflection` into it.
TyreDeformation OnFlatGround(float deflection)
{
    TyreDeformation d;
    d.onGround = true;
    d.center = kCenter;
    d.axle = kAxle;
    d.outerRadius = kRadius;
    d.innerRadius = kBead;
    d.axialCenter = 0.0f;
    d.groundNormal = glm::vec3(0.0f, 1.0f, 0.0f);
    d.groundOffset = kCenter.y - kRadius + deflection;
    d.forward = glm::vec3(0.0f, 0.0f, 1.0f);
    d.left = glm::vec3(1.0f, 0.0f, 0.0f);
    d.bulge = kTyreBulge;
    d.smoothing = kTyreSmoothing * kRadius;
    return d;
}

void TestSmoothPositive()
{
    constexpr float width = 0.005f;
    float previous = 0.0f;
    for (int step = -200; step <= 200; ++step)
    {
        const float x = 0.0001f * static_cast<float>(step);
        const float y = TyreSmoothPositive(x, width);
        Require(y >= x - 1e-7f && y >= 0.0f, "the smooth max is never below x or 0");
        Require(y >= previous - 1e-7f, "the smooth max rises with x");
        previous = y;
    }
    // C1 at the joins: the slopes either side of x = +-width match.
    const float h = 1e-4f;
    const float slopeInside = (TyreSmoothPositive(width - h, width) - TyreSmoothPositive(width - 2.0f * h, width)) / h;
    Require(std::abs(slopeInside - 1.0f) < 0.05f, "slope 1 entering the straight part");
    Require(TyreSmoothPositive(-width + h, width) < 1e-6f, "slope 0 leaving zero");
}

void TestInactiveAndPacking()
{
    TyreDeformation d = OnFlatGround(0.02f);
    d.carcassForward = 0.003f;
    d.carcassLeft = -0.007f;
    d.carcassTwist = 0.01f;
    d.carcassBending = 2.0f;
    const TyreDeformation back = UnpackTyreDeformation(PackTyreDeformation(d));
    Require(back.onGround && back.center == d.center && back.outerRadius == d.outerRadius && back.innerRadius == d.innerRadius &&
                back.groundOffset == d.groundOffset && back.left == d.left && back.carcassLeft == d.carcassLeft &&
                back.carcassBending == d.carcassBending && back.smoothing == d.smoothing && back.bulge == d.bulge,
            "packing keeps every field");

    d.onGround = false;
    const glm::vec3 point = WheelPoint(0.0f, kRadius, 0.05f);
    Require(DeformTyrePoint(d, point) == point, "an inactive tyre keeps its shape");
    Require(!UnpackTyreDeformation(PackTyreDeformation(TyreDeformation{})).onGround, "the default packs as inactive");
}

void TestFlattenedOnGround()
{
    constexpr float deflection = 0.02f;
    const TyreDeformation d = OnFlatGround(deflection);
    const float ground = d.groundOffset;

    // Nothing of the tread ends below the ground, and the part that was under it lies on it.
    for (int a = -90; a <= 90; ++a)
    {
        const float angle = glm::radians(static_cast<float>(a));
        for (float axial : {-kHalfWidth, -0.05f, 0.0f, 0.05f, kHalfWidth})
        {
            const glm::vec3 moved = DeformTyrePoint(d, WheelPoint(angle, kRadius, axial));
            Require(moved.y >= ground - 1e-5f, "no tread point stays below the ground (angle " + std::to_string(a) + ")");
            const float below = ground - WheelPoint(angle, kRadius, axial).y;
            if (below > d.smoothing)
            {
                Require(std::abs(moved.y - ground) < 1e-5f, "a point under the ground lands on it");
            }
        }
    }
    // The top of the tyre and the bead stay put; the patch's middle comes up by the deflection.
    const glm::vec3 top = WheelPoint(glm::pi<float>(), kRadius, 0.0f);
    Require(glm::length(DeformTyrePoint(d, top) - top) < 1e-6f, "the top of the tyre stays");
    const glm::vec3 bead = WheelPoint(0.0f, kBead, kHalfWidth);
    Require(glm::length(DeformTyrePoint(d, bead) - bead) < 1e-6f, "the bead stays on the rim");
    const glm::vec3 bottom = DeformTyrePoint(d, WheelPoint(0.0f, kRadius, 0.0f));
    Require(std::abs(bottom.y - (WheelPoint(0.0f, kRadius, 0.0f).y + deflection)) < 1e-5f, "the patch rises by the deflection");

    // The sidewall bulges outward on both sides, by up to bulge times the tread's move.
    const float middle = 0.5f * (kRadius + kBead);
    const glm::vec3 outer = DeformTyrePoint(d, WheelPoint(0.0f, middle, kHalfWidth));
    const glm::vec3 inner = DeformTyrePoint(d, WheelPoint(0.0f, middle, -kHalfWidth));
    Require(outer.x - (kCenter.x + kHalfWidth) > 0.5f * kTyreBulge * deflection, "the outer sidewall bulges out");
    Require((kCenter.x - kHalfWidth) - inner.x > 0.5f * kTyreBulge * deflection, "the inner sidewall bulges out");
    Require(outer.x - (kCenter.x + kHalfWidth) < 1.01f * kTyreBulge * deflection, "the bulge stays within bulge times the deflection");
    // ... and squashes a quarter of the tread's move (h = 1/2, h^2 = 1/4) towards the axle.
    Require(std::abs((outer.y - WheelPoint(0.0f, middle, 0.0f).y) - 0.25f * deflection) < 1e-4f, "the sidewall squashes with the tread");
}

void TestNormals()
{
    const TyreDeformation d = OnFlatGround(0.025f);
    // The middle of the patch faces the ground; a point well round the wheel keeps its normal.
    TyreVertexFrame patch;
    patch.position = WheelPoint(0.0f, kRadius, 0.03f);
    patch.normal = glm::normalize(patch.position - kCenter - kAxle * 0.03f);
    patch.tangent = glm::vec3(0.0f, 0.0f, 1.0f);
    const TyreVertexFrame moved = DeformTyreVertex(d, patch);
    Require(glm::dot(moved.normal, glm::vec3(0.0f, -1.0f, 0.0f)) > 0.9999f, "the flattened patch faces the ground");
    Require(std::abs(glm::dot(moved.tangent, moved.normal)) < 1e-4f, "the tangent stays across the normal");

    TyreVertexFrame side;
    const float angle = glm::radians(120.0f);
    side.position = WheelPoint(angle, kRadius, 0.0f);
    side.normal = glm::vec3(0.0f, -std::cos(angle), std::sin(angle));
    side.tangent = glm::vec3(0.0f, std::sin(angle), std::cos(angle));
    const TyreVertexFrame still = DeformTyreVertex(d, side);
    Require(glm::dot(still.normal, side.normal) > 0.9999f && glm::length(still.position - side.position) < 1e-6f,
            "the tread far round the wheel is untouched");

    // Near the patch's edge the tread's normal turns between the round and the flat: no flip.
    for (int a = 0; a <= 60; ++a)
    {
        TyreVertexFrame vertex;
        const float at = glm::radians(static_cast<float>(a));
        vertex.position = WheelPoint(at, kRadius, 0.0f);
        vertex.normal = glm::vec3(0.0f, -std::cos(at), std::sin(at));
        vertex.tangent = glm::vec3(0.0f, std::sin(at), std::cos(at));
        Require(glm::dot(DeformTyreVertex(d, vertex).normal, vertex.normal) > 0.5f, "normals turn without flipping");
    }
}

void TestCarcass()
{
    TyreDeformation d = OnFlatGround(0.02f);
    d.carcassLeft = 0.01f;
    d.carcassForward = 0.004f;
    const glm::vec3 restBottom = WheelPoint(0.0f, kRadius, 0.0f);
    const glm::vec3 shifted = DeformTyrePoint(d, restBottom);
    const glm::vec3 flat = DeformTyrePoint(OnFlatGround(0.02f), restBottom);
    Require(std::abs((shifted.x - flat.x) - 0.01f) < 1e-5f, "the patch moves to the left with the carcass");
    Require(std::abs((shifted.z - flat.z) - 0.004f) < 1e-5f, "the patch moves forward with the carcass");
    const glm::vec3 top = WheelPoint(glm::pi<float>(), kRadius, 0.0f);
    Require(glm::length(DeformTyrePoint(d, top) - top) < 1e-6f, "the top of the tyre does not move with the carcass");
    const glm::vec3 bead = WheelPoint(0.0f, kBead, 0.0f);
    Require(glm::length(DeformTyrePoint(d, bead) - bead) < 1e-6f, "the bead does not move with the carcass");

    // Twisted: the patch's front goes left and its back right.
    TyreDeformation twisted = OnFlatGround(0.02f);
    twisted.carcassTwist = 0.05f;
    const float edge = glm::radians(12.0f);
    const glm::vec3 front = DeformTyrePoint(twisted, WheelPoint(edge, kRadius, 0.0f));
    const glm::vec3 back = DeformTyrePoint(twisted, WheelPoint(-edge, kRadius, 0.0f));
    Require(front.x > kCenter.x + 1e-4f && back.x < kCenter.x - 1e-4f, "a twisted carcass turns the patch");
}

// A closed tyre section (tread, shoulders, sidewalls, bead) lathed `segments` times about kAxle; the
// last column copies the first with its own UVs, as a seam.
MeshData LatheTyre(int segments)
{
    const std::vector<glm::vec2> profile = {
        {kRadius, -0.105f}, {kRadius, 0.105f}, {0.32f, 0.118f}, {0.30f, kHalfWidth}, {0.27f, kHalfWidth},
        {kBead, 0.12f}, {kBead, -0.12f}, {0.27f, -kHalfWidth}, {0.30f, -kHalfWidth}, {0.32f, -0.118f}};
    const int rows = static_cast<int>(profile.size());
    MeshData mesh;
    for (int column = 0; column <= segments; ++column)
    {
        // The seam's copy at exactly the first column's place.
        const float angle = glm::two_pi<float>() * static_cast<float>(column % segments) / static_cast<float>(segments);
        for (int row = 0; row < rows; ++row)
        {
            Vertex vertex{};
            const glm::vec3 position = WheelPoint(angle, profile[row].x, profile[row].y);
            const glm::vec3 radial(0.0f, -std::cos(angle), std::sin(angle));
            vertex.position[0] = position.x;
            vertex.position[1] = position.y;
            vertex.position[2] = position.z;
            vertex.normal[0] = radial.x;
            vertex.normal[1] = radial.y;
            vertex.normal[2] = radial.z;
            vertex.tangent[0] = 0.0f;
            vertex.tangent[1] = std::sin(angle);
            vertex.tangent[2] = std::cos(angle);
            vertex.tangent[3] = 1.0f;
            vertex.texCoord[0] = static_cast<float>(column) / static_cast<float>(segments);
            vertex.texCoord[1] = static_cast<float>(row) / static_cast<float>(rows);
            mesh.vertices.push_back(vertex);
        }
    }
    for (int column = 0; column < segments; ++column)
    {
        for (int row = 0; row < rows; ++row)
        {
            const uint32_t a = static_cast<uint32_t>(column * rows + row);
            const uint32_t b = static_cast<uint32_t>(column * rows + (row + 1) % rows);
            const uint32_t c = static_cast<uint32_t>((column + 1) * rows + row);
            const uint32_t d = static_cast<uint32_t>((column + 1) * rows + (row + 1) % rows);
            mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
        }
    }
    return mesh;
}

ModelTyreShape TyreShape()
{
    ModelTyreShape shape;
    shape.center = kCenter;
    shape.axle = kAxle;
    shape.outerRadius = kRadius;
    shape.innerRadius = kBead;
    shape.halfWidth = kHalfWidth;
    return shape;
}

void TestRefinement()
{
    MeshData mesh = LatheTyre(24);
    const size_t triangles = mesh.indices.size() / 3;
    const uint32_t cuts = RefineTyreMesh(mesh, TyreShape(), glm::radians(2.0f));
    Require(cuts == 8, "a 15 degree step is cut into 8 at 2 degrees, got " + std::to_string(cuts));
    Require(mesh.indices.size() / 3 == triangles * cuts * cuts, "each triangle becomes cuts^2");

    // Every new point lies on the surface of revolution: the tread's at the tyre's radius.
    for (const Vertex& vertex : mesh.vertices)
    {
        const glm::vec3 offset = glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]) - kCenter;
        const float radius = std::sqrt(offset.y * offset.y + offset.z * offset.z);
        Require(radius >= kBead - 1e-5f && radius <= kRadius + 1e-5f, "refined points stay between bead and tread");
        if (std::abs(offset.x) < 0.1f && radius > kRadius - 0.002f)
        {
            Require(std::abs(radius - kRadius) < 1e-5f, "the tread stays round");
        }
        const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        Require(std::abs(glm::length(normal) - 1.0f) < 1e-4f, "normals stay unit length");
    }

    // Watertight still: every edge, by the positions of its ends, is shared by exactly two triangles.
    using Key = std::tuple<long long, long long, long long>;
    const auto key = [&](uint32_t index)
    {
        const Vertex& vertex = mesh.vertices[index];
        return Key{std::llround(vertex.position[0] * 1e6), std::llround(vertex.position[1] * 1e6), std::llround(vertex.position[2] * 1e6)};
    };
    std::map<std::pair<Key, Key>, int> edges;
    for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3)
    {
        for (size_t corner = 0; corner < 3; ++corner)
        {
            Key a = key(mesh.indices[index + corner]);
            Key b = key(mesh.indices[index + (corner + 1) % 3]);
            if (b < a)
            {
                std::swap(a, b);
            }
            ++edges[{a, b}];
        }
    }
    for (const auto& [edge, count] : edges)
    {
        Require(count == 2, "a refined edge is shared by " + std::to_string(count) + " triangles, not 2: a crack");
    }
}

void TestPrepareModelTyres()
{
    LoadedModelData model;
    ModelWheelRig rig;
    for (ModelWheelRig::Corner& corner : rig.corners)
    {
        corner.center = kCenter;
        corner.radius = kRadius;
        corner.width = 2.0f * kHalfWidth;
    }
    rig.axle = kAxle;
    model.wheelRig = rig;

    ModelSubmeshData tyre;
    tyre.mesh = LatheTyre(48);
    tyre.wheelPart = ModelWheelPart::Wheel;
    tyre.name = "Tyre_LF1";
    // A rim: a disc reaching only its lip.
    ModelSubmeshData rim;
    rim.mesh = LatheTyre(48);
    for (Vertex& vertex : rim.mesh.vertices)
    {
        glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
        position = kCenter + (position - kCenter) * 0.78f;
        vertex.position[0] = position.x;
        vertex.position[1] = position.y;
        vertex.position[2] = position.z;
    }
    rim.wheelPart = ModelWheelPart::Wheel;
    ModelSubmeshData body;
    body.mesh = LatheTyre(8);
    model.submeshes = {tyre, rim, body};

    PrepareModelTyres(model);
    Require(model.submeshes[0].tyre.has_value() && model.submeshes[0].mesh.deformable, "the tyre is found");
    Require(std::abs(model.submeshes[0].tyre->innerRadius - kBead) < 1e-4f, "its bead is its innermost reach");
    Require(!model.submeshes[1].tyre.has_value() && !model.submeshes[1].mesh.deformable, "the rim is not a tyre");
    Require(!model.submeshes[2].tyre.has_value(), "the body is not a tyre");
    Require(model.submeshes[0].mesh.vertices.size() > tyre.mesh.vertices.size(), "the tyre is refined");
}
}

int main()
{
    try
    {
        TestSmoothPositive();
        TestInactiveAndPacking();
        TestFlattenedOnGround();
        TestNormals();
        TestCarcass();
        TestRefinement();
        TestPrepareModelTyres();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "tyre deformation tests passed\n";
    return 0;
}
