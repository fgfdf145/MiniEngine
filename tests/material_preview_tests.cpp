// The Material Editor's preview renderer (docs/design/2026-10-09-material-editor-redesign-design.md):
// shapes, ray traversal, materials as the scene converts them, and whole frames.
//
// MINIENGINE_PREVIEW_MODEL=<model.gltf> also renders that model at 960 x 640 and reports the times
// (hierarchy build, a pass, the frame); MINIENGINE_PREVIEW_SNAPSHOT_DIR=<dir> writes the frames as PNGs.

#include <engine/asset/model_loader.h>
#include <engine/core/threading/task_system.h>
#include <engine/editor/material_preview/material_preview_renderer.h>
#include <engine/editor/material_preview/material_preview_shapes.h>
#include <engine/editor/services/scene_renderables.h>

#include <stb_image_write.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

std::shared_ptr<const MeshData> ShapeMesh(MaterialPreviewShape shape, const glm::vec3& offset = glm::vec3(0.0f), float scale = 1.0f)
{
    MeshData mesh = BuildMaterialPreviewShapeMesh(shape);
    for (Vertex& vertex : mesh.vertices)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            vertex.position[axis] = vertex.position[axis] * scale + offset[axis];
        }
    }
    return std::make_shared<const MeshData>(std::move(mesh));
}

MaterialPreviewMaterial SolidMaterial(const glm::vec3& colour, float roughness = 0.5f, float metallic = 0.0f)
{
    MaterialPreviewMaterial material;
    material.material.baseColorFactor[0] = colour.r;
    material.material.baseColorFactor[1] = colour.g;
    material.material.baseColorFactor[2] = colour.b;
    material.material.surfaceFactors[0] = metallic;
    material.material.surfaceFactors[1] = roughness;
    return material;
}

glm::vec3 Pixel(const MaterialPreviewFrame& frame, uint32_t x, uint32_t y)
{
    const uint8_t* texel = &frame.rgba[(static_cast<size_t>(y) * frame.width + x) * 4];
    return glm::vec3(texel[0], texel[1], texel[2]) / 255.0f;
}

void WriteSnapshot(const MaterialPreviewFrame& frame, const char* name)
{
    const char* folder = std::getenv("MINIENGINE_PREVIEW_SNAPSHOT_DIR");
    if (folder == nullptr)
    {
        return;
    }
    std::filesystem::create_directories(folder);
    const std::filesystem::path path = std::filesystem::path(folder) / name;
    stbi_write_png(path.string().c_str(), static_cast<int>(frame.width), static_cast<int>(frame.height), 4, frame.rgba.data(), static_cast<int>(frame.width) * 4);
}

// Every shape's triangles face out (glTF's counter-clockwise front), and its tangent frame's
// bitangent runs against texture v, as the engine's tangent generation makes it.
void ShapesFaceOutward()
{
    for (const MaterialPreviewShape shape : {MaterialPreviewShape::Sphere, MaterialPreviewShape::Cube, MaterialPreviewShape::Cylinder})
    {
        const MeshData mesh = BuildMaterialPreviewShapeMesh(shape);
        Require(mesh.IsValid(), std::string(ToString(shape)) + " has triangles");
        size_t inward = 0;
        size_t counted = 0;
        for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3)
        {
            const auto position = [&](size_t corner)
            {
                const Vertex& vertex = mesh.vertices[mesh.indices[index + corner]];
                return glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]);
            };
            const glm::vec3 normal = glm::cross(position(1) - position(0), position(2) - position(0));
            if (glm::length(normal) < 1e-9f)
            {
                continue;
            }
            const glm::vec3 centroid = (position(0) + position(1) + position(2)) / 3.0f;
            ++counted;
            inward += glm::dot(normal, centroid) < 0.0f ? 1u : 0u;
        }
        Require(counted > 0 && inward == 0, std::string(ToString(shape)) + ": " + std::to_string(inward) + " of " + std::to_string(counted) + " triangles face in");
    }
    // On the sphere's equator, facing -Z: u runs toward +X, v down, so the bitangent points up.
    const MeshData sphere = BuildMaterialPreviewShapeMesh(MaterialPreviewShape::Sphere);
    for (const Vertex& vertex : sphere.vertices)
    {
        if (std::abs(vertex.position[1]) < 1e-4f && vertex.position[2] < -0.49f)
        {
            const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            const glm::vec3 tangent(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
            const glm::vec3 bitangent = glm::cross(normal, tangent) * vertex.tangent[3];
            Require(tangent.x > 0.99f && bitangent.y > 0.99f, "the sphere's tangent frame follows u to the right and v down");
            return;
        }
    }
    throw std::runtime_error("no equator vertex facing -Z on the sphere");
}

