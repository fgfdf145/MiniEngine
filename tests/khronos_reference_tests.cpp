#include <engine/asset/model_loader.h>
#include <engine/renderer/camera.h>
#include <engine/renderer/exposure.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// The PBR Neutral operator the tone mapping pass runs, compiled from the same source.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/pbr_neutral.glsl>
}

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

// Khronos PBR Neutral as published (KhronosGroup/ToneMapping, PBR_Neutral), written out here
// independently of the shader.
glm::vec3 Reference(glm::vec3 color)
{
    const float startCompression = 0.8f - 0.04f;
    const float desaturation = 0.15f;
    const float x = std::min({color.r, color.g, color.b});
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    color -= offset;
    const float peak = std::max({color.r, color.g, color.b});
    if (peak < startCompression)
    {
        return color;
    }
    const float d = 1.0f - startCompression;
    const float newPeak = 1.0f - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    const float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return color * (1.0f - g) + glm::vec3(newPeak) * g;
}

bool Near(const glm::vec3& actual, const glm::vec3& expected, float tolerance = 1e-5f)
{
    return glm::all(glm::lessThan(glm::abs(actual - expected), glm::vec3(tolerance)));
}

std::string Text(const glm::vec3& value)
{
    return "(" + std::to_string(value.x) + ", " + std::to_string(value.y) + ", " + std::to_string(value.z) + ")";
}

void PbrNeutralMatchesKhronos()
{
    const glm::vec3 inputs[] = {
        glm::vec3(0.0f),
        glm::vec3(0.18f),
        glm::vec3(0.05f, 0.2f, 0.5f),
        glm::vec3(5.0f, 2.0f, 1.0f),
        glm::vec3(100.0f, 0.5f, 0.02f),
        glm::vec3(0.8f)};
    for (const glm::vec3& input : inputs)
    {
        const glm::vec3 port = shader::KhronosPbrNeutral(input);
        Require(Near(port, Reference(input)), "PBR Neutral of " + Text(input) + " is " + Text(port) + ", not " + Text(Reference(input)));
    }
    Require(Near(shader::KhronosPbrNeutral(glm::vec3(0.0f)), glm::vec3(0.0f)), "black is not black");
    Require(Near(shader::KhronosPbrNeutral(glm::vec3(0.18f)), glm::vec3(0.14f)), "mid grey is not offset by 0.04");
    const glm::vec3 bright = shader::KhronosPbrNeutral(glm::vec3(5.0f, 2.0f, 1.0f));
    Require(bright.r < 1.0f && bright.r > 0.9f, "a bright colour is not compressed below 1");
    Require(bright.b / bright.r > (1.0f - 0.04f) / (5.0f - 0.04f), "a bright colour is not desaturated");
    // Continuous where compression starts: the peak there maps to itself.
    const float start = 0.8f - 0.04f;
    const glm::vec3 atStart = shader::KhronosPbrNeutral(glm::vec3(start + 0.04f));
    Require(std::abs(atStart.r - start) < 1e-4f, "the curve jumps where compression starts");
}

// The Sample Viewer's rule: target the box centre, look down -Z, back off until the larger of the
// x and y extents fits the vertical FOV and the horizontal one (vertical FOV times aspect).
float ExpectedDistance(float width, float height, float aspect)
{
    const float yfov = glm::radians(45.0f);
    const float xfov = yfov * aspect;
    const float maxAxis = std::max(width, height);
    return std::max(maxAxis / 2.0f / std::tan(yfov / 2.0f), maxAxis / 2.0f / std::tan(xfov / 2.0f));
}

