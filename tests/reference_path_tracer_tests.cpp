#include <engine/renderer/reference_path_tracer.h>

#include <array>
#include <cmath>
#include <iostream>
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

// A scene of one mesh, one instance at the identity, material 0.
RayScene SingleMeshScene(const std::vector<glm::vec3>& positions, const std::vector<uint32_t>& indices)
{
    RayScene scene;
    AppendMesh(scene, BuildMeshBvh(positions, indices));
    const std::array<RayInstanceInput, 1> inputs = {RayInstanceInput{0, glm::mat4(1.0f), 0, 0}};
    BuildTopLevel(scene, inputs);
    return scene;
}

// The cube [-1, 1]^3, closed. Its material is double-sided, so the faces' winding does not matter.
RayScene InsideOfCube()
{
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
    const auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d)
    {
        const uint32_t base = static_cast<uint32_t>(positions.size());
        positions.insert(positions.end(), {a, b, c, d});
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    quad({-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1});
    quad({-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1});
    quad({-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1});
    quad({1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1});
    quad({1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1});
    quad({-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1});
    return SingleMeshScene(positions, indices);
}

// Inside a closed box that glows with Le and reflects a share a, radiance is Le / (1 - a) everywhere
// (the furnace), so that is the irradiance / pi at any point on its walls.
void FurnaceConverges()
{
    const RayScene scene = InsideOfCube();
    ReferenceMaterial material;
    material.albedo = glm::vec3(0.5f, 0.25f, 0.0f);
    material.emission = glm::vec3(1.0f);
    material.doubleSided = true;
    const std::array<ReferenceMaterial, 1> materials = {material};
    ReferenceSettings settings;
    settings.samples = 4096;
    settings.maxBounces = 64;
    const glm::vec3 expected = material.emission / (glm::vec3(1.0f) - material.albedo);
    const glm::vec3 result = ReferenceIndirectIrradiance(scene, materials, {}, glm::vec3(0.3f, -1.0f, 0.2f), glm::vec3(0.0f, 1.0f, 0.0f), settings, 7);
    for (int channel = 0; channel < 3; ++channel)
    {
        Require(std::abs(result[channel] / expected[channel] - 1.0f) < 0.03f,
                "furnace channel " + std::to_string(channel) + ": " + std::to_string(result[channel]) + " against " + std::to_string(expected[channel]));
    }
}

// A floor under an open sky: the point sees nothing but sky, so its irradiance / pi is the sky's
// radiance, and the sun's direct beam is not part of it.
void OpenFloorSeesOnlySky()
{
    const std::vector<glm::vec3> positions = {{-50, 0, 50}, {50, 0, 50}, {50, 0, -50}, {-50, 0, -50}};
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    const RayScene scene = SingleMeshScene(positions, indices);
    ReferenceMaterial material;
    material.albedo = glm::vec3(0.8f);
    const std::array<ReferenceMaterial, 1> materials = {material};
    const std::array<ReferenceLight, 1> sun = {ReferenceLight{glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)), glm::vec3(100000.0f)}};
    ReferenceSettings settings;
    settings.samples = 512;
    settings.skyRadiance = glm::vec3(2.0f, 3.0f, 4.0f);
    const glm::vec3 result = ReferenceIndirectIrradiance(scene, materials, sun, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), settings, 3);
    Require(glm::all(glm::lessThan(glm::abs(result - settings.skyRadiance), glm::vec3(1e-4f))), "an open floor sees the sky alone");

    const ReferenceSurface surface = ReferencePrimaryHit(scene, materials, glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
    Require(surface.valid && std::abs(surface.position.y) < 1e-5f && surface.normal.y > 0.99f, "the primary hit finds the floor");
    const ReferenceSurface below = ReferencePrimaryHit(scene, materials, glm::vec3(0.0f, -2.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    Require(!below.valid, "the back of a single-sided floor is not a surface to compare");
}

// A white floor and a white wall facing away from the sun, no sky: the wall's foot receives the lit
// floor's light. Near the floor it sees half of it, so its irradiance / pi approaches half the floor's
// radiance (albedo E cos / pi), a little more for the second bounce off the wall.
void WallFootSeesTheLitFloor()
{
    std::vector<glm::vec3> positions = {{-100, 0, 100}, {100, 0, 100}, {100, 0, -100}, {-100, 0, -100}};
    std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    // The wall at z = 0, facing +z, from the floor up to 100 m.
    positions.insert(positions.end(), {{-100, 0, 0}, {100, 0, 0}, {100, 100, 0}, {-100, 100, 0}});
    indices.insert(indices.end(), {4, 5, 6, 4, 6, 7});
    const RayScene scene = SingleMeshScene(positions, indices);
    ReferenceMaterial material;
    material.albedo = glm::vec3(0.5f);
    const std::array<ReferenceMaterial, 1> materials = {material};
    // The sun straight overhead lights the floor and grazes the wall.
    const std::array<ReferenceLight, 1> sun = {ReferenceLight{glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1000.0f)}};
    ReferenceSettings settings;
    settings.samples = 8192;
    const glm::vec3 result = ReferenceIndirectIrradiance(scene, materials, sun, glm::vec3(0.0f, 0.01f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), settings, 11);
    const float floorRadiance = 0.5f * 1000.0f / 3.14159265f;
    Require(result.x > 0.45f * floorRadiance && result.x < 0.6f * floorRadiance,
            "the wall's foot gets about half the floor's radiance: " + std::to_string(result.x) + " of " + std::to_string(floorRadiance));
}

// A sunlit floor away from the origin, reached by a camera ray the way the comparison reaches it: the
// point sees only black sky, so any indirect light would be the floor lighting itself through a ray
// that did not leave it.
void SunlitFloorDoesNotLightItself()
{
    const std::vector<glm::vec3> positions = {{-50, -2, 50}, {50, -2, 50}, {50, -2, -50}, {-50, -2, -50}};
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    const RayScene scene = SingleMeshScene(positions, indices);
    ReferenceMaterial material;
    material.albedo = glm::vec3(0.73f);
    const std::array<ReferenceMaterial, 1> materials = {material};
    const std::array<ReferenceLight, 1> sun = {ReferenceLight{glm::normalize(glm::vec3(0.0f, 0.966f, -0.259f)), glm::vec3(100000.0f)}};
    ReferenceSettings settings;
    settings.samples = 256;
    for (int trial = 0; trial < 64; ++trial)
    {
        const glm::vec3 camera(0.0f, 0.0f, 4.0f);
        const glm::vec3 target(-1.3f + 0.041f * static_cast<float>(trial), -2.0f, -5.0f - 0.037f * static_cast<float>(trial));
        const ReferenceSurface surface = ReferencePrimaryHit(scene, materials, camera, glm::normalize(target - camera));
        Require(surface.valid, "the camera ray finds the floor");
        const glm::vec3 result = ReferenceIndirectIrradiance(scene, materials, sun, surface.position, surface.normal, settings, 100 + trial);
        Require(result == glm::vec3(0.0f), "a floor under a black sky gets no indirect light: " + std::to_string(result.x));
    }
}
}

int main()
{
    try
    {
        FurnaceConverges();
        OpenFloorSeesOnlySky();
        WallFootSeesTheLitFloor();
        SunlitFloorDoesNotLightItself();
    }
    catch (const std::exception& error)
    {
        std::cerr << "reference path tracer tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "reference path tracer tests passed\n";
    return 0;
}