void RaysMeetTheNearestSurface()
{
    std::vector<MaterialPreviewMesh> meshes;
    meshes.push_back({ShapeMesh(MaterialPreviewShape::Sphere), 0, true, glm::vec3(1.0f)});
    meshes.push_back({ShapeMesh(MaterialPreviewShape::Cube, glm::vec3(0.0f, 0.0f, 2.0f), 0.5f), 1, true, glm::vec3(1.0f)});
    const std::shared_ptr<const MaterialPreviewGeometry> geometry = MaterialPreviewGeometry::Build(std::move(meshes));
    Require(geometry->TriangleCount() > 1000, "both meshes are in the hierarchy");

    const auto any = [](const MaterialPreviewCandidate&)
    {
        return true;
    };
    MaterialPreviewHit hit;
    // From +Z the cube (half size 0.2 at z = 2) comes first.
    Require(geometry->Intersect(Ray{glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)}, hit, any), "a ray down the axis hits");
    Require(hit.mesh == 1 && std::abs(hit.t - 2.8f) < 1e-3f, "the cube's face at z = 2.2 is nearest, t = " + std::to_string(hit.t));
    // Refusing the cube lets the ray on to the sphere (radius 0.5, tessellated a little inside it).
    Require(geometry->Intersect(Ray{glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)}, hit,
                                [](const MaterialPreviewCandidate& candidate)
                                {
                                    return candidate.mesh != 1;
                                }),
            "past the refused cube the sphere");
    Require(hit.mesh == 0 && hit.t > 4.49f && hit.t < 4.51f, "the sphere's front at z = 0.5, t = " + std::to_string(hit.t));
    Require(!geometry->Occluded(Ray{glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)}, any), "nothing above the sphere");
    Require(geometry->Occluded(Ray{glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)}, any), "the sphere below");
    Require(!geometry->Occluded(Ray{glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 0.0f, 1.0f}, any), "but not within a metre");
}

// The preview converts a model's material as the scene does: FillRenderSubmeshMaterial.
void MaterialsConvertAsTheSceneDoes()
{
    ModelMaterialData model;
    model.baseColor[0] = 0.2f;
    model.opacity = 0.5f;
    model.clearcoatFactor = 0.8f;
    model.clearcoatRoughnessFactor = 0.1f;
    model.sheenColorFactor[1] = 0.4f;
    model.baseColorTexturePath = "paint.png";
    model.emissiveColor[0] = 1.0f;
    model.emissiveIntensity = 25500.0f;
    CpuRenderSubmesh submesh;
    FillRenderSubmeshMaterial(submesh, model, true, glm::vec3(1.0f), [](const std::string& path)
                              {
                                  return path.empty() ? path : "C:/models/" + path;
                              });
    Require((submesh.material.shadingModel[0] & kShadingFlagClearcoat) != 0u && (submesh.material.shadingModel[0] & kShadingFlagSheen) != 0u,
            "the coat and the sheen are flagged");
    Require(std::abs(submesh.material.baseColorFactor[3] - 0.5f) < 1e-6f, "opacity folds into the base alpha");
    Require(submesh.textures.baseColor == "C:/models/paint.png", "relative texture paths resolve through the callback");
    Require(MaterialPreviewTexturePaths(submesh).size() == 2, "the base map and its blend graph twin are the paths to load, got " + std::to_string(MaterialPreviewTexturePaths(submesh).size()) + " " + MaterialPreviewTexturePaths(submesh).front());

    FillRenderSubmeshMaterial(submesh, model, false, glm::vec3(1.0f), [](const std::string& path)
                              {
                                  return path;
                              });
    Require(submesh.textures.baseColor.empty(), "a submesh without texture coordinates samples no map");

    MaterialPreviewTextureCache cache;
    const MaterialPreviewMaterial material = BuildMaterialPreviewMaterial(submesh, cache);
    MaterialPreviewSurfacePoint point;
    point.vertexNormal = glm::vec3(0.0f, 1.0f, 0.0f);
    const MaterialPreviewSurface surface = EvaluateMaterialPreviewSurface(material, point);
    Require(std::abs(surface.coatFactor - 0.8f) < 1e-6f && std::abs(surface.coatRoughness - 0.1f) < 1e-6f, "the coat reads back as the lighting pass reads it");
    Require(std::abs(surface.emissive.r - 1.0f) < 1e-4f, "emission of a white surface in daylight is one preview unit");
}