void FramesLikeTheSampleViewer()
{
    Camera camera;
    camera.FrameBoundsLikeKhronosViewer(glm::vec3(-1.0f), glm::vec3(1.0f), 1.5f);
    Require(std::abs(camera.fovDegrees - 45.0f) < 1e-5f, "the viewer's vertical FOV is 45 degrees");
    Require(Near(camera.GetForward(), glm::vec3(0.0f, 0.0f, -1.0f)), "the camera does not look down -Z");
    Require(Near(camera.position, glm::vec3(0.0f, 0.0f, ExpectedDistance(2.0f, 2.0f, 1.5f)), 1e-4f),
            "a unit cube is not framed from " + std::to_string(ExpectedDistance(2.0f, 2.0f, 1.5f)) + ": " + Text(camera.position));

    // The distance is measured from the centre; depth does not enter it.
    camera.FrameBoundsLikeKhronosViewer(glm::vec3(1.0f, 2.0f, -10.0f), glm::vec3(5.0f, 3.0f, 10.0f), 1.5f);
    Require(Near(camera.position, glm::vec3(3.0f, 2.5f, ExpectedDistance(4.0f, 1.0f, 1.5f)), 1e-4f),
            "a wide box is not framed by its width: " + Text(camera.position));

    // A narrow viewport: the horizontal FOV decides.
    camera.FrameBoundsLikeKhronosViewer(glm::vec3(0.0f), glm::vec3(1.0f, 3.0f, 1.0f), 0.5f);
    Require(Near(camera.position, glm::vec3(0.5f, 1.5f, 0.5f + ExpectedDistance(1.0f, 3.0f, 0.5f)), 1e-4f),
            "a tall box in a narrow view is not framed by the horizontal FOV: " + Text(camera.position));
    Require(camera.nearPlane > 0.0f && camera.nearPlane < ExpectedDistance(1.0f, 3.0f, 0.5f) - 1.0f, "the near plane clips the model");
    Require(camera.farPlane > ExpectedDistance(1.0f, 3.0f, 0.5f) + 3.0f, "the far plane clips the model");
}

// The viewer grows each primitive's box to the cube around its bounding sphere before taking the
// union (getExtentsFromAccessor), so two unit spheres side by side frame wider than their boxes.
void ExtentsFollowTheSampleViewer()
{
    LoadedModelData model;
    for (const float x : {-0.55f, 0.55f})
    {
        ModelSubmeshData submesh;
        submesh.viewerBoundsCenter = glm::vec3(x, 0.0f, 0.0f);
        submesh.viewerBoundsRadius = std::sqrt(3.0f) * 0.5f;
        model.submeshes.push_back(submesh);
    }
    glm::vec3 minBounds(0.0f);
    glm::vec3 maxBounds(0.0f);
    Require(ComputeKhronosViewerExtents(model, glm::mat4(1.0f), minBounds, maxBounds), "a model with submeshes has no extents");
    const float r = std::sqrt(3.0f) * 0.5f;
    Require(Near(minBounds, glm::vec3(-0.55f - r, -r, -r)) && Near(maxBounds, glm::vec3(0.55f + r, r, r)),
            "the extents are not the union of the spheres' cubes: " + Text(minBounds) + " " + Text(maxBounds));

    // A placed entity: the cubes' corners move with it.
    Require(ComputeKhronosViewerExtents(model, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f)), minBounds, maxBounds),
            "a placed model has no extents");
    Require(Near(minBounds, glm::vec3(-0.55f - r, 2.0f - r, -r)), "the extents do not follow the entity: " + Text(minBounds));

    Require(!ComputeKhronosViewerExtents(LoadedModelData{}, glm::mat4(1.0f), minBounds, maxBounds), "an empty model has extents");
}