void TexturesFilterAndWrap()
{
    // A 4 x 2 checker of black and white columns.
    std::vector<uint8_t> pixels(4 * 2 * 4, 255);
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 4; x += 2)
        {
            uint8_t* texel = &pixels[(y * 4 + x) * 4];
            texel[0] = texel[1] = texel[2] = 0;
        }
    }
    const MaterialPreviewTexture texture = BuildMaterialPreviewTexture(4, 2, pixels.data());
    Require(texture.levels.size() == 3, "4 x 2, 2 x 1 and 1 x 1");
    TextureSampler nearest;
    nearest.magFilter = TextureFilter::Nearest;
    nearest.minFilter = TextureFilter::Nearest;
    Require(texture.Sample(glm::vec2(0.1f, 0.5f), 0.0f, nearest, false).r == 0.0f, "column 0 is black");
    Require(texture.Sample(glm::vec2(0.3f, 0.5f), 0.0f, nearest, false).r == 1.0f, "column 1 is white");
    Require(texture.Sample(glm::vec2(1.1f, 0.5f), 0.0f, nearest, false).r == 0.0f, "repeat wraps 1.1 to 0.1");
    Require(std::abs(texture.Sample(glm::vec2(0.5f, 0.5f), 2.0f, TextureSampler{}, false).r - 0.5f) < 0.01f, "the last level is the average");
    Require(std::abs(texture.Sample(glm::vec2(0.3f, 0.5f), 0.0f, nearest, true).r - 1.0f) < 1e-6f, "sRGB white decodes to 1");
    Require(std::abs(MaterialPreviewSrgbToLinear(128) - 0.2158605f) < 1e-5f, "sRGB 128 decodes to 0.2159");
}

struct Scene
{
    std::shared_ptr<const MaterialPreviewGeometry> geometry;
    std::shared_ptr<std::vector<MaterialPreviewMaterial>> materials;
};

Scene SphereScene(const MaterialPreviewMaterial& material)
{
    std::vector<MaterialPreviewMesh> meshes;
    meshes.push_back({ShapeMesh(MaterialPreviewShape::Sphere), 0, true, glm::vec3(1.0f)});
    Scene scene;
    scene.geometry = MaterialPreviewGeometry::Build(std::move(meshes));
    scene.materials = std::make_shared<std::vector<MaterialPreviewMaterial>>(1, material);
    return scene;
}

MaterialPreviewCamera FrontCamera()
{
    MaterialPreviewCamera camera;
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;
    camera.distance = 2.5f;
    return camera;
}

void FramesShowTheMaterial()
{
    MaterialPreviewSettings settings;
    settings.targetSamples = 8;
    settings.floor = false;
    constexpr uint32_t kSize = 96;

    MaterialPreviewRenderer red;
    const Scene redScene = SphereScene(SolidMaterial(glm::vec3(0.8f, 0.05f, 0.05f)));
    red.SetScene(redScene.geometry, redScene.materials);
    red.Finish(kSize, kSize, FrontCamera(), settings);
    const MaterialPreviewFrame& frame = red.Frame();
    Require(frame.width == kSize && frame.samples == 8, "the frame has its size and every sample");
    WriteSnapshot(frame, "preview_red_sphere.png");
    const glm::vec3 centre = Pixel(frame, kSize / 2, kSize / 2);
    Require(centre.r > 0.2f && centre.r > 2.0f * centre.g && centre.r > 2.0f * centre.b, "the sphere's middle is red");
    Require(red.SlotAt(0.5f, 0.5f) == 0 && red.SlotAt(0.01f, 0.01f) == -1, "the slots: the sphere's in the middle, none in the corner");
    const glm::vec3 corner = Pixel(frame, 1, 1);
    Require(std::abs(corner.r - corner.b) < 0.1f, "the studio's corner is a neutral grey");

    // A smooth metal reflects the studio's softbox: brighter highlights than the rough red.
    MaterialPreviewRenderer chrome;
    const Scene chromeScene = SphereScene(SolidMaterial(glm::vec3(0.95f), 0.05f, 1.0f));
    chrome.SetScene(chromeScene.geometry, chromeScene.materials);
    chrome.Finish(kSize, kSize, FrontCamera(), settings);
    WriteSnapshot(chrome.Frame(), "preview_chrome_sphere.png");
    float brightest = 0.0f;
    for (uint32_t y = 0; y < kSize; ++y)
    {
        for (uint32_t x = 0; x < kSize; ++x)
        {
            brightest = std::max(brightest, Pixel(chrome.Frame(), x, y).g);
        }
    }
    Require(brightest > 0.8f, "chrome mirrors the softbox: " + std::to_string(brightest));

    // A Mask material below its cut-off is not there: the background shows and no slot is drawn.
    MaterialPreviewMaterial cutOut = SolidMaterial(glm::vec3(0.8f, 0.05f, 0.05f));
    cutOut.alphaMode = MaterialAlphaMode::Mask;
    cutOut.material.baseColorFactor[3] = 0.2f;
    cutOut.material.alphaCutoff = 0.5f;
    MaterialPreviewRenderer masked;
    const Scene maskedScene = SphereScene(cutOut);
    masked.SetScene(maskedScene.geometry, maskedScene.materials);
    masked.Finish(kSize, kSize, FrontCamera(), settings);
    Require(masked.SlotAt(0.5f, 0.5f) == -1, "a cut-out sphere is not drawn");

    // The highlighted slot gets the selection colour round its edge.
    red.SetHighlightSlot(0);
    Require(red.TakeNewFrame(), "a new outline is a new frame");
    WriteSnapshot(red.Frame(), "preview_red_outline.png");
    int outlined = 0;
    for (uint32_t x = 0; x < kSize; ++x)
    {
        const glm::vec3 pixel = Pixel(red.Frame(), x, kSize / 2);
        outlined += pixel.r > 0.95f && pixel.g > 0.7f && pixel.b < 0.4f ? 1 : 0;
    }
    Require(outlined >= 2, "the outline crosses the middle row on both sides");
}

void ModelTimings()
{
    const char* path = std::getenv("MINIENGINE_PREVIEW_MODEL");
    if (path == nullptr)
    {
        return;
    }
    const auto now = []
    {
        return std::chrono::steady_clock::now();
    };
    const auto milliseconds = [](auto start, auto end)
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    };
    auto start = now();
    const auto model = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(path));
    std::cout << "loaded " << path << " in " << milliseconds(start, now()) << " ms\n";
    const std::filesystem::path directory = std::filesystem::path(path).parent_path();
    const auto resolve = [&](const std::string& texture)
    {
        return texture.empty() || std::filesystem::path(texture).is_absolute() ? texture : (directory / texture).lexically_normal().string();
    };
    std::vector<MaterialPreviewMesh> meshes;
    std::vector<CpuRenderSubmesh> submeshes;
    for (const ModelSubmeshData& submesh : model->submeshes)
    {
        meshes.push_back({std::shared_ptr<const MeshData>(model, &submesh.mesh), submesh.materialIndex, submesh.hasTexCoords, submesh.nodeScale});
        CpuRenderSubmesh render;
        FillRenderSubmeshMaterial(render, model->materials[std::min<size_t>(submesh.materialIndex, model->materials.size() - 1)], submesh.hasTexCoords, submesh.nodeScale, resolve);
        submeshes.push_back(std::move(render));
    }
    start = now();
    const std::shared_ptr<const MaterialPreviewGeometry> geometry = MaterialPreviewGeometry::Build(std::move(meshes));
    std::cout << "hierarchies over " << geometry->TriangleCount() << " triangles in " << milliseconds(start, now()) << " ms\n";

    start = now();
    MaterialPreviewTextureCache cache;
    for (const CpuRenderSubmesh& submesh : submeshes)
    {
        cache.Request(MaterialPreviewTexturePaths(submesh));
    }
    cache.WaitForAll();
    std::cout << cache.LoadedCount() << " textures in " << milliseconds(start, now()) << " ms\n";
    auto materials = std::make_shared<std::vector<MaterialPreviewMaterial>>();
    for (const CpuRenderSubmesh& submesh : submeshes)
    {
        materials->push_back(BuildMaterialPreviewMaterial(submesh, cache));
    }

    MaterialPreviewRenderer renderer;
    renderer.SetScene(geometry, materials);
    MaterialPreviewCamera camera;
    const glm::vec3 centre = 0.5f * (geometry->BoundsMin() + geometry->BoundsMax());
    const float radius = 0.5f * glm::length(geometry->BoundsMax() - geometry->BoundsMin());
    camera.target = centre;
    camera.distance = radius / std::sin(camera.verticalFov * 0.5f) * 0.9f;
    MaterialPreviewSettings settings;
    settings.targetSamples = 64;
    start = now();
    renderer.Finish(960, 640, camera, settings);
    const double total = milliseconds(start, now());
    std::cout << "64 samples at 960 x 640 in " << total << " ms (" << total / 64.0 << " ms a pass)\n";
    WriteSnapshot(renderer.Frame(), "preview_model.png");
    settings.environment = MaterialPreviewEnvironmentPreset::Daylight;
    renderer.Finish(960, 640, camera, settings);
    WriteSnapshot(renderer.Frame(), "preview_model_daylight.png");
}
}

int main()
{
    TaskSystem::Initialize();
    int result = 0;
    try
    {
        ShapesFaceOutward();
        RaysMeetTheNearestSurface();
        MaterialsConvertAsTheSceneDoes();
        TexturesFilterAndWrap();
        FramesShowTheMaterial();
        ModelTimings();
        std::cout << "material preview tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "material preview test failed: " << error.what() << '\n';
        result = 1;
    }
    TaskSystem::Shutdown();
    return result;
}