// The viewer transforms the POSITION accessor's box by the node before taking its bounds, so a
// rotated node's box grows where the transformed vertices' own box would not.
void LoaderKeepsTheViewersPrimitiveBounds()
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "miniengine_khronos_reference_bounds";
    std::filesystem::create_directories(directory);
    {
        std::ofstream file(directory / "rotated.gltf");
        // A triangle (0,0,0) (1,0,0) (0,1,0) under a node turned 45 degrees about +Z.
        file << R"({ "asset": { "version": "2.0" },
          "buffers": [{ "uri": "rotated.bin", "byteLength": 42 }],
          "bufferViews": [
            { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
            { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
          ],
          "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0] },
            { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
          ],
          "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0 }, "indices": 1 }] }],
          "nodes": [{ "mesh": 0, "rotation": [0, 0, 0.38268343, 0.92387953] }],
          "scenes": [{ "nodes": [0] }], "scene": 0 })";
        std::ofstream buffer(directory / "rotated.bin", std::ios::binary);
        const std::array<float, 9> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const std::array<uint16_t, 3> indices = {0, 1, 2};
        buffer.write(reinterpret_cast<const char*>(positions.data()), static_cast<std::streamsize>(sizeof(positions)));
        buffer.write(reinterpret_cast<const char*>(indices.data()), static_cast<std::streamsize>(sizeof(indices)));
    }
    const LoadedModelData model = ModelLoader::LoadModel((directory / "rotated.gltf").string());
    std::filesystem::remove_all(directory);

    // The box's corners turned 45 degrees span x in [-0.707, 0.707] and y in [0, 1.414].
    const ModelSubmeshData& submesh = model.submeshes.at(0);
    Require(Near(submesh.viewerBoundsCenter, glm::vec3(0.0f, 0.70710678f, 0.0f), 1e-4f),
            "the viewer's box centre is " + Text(submesh.viewerBoundsCenter));
    Require(std::abs(submesh.viewerBoundsRadius - 1.0f) < 1e-4f,
            "the viewer's half-diagonal is " + std::to_string(submesh.viewerBoundsRadius) + ", not 1");
    Require(submesh.boundsRadius < submesh.viewerBoundsRadius - 0.1f, "the vertices' own bounds did not stay tighter");
    Require(glm::length(submesh.nodeScale - glm::vec3(1.0f)) < 1e-5f, "a rotated node has no scale");
}

// Node transforms are baked into the vertices, so a volume's thickness (KHR_materials_volume, in
// mesh units) needs the node's scale kept beside them, per axis as the viewer takes it.
void LoaderKeepsTheNodeScale()
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "miniengine_khronos_reference_scale";
    std::filesystem::create_directories(directory);
    {
        std::ofstream file(directory / "scaled.gltf");
        file << R"({ "asset": { "version": "2.0" },
          "buffers": [{ "uri": "scaled.bin", "byteLength": 42 }],
          "bufferViews": [
            { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
            { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
          ],
          "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0] },
            { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
          ],
          "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0 }, "indices": 1 }] }],
          "nodes": [{ "children": [1], "scale": [2, 2, 2] }, { "mesh": 0, "scale": [0.5, 1.5, 3], "rotation": [0, 0, 0.38268343, 0.92387953] }],
          "scenes": [{ "nodes": [0] }], "scene": 0 })";
        std::ofstream buffer(directory / "scaled.bin", std::ios::binary);
        const std::array<float, 9> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const std::array<uint16_t, 3> indices = {0, 1, 2};
        buffer.write(reinterpret_cast<const char*>(positions.data()), static_cast<std::streamsize>(sizeof(positions)));
        buffer.write(reinterpret_cast<const char*>(indices.data()), static_cast<std::streamsize>(sizeof(indices)));
    }
    const LoadedModelData model = ModelLoader::LoadModel((directory / "scaled.gltf").string());
    std::filesystem::remove_all(directory);
    const glm::vec3 scale = model.submeshes.at(0).nodeScale;
    Require(glm::length(scale - glm::vec3(1.0f, 3.0f, 6.0f)) < 1e-4f, "the node's world scale per axis is kept: " + Text(scale));
}

void ExposesHdriTexelOneToOne()
{
    for (const float intensity : {1.0f, 100.0f, 1000.0f, 30000.0f})
    {
        const float ev = KhronosReferenceEv100(intensity);
        const float exposed = intensity * ExposureFromEv100(ev);
        Require(std::abs(exposed - 1.0f) < 1e-4f, "an HDRI texel of 1 at intensity " + std::to_string(intensity) + " is exposed to " + std::to_string(exposed));
    }
}
}

int main()
{
    try
    {
        PbrNeutralMatchesKhronos();
        FramesLikeTheSampleViewer();
        ExtentsFollowTheSampleViewer();
        LoaderKeepsTheViewersPrimitiveBounds();
        LoaderKeepsTheNodeScale();
        ExposesHdriTexelOneToOne();
    }
    catch (const std::exception& error)
    {
        std::cerr << "Khronos reference tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Khronos reference tests passed\n";
    return 0;
}
