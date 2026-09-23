# Sky and Atmosphere Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every scene a sky: Hillaire 2020 physical atmosphere (sky, sun disk, sun transmittance, aerial perspective) or an HDRI environment map, selectable per scene.

**Architecture:** A scene-level `SceneEnvironment` (saved in the scene YAML) drives a CPU atmosphere model (`engine/renderer/atmosphere.*`) that fills an environment block appended to the camera UBO. `VulkanAtmosphere` owns four device-lifetime LUT images (transmittance, multiple scattering, sky-view, aerial perspective) written by compute before the scene passes and sampled through new set 0 bindings. The forward pass draws the sky between its opaque and blend items; the lighting pass and `triangle.frag` apply aerial perspective.

**Tech Stack:** C++20, Vulkan 1.x, GLSL (glslc), CMake + CTest, ImGui, yaml-cpp, stb_image_write.

**Spec:** `docs/superpowers/specs/2026-09-23-sky-atmosphere-design.md`

## Global Constraints

- Commit directly to `main`. Never stage `miniengine.settings.json` or `docs/superpowers/plans/2026-09-13-gbuffer-phase2-deferred-shading.md`.
- Configure: `cmake --preset vs2026-x64` (needed after adding source files or tests). Build: `cmake --build --preset vs2026-x64-debug --parallel`. Tests: `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`.
- Format: `git add` new files first (the check only sees tracked files), then `powershell -ExecutionPolicy Bypass -File scripts/check-format.ps1`; fix with `powershell -ExecutionPolicy Bypass -File scripts/format-code.ps1`. Run the check in its own command, never chained into a commit.
- Validation run: `out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 300` prints zero `[error]`/`[warning]` lines.
- Headers compiled into `miniengine_scene_pass_tests` (`render_target_layout.h`, `scene_pass_order.h`) stay ASCII and free of `common.h`. This plan does not touch them.
- Units: atmosphere in km with the planet centre at the origin and +Y up; world metres / 1000 = km; world y = 0 is the ground. Planet radius 6360 km, top 6460 km.
- LUT sizes: transmittance 256x64, multiple scattering 32x32, sky-view 192x108, aerial perspective 32x32x32; all `VK_FORMAT_R16G16B16A16_SFLOAT`, kept in `VK_IMAGE_LAYOUT_GENERAL`.
- Set 0 bindings: 3 transmittance LUT, 4 sky-view LUT, 5 aerial perspective volume, 6 HDRI equirectangular map; all combined image samplers, fragment stage.
- `EnvironmentMode` values: None 0, Atmosphere 1, Hdri 2 (they reach the shaders).
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

---

### Task 1: Viewport capture (`--capture`)

A verification tool the later tasks use: after `--frames N`, write the viewport (the tone mapped `SceneLdr` image of the last recorded frame) to a PNG.

**Files:**
- Create: `engine/renderer/vulkan/viewport_capture.h`, `engine/renderer/vulkan/viewport_capture.cpp`
- Modify: `engine/renderer/CMakeLists.txt`, `engine/renderer/rhi/backend.h`, `engine/renderer/vulkan/renderer.h`, `engine/renderer/vulkan/renderer.cpp`, `engine/renderer/vulkan/scene_render_targets.cpp:229`, `engine/application/editor_application.h`, `engine/application/editor_application.cpp`, `README.md`

**Interfaces:**
- Produces: `me::CaptureImageToPng(const ImageCaptureRequest&)`; `IRenderBackend::CaptureViewport(const std::filesystem::path&)`; CLI `--capture <file.png>`.

- [ ] **Step 1: The capture routine.** `engine/renderer/vulkan/viewport_capture.h`:

```cpp
#pragma once

#include "common.h"

#include <filesystem>

namespace me
{

struct ImageCaptureRequest
{
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    // R8G8B8A8_* or B8G8R8A8_*; the bytes are written as stored, so an _SRGB image yields
    // display-encoded PNG values.
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    // The layout the image is in, and is returned to, around the copy.
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

// Copies a colour image to host memory and writes it as an RGBA PNG. The caller makes sure the
// GPU is idle and the image was created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT. Throws
// std::runtime_error on failure.
void CaptureImageToPng(const ImageCaptureRequest& request, const std::filesystem::path& path);
}
```

`engine/renderer/vulkan/viewport_capture.cpp`:

```cpp
#include "viewport_capture.h"

#include "upload_batch.h"

#include <stb_image_write.h>

#include <cstring>
#include <stdexcept>
#include <vector>

namespace me
{

namespace
{
uint32_t FindHostVisibleMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter)
{
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
    {
        if ((typeFilter & (1u << index)) != 0 && (properties.memoryTypes[index].propertyFlags & wanted) == wanted)
        {
            return index;
        }
    }
    throw std::runtime_error("No host-visible memory for the viewport capture");
}

bool IsBgra(VkFormat format)
{
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
}

bool IsRgba(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM;
}

void TransitionForCopy(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);
}
}

void CaptureImageToPng(const ImageCaptureRequest& request, const std::filesystem::path& path)
{
    if (!IsBgra(request.format) && !IsRgba(request.format))
    {
        throw std::runtime_error("Viewport capture supports only 8-bit RGBA and BGRA images");
    }

    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(request.extent.width) * request.extent.height * 4;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = byteCount;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    CheckVulkan(vkCreateBuffer(request.device, &bufferInfo, nullptr, &buffer), "Failed to create the capture buffer");

    VkDeviceMemory memory = VK_NULL_HANDLE;
    try
    {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(request.device, buffer, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindHostVisibleMemoryType(request.physicalDevice, requirements.memoryTypeBits);
        CheckVulkan(vkAllocateMemory(request.device, &allocateInfo, nullptr, &memory), "Failed to allocate the capture buffer");
        CheckVulkan(vkBindBufferMemory(request.device, buffer, memory, 0), "Failed to bind the capture buffer");

        VulkanUploadBatch batch(request.device, request.queueFamily, request.queue);
        const VkCommandBuffer commandBuffer = batch.GetCommandBuffer();
        TransitionForCopy(commandBuffer, request.image, request.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {request.extent.width, request.extent.height, 1};
        vkCmdCopyImageToBuffer(commandBuffer, request.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        TransitionForCopy(commandBuffer, request.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, request.layout);
        batch.Flush();

        std::vector<uint8_t> pixels(static_cast<size_t>(byteCount));
        void* mapped = nullptr;
        CheckVulkan(vkMapMemory(request.device, memory, 0, byteCount, 0, &mapped), "Failed to map the capture buffer");
        std::memcpy(pixels.data(), mapped, pixels.size());
        vkUnmapMemory(request.device, memory);
        if (IsBgra(request.format))
        {
            for (size_t texel = 0; texel < pixels.size(); texel += 4)
            {
                std::swap(pixels[texel], pixels[texel + 2]);
            }
        }
        for (size_t texel = 3; texel < pixels.size(); texel += 4)
        {
            pixels[texel] = 255;
        }

        if (stbi_write_png(
                path.string().c_str(),
                static_cast<int>(request.extent.width),
                static_cast<int>(request.extent.height),
                4,
                pixels.data(),
                static_cast<int>(request.extent.width) * 4) == 0)
        {
            throw std::runtime_error("Failed to write '" + path.string() + "'");
        }
    }
    catch (...)
    {
        vkDestroyBuffer(request.device, buffer, nullptr);
        if (memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(request.device, memory, nullptr);
        }
        throw;
    }
    vkDestroyBuffer(request.device, buffer, nullptr);
    vkFreeMemory(request.device, memory, nullptr);
}
}
```

`stb_image_write`'s implementation is compiled into `engine_asset` (`gltf_model_loader.cpp`), which `engine_renderer` already links through `engine_render_core`. In `engine/renderer/CMakeLists.txt` add `vulkan/viewport_capture.cpp` and `vulkan/viewport_capture.h` to `engine_renderer` (alphabetical, after `vulkan/upload_batch.h`) and, after its `target_link_libraries`, add:

```cmake
target_include_directories(engine_renderer PRIVATE "${Stb_INCLUDE_DIR}")
```

- [ ] **Step 2: The LDR target can be a copy source.** `scene_render_targets.cpp`, the `ldr.usage` line becomes:

```cpp
    // Transfer source for --capture, which copies the viewport to a PNG.
    ldr.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
```

- [ ] **Step 3: Backend entry point.** `rhi/backend.h`: add `#include <filesystem>` and `#include <stdexcept>`, and to `IRenderBackend`:

```cpp
    // Writes the viewport of the last drawn frame to a PNG. For verification runs (--capture).
    virtual void CaptureViewport(const std::filesystem::path& path)
    {
        (void)path;
        throw std::runtime_error("This render backend cannot capture the viewport");
    }
```

`vulkan/renderer.h`: declare `void CaptureViewport(const std::filesystem::path& path) override;` next to `DrawFrame`, and add the member `std::optional<uint32_t> m_lastRecordedImageIndex;` (include `<optional>` if missing). In `renderer.cpp`, right after `m_commandContext->Submit(m_device->GetGraphicsQueue(), imageIndex);` in `DrawFrame`, add `m_lastRecordedImageIndex = imageIndex;`, include `"viewport_capture.h"`, and define:

```cpp
void VulkanRenderer::CaptureViewport(const std::filesystem::path& path)
{
    if (!m_lastRecordedImageIndex.has_value() || !m_sceneTargets)
    {
        throw std::runtime_error("No frame has been drawn to capture");
    }
    vkDeviceWaitIdle(m_device->GetHandle());

    ImageCaptureRequest request{};
    request.physicalDevice = m_device->GetPhysicalDevice();
    request.device = m_device->GetHandle();
    request.queueFamily = m_device->GetQueueFamilies().graphicsFamily.value();
    request.queue = m_device->GetGraphicsQueue();
    const uint32_t index = m_sceneTargets->ResolveIndex(RenderTargetId::SceneLdr, *m_lastRecordedImageIndex, 0);
    request.image = m_sceneTargets->GetImage(RenderTargetId::SceneLdr, index);
    request.format = m_sceneTargets->GetFormat(RenderTargetId::SceneLdr);
    request.extent = m_sceneTargets->GetExtent();
    // The ImGui pass sampled it last, so the tracker left it shader-read.
    request.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    CaptureImageToPng(request, path);
    LOG_INFO("Captured the viewport to '{}'", path.string());
}
```

(`SceneLdr` is indexed by swapchain image, so `ResolveIndex` ignores the frame slot argument for it; check `ResolveIndex` and pass the frame slot if it does not.)

- [ ] **Step 4: CLI.** `editor_application.h`: add `std::optional<std::string> capturePath;` to `EditorApplicationOptions` with the comment `// With --frames: the viewport of the last frame is written here as a PNG.` In `ParseArgs`, beside `--frames`:

```cpp
        if (argument == "--capture")
        {
            options.capturePath = ReadRequiredArgument(i, argc, argv, argument);
            continue;
        }
```

(match the surrounding branches' exact shape: if they use `else if` chains or `continue`, follow them). In `Run()`, after the loop and before `return 0;`:

```cpp
    if (m_options.capturePath.has_value())
    {
        renderer->CaptureViewport(*m_options.capturePath);
    }
```

Document `--capture` in the README's command-line options list next to `--frames` (one line: `--capture <file.png>`: 与 `--frames` 一起使用，退出前把最后一帧的视口（色调映射后的 LDR 图）保存为 PNG).

- [ ] **Step 5: Verify.** Configure, build, run `out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 300 --capture %TEMP%\capture_test.png`. Expected: exit 0, zero validation messages, the log line `Captured the viewport`, and the PNG shows the two-cube scene (open it with the Read tool). Full ctest passes; format check passes.

- [ ] **Step 6: Commit.**

```bash
git add engine/renderer/vulkan/viewport_capture.h engine/renderer/vulkan/viewport_capture.cpp engine/renderer/CMakeLists.txt engine/renderer/rhi/backend.h engine/renderer/vulkan/renderer.h engine/renderer/vulkan/renderer.cpp engine/renderer/vulkan/scene_render_targets.cpp engine/application/editor_application.h engine/application/editor_application.cpp README.md
git commit -m "feat(app): --capture writes the viewport to a PNG

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Scene environment data and serialization

**Files:**
- Create: `engine/scene/scene_environment.h`, `tests/scene_environment_tests.cpp`
- Modify: `engine/scene/CMakeLists.txt`, `engine/logic/editor_world.h`, `engine/logic/editor_scene.h`, `engine/logic/editor_scene.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `me::EnvironmentMode`, `me::AtmosphereSettings`, `me::HdriSettings`, `me::SceneEnvironment` (all with defaulted `operator==`); `SerializedSceneData::environment`; `IEditorWorld::GetEnvironment() const -> const SceneEnvironment&`, `IEditorWorld::SetEnvironment(const SceneEnvironment&)`; `me::kDefaultSunIlluminanceLux`.

- [ ] **Step 1: Write the failing test** `tests/scene_environment_tests.cpp`:

```cpp
#include <engine/logic/editor_world.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

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

// Every value is exact in binary floating point, so the YAML round trip can be compared with ==.
SceneEnvironment MakeEnvironment()
{
    SceneEnvironment environment{};
    environment.mode = EnvironmentMode::Hdri;
    environment.atmosphere.groundAlbedo = glm::vec3(0.25f, 0.5f, 0.125f);
    environment.atmosphere.rayleighDensityScale = 2.0f;
    environment.atmosphere.mieDensityScale = 0.5f;
    environment.atmosphere.mieAnisotropy = 0.75f;
    environment.atmosphere.ozoneDensityScale = 0.0f;
    environment.atmosphere.aerialPerspectiveDistanceScale = 100.0f;
    environment.atmosphere.sunAngularDiameterDegrees = 1.5f;
    environment.hdri.path = "C:/hdri/sky.exr";
    environment.hdri.uuid = "33333333-3333-4333-8333-333333333333";
    environment.hdri.intensity = 1500.0f;
    environment.hdri.rotationDegrees = -90.0f;
    return environment;
}

void RoundTripsThroughYaml()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    const SceneEnvironment environment = MakeEnvironment();
    world->SetEnvironment(environment);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_scene_environment_test.yaml";
    world->SaveSceneToFile(path.string());
    const SerializedSceneData loaded = LoadEditorSceneDataFromFile(path.string());
    std::filesystem::remove(path);
    Require(loaded.environment == environment, "the environment did not survive the YAML round trip");

    std::unique_ptr<IEditorWorld> other = CreateEditorWorld();
    other->ApplySceneData(loaded);
    Require(other->GetEnvironment() == environment, "ApplySceneData did not restore the environment");
}

void MissingNodeLoadsAsNone()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_scene_environment_legacy.yaml";
    {
        std::ofstream out(path, std::ios::trunc);
        out << "scene:\n  version: 3\nentities: []\nlights: []\n";
    }
    const SerializedSceneData loaded = LoadEditorSceneDataFromFile(path.string());
    std::filesystem::remove(path);
    Require(loaded.environment.mode == EnvironmentMode::None, "a scene without an environment node must load as None");
}

void StartupSceneHasAtmosphereAndSun()
{
    std::unique_ptr<IEditorWorld> world = CreateEditorWorld();
    world->CreateTwoCubeTestScene();
    Require(world->GetEnvironment().mode == EnvironmentMode::Atmosphere, "the startup scene uses the atmosphere");
    int directionalLights = 0;
    float sunIntensity = 0.0f;
    world->ForEachLight(
        [&](entt::entity, const TagComponent&, const TransformComponent&, const LightComponent& light)
        {
            if (light.type == LightType::Directional)
            {
                ++directionalLights;
                sunIntensity = light.intensity;
            }
        });
    Require(directionalLights == 1, "the startup scene has one sun");
    Require(sunIntensity == kDefaultSunIlluminanceLux, "the sun has the default illuminance");
}
}

int main()
{
    try
    {
        RoundTripsThroughYaml();
        MissingNodeLoadsAsNone();
        StartupSceneHasAtmosphereAndSun();
    }
    catch (const std::exception& error)
    {
        std::cerr << "scene environment tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "scene environment tests passed\n";
    return 0;
}
```

Register it in `tests/CMakeLists.txt` after the `miniengine_world_bounds_tests` block, same shape, linking `engine_logic`, test name `miniengine.scene_environment`, target `miniengine_scene_environment_tests`. (Check `ForEachLight`'s exact callback signature in `engine/scene/scene_world.h` and match it.)

- [ ] **Step 2: Run to verify it fails.** Configure and build. Expected: compile errors, `SceneEnvironment` and `GetEnvironment` unknown.

- [ ] **Step 3: The data.** `engine/scene/scene_environment.h`:

```cpp
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace me
{

// What fills the pixels no geometry covers, and whether the atmosphere tints the sun and fogs the
// scene. The values reach the shaders through the camera uniform block and must match the
// ENVIRONMENT_* constants in shaders/vulkan/atmosphere_common.glsl.
enum class EnvironmentMode : uint32_t
{
    // The flat viewport background, standing for no physical light.
    None = 0,
    Atmosphere = 1,
    Hdri = 2
};

// Top-of-atmosphere illuminance of the startup scene's sun, in lux.
inline constexpr float kDefaultSunIlluminanceLux = 120000.0f;

// The parts of Hillaire 2020's Earth atmosphere the editor exposes. Radii and base coefficients
// are fixed; see engine/renderer/atmosphere.h.
struct AtmosphereSettings
{
    glm::vec3 groundAlbedo{0.3f, 0.3f, 0.3f};
    // Multipliers on the base Rayleigh scattering, Mie scattering and extinction, and ozone
    // absorption coefficients.
    float rayleighDensityScale = 1.0f;
    float mieDensityScale = 1.0f;
    // Cornette-Shanks g.
    float mieAnisotropy = 0.8f;
    float ozoneDensityScale = 1.0f;
    // Multiplies every view distance before the aerial perspective lookup, so an editor-sized
    // scene can show the haze the real atmosphere only builds up over kilometres.
    float aerialPerspectiveDistanceScale = 1.0f;
    float sunAngularDiameterDegrees = 0.545f;

    bool operator==(const AtmosphereSettings&) const = default;
};

struct HdriSettings
{
    // An equirectangular .hdr or .exr, referenced like a model: path plus asset uuid.
    std::string path;
    std::string uuid;
    // cd/m^2 per unit of texel value.
    float intensity = 1000.0f;
    // Turns the map about +Y.
    float rotationDegrees = 0.0f;

    bool operator==(const HdriSettings&) const = default;
};

struct SceneEnvironment
{
    EnvironmentMode mode = EnvironmentMode::None;
    AtmosphereSettings atmosphere;
    HdriSettings hdri;

    bool operator==(const SceneEnvironment&) const = default;
};
}
```

Add `scene_environment.h` to `engine/scene/CMakeLists.txt` (after `scene_components.h`).

- [ ] **Step 4: World and YAML.** `editor_world.h`: `#include <engine/scene/scene_environment.h>`; `SerializedSceneData` gains `SceneEnvironment environment;` (after `gizmo`); `IEditorWorld` gains, after `GetSceneFilePath`:

```cpp
    // The scene's sky. Saved with the scene; a scene file without it loads as EnvironmentMode::None.
    virtual const SceneEnvironment& GetEnvironment() const = 0;
    virtual void SetEnvironment(const SceneEnvironment& environment) = 0;
```

`editor_scene.h`: the two overrides and a member `SceneEnvironment m_environment;`. `editor_scene.cpp`:

```cpp
const SceneEnvironment& EditorScene::GetEnvironment() const
{
    return m_environment;
}

void EditorScene::SetEnvironment(const SceneEnvironment& environment)
{
    m_environment = environment;
}
```

In `ApplySceneData`, next to `m_gizmoSettings = sceneData.gizmo;`: `m_environment = sceneData.environment;`. In `CaptureSceneData`, next to `sceneData.gizmo = m_gizmoSettings;`: `sceneData.environment = m_environment;`.

In the anonymous namespace, before `ReadSceneData`:

```cpp
const char* EnvironmentModeToString(EnvironmentMode mode)
{
    switch (mode)
    {
    case EnvironmentMode::Atmosphere:
        return "atmosphere";
    case EnvironmentMode::Hdri:
        return "hdri";
    case EnvironmentMode::None:
        break;
    }
    return "none";
}

EnvironmentMode EnvironmentModeFromString(const std::string& text)
{
    if (text == "atmosphere")
    {
        return EnvironmentMode::Atmosphere;
    }
    if (text == "hdri")
    {
        return EnvironmentMode::Hdri;
    }
    return EnvironmentMode::None;
}

// A missing node, from a scene saved before environments existed, reads as the default: None.
SceneEnvironment ReadEnvironment(const YAML::Node& node)
{
    SceneEnvironment environment{};
    if (!node || !node.IsMap())
    {
        return environment;
    }
    environment.mode = EnvironmentModeFromString(node["mode"].as<std::string>("none"));

    const YAML::Node atmosphereNode = node["atmosphere"];
    AtmosphereSettings& atmosphere = environment.atmosphere;
    atmosphere.groundAlbedo = ReadVec3(atmosphereNode["ground_albedo"], atmosphere.groundAlbedo);
    atmosphere.rayleighDensityScale = atmosphereNode["rayleigh_density_scale"].as<float>(atmosphere.rayleighDensityScale);
    atmosphere.mieDensityScale = atmosphereNode["mie_density_scale"].as<float>(atmosphere.mieDensityScale);
    atmosphere.mieAnisotropy = atmosphereNode["mie_anisotropy"].as<float>(atmosphere.mieAnisotropy);
    atmosphere.ozoneDensityScale = atmosphereNode["ozone_density_scale"].as<float>(atmosphere.ozoneDensityScale);
    atmosphere.aerialPerspectiveDistanceScale =
        atmosphereNode["aerial_perspective_distance_scale"].as<float>(atmosphere.aerialPerspectiveDistanceScale);
    atmosphere.sunAngularDiameterDegrees =
        atmosphereNode["sun_angular_diameter_degrees"].as<float>(atmosphere.sunAngularDiameterDegrees);

    const YAML::Node hdriNode = node["hdri"];
    HdriSettings& hdri = environment.hdri;
    hdri.path = hdriNode["path"].as<std::string>(hdri.path);
    hdri.uuid = hdriNode["uuid"].as<std::string>(hdri.uuid);
    hdri.intensity = hdriNode["intensity"].as<float>(hdri.intensity);
    hdri.rotationDegrees = hdriNode["rotation_degrees"].as<float>(hdri.rotationDegrees);
    return environment;
}

void EmitEnvironment(YAML::Emitter& emitter, const SceneEnvironment& environment)
{
    emitter << YAML::Key << "environment" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "mode" << YAML::Value << EnvironmentModeToString(environment.mode);
    emitter << YAML::Key << "atmosphere" << YAML::Value << YAML::BeginMap;
    const AtmosphereSettings& atmosphere = environment.atmosphere;
    EmitVec3(emitter, "ground_albedo", atmosphere.groundAlbedo);
    emitter << YAML::Key << "rayleigh_density_scale" << YAML::Value << atmosphere.rayleighDensityScale;
    emitter << YAML::Key << "mie_density_scale" << YAML::Value << atmosphere.mieDensityScale;
    emitter << YAML::Key << "mie_anisotropy" << YAML::Value << atmosphere.mieAnisotropy;
    emitter << YAML::Key << "ozone_density_scale" << YAML::Value << atmosphere.ozoneDensityScale;
    emitter << YAML::Key << "aerial_perspective_distance_scale" << YAML::Value << atmosphere.aerialPerspectiveDistanceScale;
    emitter << YAML::Key << "sun_angular_diameter_degrees" << YAML::Value << atmosphere.sunAngularDiameterDegrees;
    emitter << YAML::EndMap;
    emitter << YAML::Key << "hdri" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "path" << YAML::Value << environment.hdri.path;
    emitter << YAML::Key << "uuid" << YAML::Value << environment.hdri.uuid;
    emitter << YAML::Key << "intensity" << YAML::Value << environment.hdri.intensity;
    emitter << YAML::Key << "rotation_degrees" << YAML::Value << environment.hdri.rotationDegrees;
    emitter << YAML::EndMap;
    emitter << YAML::EndMap;
}
```

In `ReadSceneData`, after the gizmo line: `sceneData.environment = ReadEnvironment(root["environment"]);`. In `EmitSceneYaml`, after the `lights` sequence and before `editor`: `EmitEnvironment(emitter, sceneData.environment);`. (`ReadVec3`/`EmitVec3` already exist in this file; if `ReadVec3` rejects an undefined node differently from the others, follow its signature.)

- [ ] **Step 5: The startup scene gets a sky and a sun.** In `EditorScene::CreateTwoCubeTestScene`, after the two `CreateEntity` calls and before `EnsureSelection()`:

```cpp
    // The atmosphere needs a sun to light it: 35 degrees up, behind the default camera, which looks
    // down -Z. A directional light shines along its local -Y; 55 degrees about X tips that toward -Z.
    // (Y is applied before X in BuildLightRotation, so it cannot turn a -Y light; azimuth would need
    // Z.)
    SerializedLightData sun{};
    sun.tagName = "Sun";
    sun.lightType = LightType::Directional;
    sun.intensity = kDefaultSunIlluminanceLux;
    sun.transform.translation = glm::vec3(0.0f, 4.0f, 0.0f);
    sun.transform.rotationDegrees = glm::vec3(55.0f, 0.0f, 0.0f);
    CreateLightEntity(sun);

    m_environment = SceneEnvironment{};
    m_environment.mode = EnvironmentMode::Atmosphere;
```

If `EnsureSelection` would now select the light first, keep the previous behaviour by selecting the first cube explicitly (check what `EnsureSelection` picks).

- [ ] **Step 6: Run to verify it passes.** Build; `ctest ... -R "scene_environment|scene_identity" --output-on-failure`: PASS. Validation run with zero messages. Format check.

- [ ] **Step 7: Commit.**

```bash
git add engine/scene/scene_environment.h engine/scene/CMakeLists.txt engine/logic/editor_world.h engine/logic/editor_scene.h engine/logic/editor_scene.cpp tests/scene_environment_tests.cpp tests/CMakeLists.txt
git commit -m "feat(scene): scene environment with atmosphere and HDRI settings

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: CPU atmosphere model and environment uniform data

**Files:**
- Create: `engine/renderer/atmosphere.h`, `engine/renderer/atmosphere.cpp`, `tests/atmosphere_tests.cpp`
- Modify: `engine/renderer/CMakeLists.txt` (engine_render_core), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `SceneEnvironment`, `AtmosphereSettings`, `EnvironmentMode` (Task 2).
- Produces: `me::AtmosphereParameters`, `me::BuildAtmosphereParameters(const AtmosphereSettings&)`, `me::ComputeExtinction(const AtmosphereParameters&, float altitudeKm) -> glm::vec3`, `me::ComputeTransmittanceToSpace(const AtmosphereParameters&, float altitudeKm, float cosZenith) -> glm::vec3`, `me::ToAtmosphereCameraPositionKm(const AtmosphereParameters&, const glm::vec3& cameraPositionMeters) -> glm::vec3`, `me::AtmosphereSun { glm::vec3 directionToSun; glm::vec3 illuminance; }`, `me::EnvironmentUniformData` (9 x vec4, 144 bytes), `me::BuildEnvironmentUniformData(EnvironmentMode mode, const SceneEnvironment&, const AtmosphereParameters&, const std::optional<AtmosphereSun>&, const glm::vec3& cameraPositionMeters) -> EnvironmentUniformData`, constants `kOzoneCenterAltitudeKm`, `kOzoneHalfWidthKm`.

- [ ] **Step 1: Write the failing test** `tests/atmosphere_tests.cpp`:

```cpp
#include <engine/renderer/atmosphere.h>

#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

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

std::string Text(const glm::vec3& value)
{
    return "(" + std::to_string(value.x) + ", " + std::to_string(value.y) + ", " + std::to_string(value.z) + ")";
}

// Looking straight up from the ground the optical depth has a closed form: each exponential layer
// contributes coefficient * scale height (the 100 km column holds all but e^-12.5 of it) and the
// ozone tent contributes coefficient * its 15 km area.
void ZenithTransmittanceMatchesOpticalDepth()
{
    const AtmosphereParameters p = BuildAtmosphereParameters(AtmosphereSettings{});
    const glm::vec3 opticalDepth =
        p.rayleighScattering * p.rayleighScaleHeightKm +
        glm::vec3(p.mieExtinction * p.mieScaleHeightKm) +
        p.ozoneAbsorption * kOzoneHalfWidthKm;
    const glm::vec3 expected = glm::exp(-opticalDepth);
    const glm::vec3 actual = ComputeTransmittanceToSpace(p, 0.0f, 1.0f);
    for (int channel = 0; channel < 3; ++channel)
    {
        Require(std::fabs(actual[channel] - expected[channel]) < 0.005f * expected[channel],
                "zenith transmittance " + Text(actual) + ", expected " + Text(expected));
    }
    Require(std::fabs(actual.r - 0.940f) < 0.005f && std::fabs(actual.g - 0.868f) < 0.005f && std::fabs(actual.b - 0.762f) < 0.005f,
            "zenith transmittance " + Text(actual) + " is not the spec's (0.940, 0.868, 0.762)");
}

void LowSunIsDimmerAndRedder()
{
    const AtmosphereParameters p = BuildAtmosphereParameters(AtmosphereSettings{});
    glm::vec3 previous = ComputeTransmittanceToSpace(p, 0.0f, 1.0f);
    for (float elevation = 85.0f; elevation >= 1.0f; elevation -= 1.0f)
    {
        const glm::vec3 current = ComputeTransmittanceToSpace(p, 0.0f, std::sin(glm::radians(elevation)));
        Require(glm::all(glm::lessThanEqual(current, previous + glm::vec3(1e-6f))),
                "transmittance rose as the sun lowered to " + std::to_string(elevation) + " degrees");
        previous = current;
    }
    const glm::vec3 low = ComputeTransmittanceToSpace(p, 0.0f, std::sin(glm::radians(2.0f)));
    Require(low.b < low.g && low.g < low.r, "a 2 degree sun must be reddened, got " + Text(low));
}

void EdgeCases()
{
    const AtmosphereParameters p = BuildAtmosphereParameters(AtmosphereSettings{});
    const float top = p.topRadiusKm - p.bottomRadiusKm;
    Require(glm::all(glm::greaterThan(ComputeTransmittanceToSpace(p, top, 1.0f), glm::vec3(0.999999f))),
            "from the top of the atmosphere looking up nothing is in the way");
    Require(ComputeTransmittanceToSpace(p, 0.0f, -0.5f) == glm::vec3(0.0f), "a ray into the ground reaches no sky");

    AtmosphereSettings empty{};
    empty.rayleighDensityScale = 0.0f;
    empty.mieDensityScale = 0.0f;
    empty.ozoneDensityScale = 0.0f;
    Require(ComputeTransmittanceToSpace(BuildAtmosphereParameters(empty), 0.0f, 0.01f) == glm::vec3(1.0f),
            "an empty atmosphere is transparent");
}

void BuildsUniformData()
{
    SceneEnvironment environment{};
    environment.mode = EnvironmentMode::Atmosphere;
    environment.hdri.rotationDegrees = 90.0f;
    environment.hdri.intensity = 2000.0f;
    const AtmosphereParameters p = BuildAtmosphereParameters(environment.atmosphere);
    AtmosphereSun sun{};
    sun.directionToSun = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f));
    sun.illuminance = glm::vec3(100000.0f);

    const EnvironmentUniformData data =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, sun, glm::vec3(0.0f, -10.0f, 0.0f));
    Require(data.sunDirectionAndMode.w == 1.0f, "the mode is carried in w");
    Require(glm::length(glm::vec3(data.sunDirectionAndMode) - sun.directionToSun) < 1e-6f, "the sun direction is carried");
    Require(glm::vec3(data.sunIlluminance) == sun.illuminance, "the sun illuminance is carried");
    Require(std::fabs(data.sunIlluminance.w - std::cos(glm::radians(0.545f * 0.5f))) < 1e-7f, "w is cos of the sun's angular radius");
    const float altitude = glm::length(glm::vec3(data.cameraPositionKm)) - p.bottomRadiusKm;
    Require(altitude >= 0.0f && altitude < 0.002f, "a camera below the ground is lifted to it, got " + std::to_string(altitude));
    Require(data.hdriParameters.x == 2000.0f && data.hdriParameters.y == 0.25f, "HDRI intensity and rotation in turns");

    const EnvironmentUniformData high =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, sun, glm::vec3(0.0f, 200000.0f, 0.0f));
    const float highAltitude = glm::length(glm::vec3(high.cameraPositionKm)) - p.bottomRadiusKm;
    Require(std::fabs(highAltitude - 99.0f) < 0.01f, "a camera above the atmosphere is held 1 km below its top");

    const EnvironmentUniformData dark =
        BuildEnvironmentUniformData(EnvironmentMode::Atmosphere, environment, p, std::nullopt, glm::vec3(0.0f));
    Require(glm::vec3(dark.sunIlluminance) == glm::vec3(0.0f), "no sun, no illuminance");
}
}

int main()
{
    try
    {
        ZenithTransmittanceMatchesOpticalDepth();
        LowSunIsDimmerAndRedder();
        EdgeCases();
        BuildsUniformData();
    }
    catch (const std::exception& error)
    {
        std::cerr << "atmosphere tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "atmosphere tests passed\n";
    return 0;
}
```

Register it after `miniengine_ao_history_tests`, same shape, linking `engine_render_core`; test name `miniengine.atmosphere`.

- [ ] **Step 2: Run to verify it fails.** Configure, build: `atmosphere.h` not found.

- [ ] **Step 3: Implement.** `engine/renderer/atmosphere.h`:

```cpp
#pragma once

#include <engine/scene/scene_environment.h>

#include <glm/glm.hpp>

#include <optional>

namespace me
{

// The ozone layer's tent profile (Hillaire 2020): density 1 at 25 km, falling linearly to 0 at 10
// and 40 km. Mirrored by OZONE_* in shaders/vulkan/atmosphere_common.glsl.
inline constexpr float kOzoneCenterAltitudeKm = 25.0f;
inline constexpr float kOzoneHalfWidthKm = 15.0f;

// Hillaire 2020's Earth atmosphere with the scene's scales applied, in kilometres. Everything the
// LUT shaders need comes from here through EnvironmentUniformData, so no coefficient is written
// twice.
struct AtmosphereParameters
{
    float bottomRadiusKm = 6360.0f;
    float topRadiusKm = 6460.0f;
    glm::vec3 rayleighScattering{5.802e-3f, 13.558e-3f, 33.1e-3f};
    float rayleighScaleHeightKm = 8.0f;
    float mieScattering = 3.996e-3f;
    float mieExtinction = 4.40e-3f;
    float mieScaleHeightKm = 1.2f;
    float mieAnisotropy = 0.8f;
    glm::vec3 ozoneAbsorption{0.650e-3f, 1.881e-3f, 0.085e-3f};
    glm::vec3 groundAlbedo{0.3f};
    float sunAngularDiameterDegrees = 0.545f;
    float aerialPerspectiveDistanceScale = 1.0f;

    bool operator==(const AtmosphereParameters&) const = default;
};

// Applies the settings' scales to the Earth defaults. Every setting is clamped to the range the
// editor offers, so a value from a hand-edited scene cannot reach the shaders.
AtmosphereParameters BuildAtmosphereParameters(const AtmosphereSettings& settings);

// Per-km extinction at an altitude above the ground.
glm::vec3 ComputeExtinction(const AtmosphereParameters& p, float altitudeKm);

// Transmittance from a point at this altitude (km above the ground) along a direction with this
// cos zenith, out to the top of the atmosphere. Zero when the ray hits the ground; one when it
// never enters the atmosphere. Integrates the same medium as the transmittance LUT shader.
glm::vec3 ComputeTransmittanceToSpace(const AtmosphereParameters& p, float altitudeKm, float cosZenith);

// The camera relative to the planet centre, in km: world metres / 1000, with world y = 0 on the
// ground. The altitude is held in [0.5 m, top - 1 km], the range the LUTs are built for.
glm::vec3 ToAtmosphereCameraPositionKm(const AtmosphereParameters& p, const glm::vec3& cameraPositionMeters);

struct AtmosphereSun
{
    // Unit vector from the scene toward the sun.
    glm::vec3 directionToSun{0.0f, 1.0f, 0.0f};
    // Top-of-atmosphere illuminance in lux: the light's colour times its intensity.
    glm::vec3 illuminance{0.0f};
};

// The environment block at the end of CameraBuffer in shaders/vulkan/scene_common.glsl, member for
// member. Every member is a vec4 so the C++ layout is the std140 layout.
struct EnvironmentUniformData
{
    glm::vec4 sunDirectionAndMode{0.0f, 1.0f, 0.0f, 0.0f}; // xyz toward the sun, w EnvironmentMode
    glm::vec4 sunIlluminance{0.0f};                        // rgb lux at the top of the atmosphere, w cos(sun angular radius)
    glm::vec4 rayleighScattering{0.0f};                    // rgb per km, w scale height km
    glm::vec4 mieParameters{0.0f};                         // x scattering per km, y extinction per km, z scale height km, w g
    glm::vec4 ozoneAbsorption{0.0f};                       // rgb per km
    glm::vec4 groundAlbedo{0.0f};                          // rgb
    glm::vec4 radii{0.0f};                                 // x bottom km, y top km, z aerial perspective distance scale
    glm::vec4 cameraPositionKm{0.0f};                      // xyz, see ToAtmosphereCameraPositionKm
    glm::vec4 hdriParameters{0.0f};                        // x intensity, y rotation in turns
};
static_assert(sizeof(EnvironmentUniformData) == 9 * 16, "EnvironmentUniformData must stay nine vec4s");

// mode is the mode the frame renders with, which differs from environment.mode while an HDRI is
// still loading. With no sun the illuminance is zero and the sky is black.
EnvironmentUniformData BuildEnvironmentUniformData(
    EnvironmentMode mode,
    const SceneEnvironment& environment,
    const AtmosphereParameters& p,
    const std::optional<AtmosphereSun>& sun,
    const glm::vec3& cameraPositionMeters);
}
```

`engine/renderer/atmosphere.cpp`:

```cpp
#include "atmosphere.h"

#include <algorithm>
#include <cmath>

namespace me
{

AtmosphereParameters BuildAtmosphereParameters(const AtmosphereSettings& settings)
{
    AtmosphereParameters p{};
    const float rayleigh = std::clamp(settings.rayleighDensityScale, 0.0f, 10.0f);
    const float mie = std::clamp(settings.mieDensityScale, 0.0f, 10.0f);
    const float ozone = std::clamp(settings.ozoneDensityScale, 0.0f, 10.0f);
    p.rayleighScattering *= rayleigh;
    p.mieScattering *= mie;
    p.mieExtinction *= mie;
    p.ozoneAbsorption *= ozone;
    p.mieAnisotropy = std::clamp(settings.mieAnisotropy, 0.0f, 0.99f);
    p.groundAlbedo = glm::clamp(settings.groundAlbedo, glm::vec3(0.0f), glm::vec3(1.0f));
    p.sunAngularDiameterDegrees = std::clamp(settings.sunAngularDiameterDegrees, 0.1f, 5.0f);
    p.aerialPerspectiveDistanceScale = std::clamp(settings.aerialPerspectiveDistanceScale, 0.0f, 10000.0f);
    return p;
}

glm::vec3 ComputeExtinction(const AtmosphereParameters& p, float altitudeKm)
{
    const float altitude = std::max(altitudeKm, 0.0f);
    const float rayleighDensity = std::exp(-altitude / p.rayleighScaleHeightKm);
    const float mieDensity = std::exp(-altitude / p.mieScaleHeightKm);
    const float ozoneDensity = std::max(0.0f, 1.0f - std::fabs(altitude - kOzoneCenterAltitudeKm) / kOzoneHalfWidthKm);
    return p.rayleighScattering * rayleighDensity + glm::vec3(p.mieExtinction * mieDensity) + p.ozoneAbsorption * ozoneDensity;
}

glm::vec3 ComputeTransmittanceToSpace(const AtmosphereParameters& p, float altitudeKm, float cosZenith)
{
    const float r = p.bottomRadiusKm + std::max(altitudeKm, 0.0f);
    const float mu = std::clamp(cosZenith, -1.0f, 1.0f);
    // Below the horizon of the ground sphere: the ray ends on the planet.
    const float groundDiscriminant = r * r * (mu * mu - 1.0f) + p.bottomRadiusKm * p.bottomRadiusKm;
    if (mu < 0.0f && groundDiscriminant >= 0.0f)
    {
        return glm::vec3(0.0f);
    }
    const float topDiscriminant = r * r * (mu * mu - 1.0f) + p.topRadiusKm * p.topRadiusKm;
    if (topDiscriminant < 0.0f)
    {
        return glm::vec3(1.0f);
    }
    const float distance = -r * mu + std::sqrt(topDiscriminant);
    if (distance <= 0.0f)
    {
        return glm::vec3(1.0f);
    }

    constexpr int kSteps = 500;
    const float dt = distance / static_cast<float>(kSteps);
    glm::vec3 opticalDepth(0.0f);
    for (int step = 0; step < kSteps; ++step)
    {
        const float t = (static_cast<float>(step) + 0.5f) * dt;
        const float height = std::sqrt(r * r + t * t + 2.0f * r * mu * t) - p.bottomRadiusKm;
        opticalDepth += ComputeExtinction(p, height) * dt;
    }
    return glm::exp(-opticalDepth);
}

glm::vec3 ToAtmosphereCameraPositionKm(const AtmosphereParameters& p, const glm::vec3& cameraPositionMeters)
{
    const float altitude = std::clamp(cameraPositionMeters.y * 0.001f, 0.0005f, p.topRadiusKm - p.bottomRadiusKm - 1.0f);
    return glm::vec3(cameraPositionMeters.x * 0.001f, p.bottomRadiusKm + altitude, cameraPositionMeters.z * 0.001f);
}

EnvironmentUniformData BuildEnvironmentUniformData(
    EnvironmentMode mode,
    const SceneEnvironment& environment,
    const AtmosphereParameters& p,
    const std::optional<AtmosphereSun>& sun,
    const glm::vec3& cameraPositionMeters)
{
    EnvironmentUniformData data{};
    const glm::vec3 directionToSun = sun.has_value() ? glm::normalize(sun->directionToSun) : glm::vec3(0.0f, 1.0f, 0.0f);
    data.sunDirectionAndMode = glm::vec4(directionToSun, static_cast<float>(static_cast<uint32_t>(mode)));
    data.sunIlluminance = glm::vec4(
        sun.has_value() ? sun->illuminance : glm::vec3(0.0f),
        std::cos(glm::radians(p.sunAngularDiameterDegrees * 0.5f)));
    data.rayleighScattering = glm::vec4(p.rayleighScattering, p.rayleighScaleHeightKm);
    data.mieParameters = glm::vec4(p.mieScattering, p.mieExtinction, p.mieScaleHeightKm, p.mieAnisotropy);
    data.ozoneAbsorption = glm::vec4(p.ozoneAbsorption, 0.0f);
    data.groundAlbedo = glm::vec4(p.groundAlbedo, 0.0f);
    data.radii = glm::vec4(p.bottomRadiusKm, p.topRadiusKm, p.aerialPerspectiveDistanceScale, 0.0f);
    data.cameraPositionKm = glm::vec4(ToAtmosphereCameraPositionKm(p, cameraPositionMeters), 0.0f);
    data.hdriParameters = glm::vec4(
        std::max(environment.hdri.intensity, 0.0f),
        environment.hdri.rotationDegrees / 360.0f,
        0.0f,
        0.0f);
    return data;
}
}
```

Add `atmosphere.cpp`/`atmosphere.h` to `engine_render_core` (alphabetical, first).

- [ ] **Step 4: Run to verify it passes.** Configure, build, `ctest ... -R atmosphere --output-on-failure`: PASS. If `ZenithTransmittanceMatchesOpticalDepth` misses by a little on one channel, print both vectors: the midpoint rule with 500 steps over 100 km should be well inside 0.5%; do not loosen the tolerance before understanding why.

- [ ] **Step 5: Commit.**

```bash
git add engine/renderer/atmosphere.h engine/renderer/atmosphere.cpp engine/renderer/CMakeLists.txt tests/atmosphere_tests.cpp tests/CMakeLists.txt
git commit -m "feat(renderer): CPU atmosphere model and environment uniform data

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Atmosphere LUTs on the GPU

**Files:**
- Create: `shaders/vulkan/atmosphere_common.glsl`, `shaders/vulkan/atmosphere_integrate.glsl`, `shaders/vulkan/atmosphere_transmittance.comp`, `shaders/vulkan/atmosphere_multiscattering.comp`, `shaders/vulkan/atmosphere_skyview.comp`, `shaders/vulkan/atmosphere_aerial_perspective.comp`, `engine/renderer/vulkan/atmosphere.h`, `engine/renderer/vulkan/atmosphere.cpp`
- Modify: `shaders/vulkan/scene_common.glsl`, `engine/renderer/vulkan/uniform_buffer.h`, `engine/renderer/vulkan/uniform_buffer.cpp`, `engine/renderer/vulkan/texture.h`, `engine/renderer/vulkan/texture.cpp`, `engine/renderer/vulkan/renderer.h`, `engine/renderer/vulkan/renderer.cpp`, `engine/renderer/CMakeLists.txt`

**Interfaces:**
- Consumes: `EnvironmentUniformData`, `BuildEnvironmentUniformData`, `BuildAtmosphereParameters`, `ComputeTransmittanceToSpace`, `ToAtmosphereCameraPositionKm`, `AtmosphereSun` (Task 3); `SceneEnvironment` (Task 2).
- Produces: `VulkanAtmosphere(VkPhysicalDevice, VkDevice, VkPipelineCache, VkDescriptorSetLayout frameSetLayout)`, `VulkanAtmosphere::Record(VkCommandBuffer, VkDescriptorSet frameDescriptorSet, const AtmosphereParameters* parameters)`, `GetTransmittanceBinding()`, `GetSkyViewBinding()`, `GetAerialPerspectiveBinding()`; `me::EnvironmentDescriptorBindings { TextureDescriptorBinding transmittance, skyView, aerialPerspective, environmentMap; }`; `VulkanTexture(VkPhysicalDevice, VkDevice, const FloatTextureData&, VulkanUploadBatch&)` (equirectangular map); `VulkanRenderer::BuildEnvironmentBindings()`; `VulkanRenderer::EffectiveEnvironmentMode(const SceneEnvironment&)`; GLSL `EnvironmentMode()`, `SampleTransmittance`, `SampleMultipleScattering`, `TransmittanceLutParamsToUv`, `SkyViewLutParamsToUv`, `RaySphereIntersectNearest`, `AERIAL_PERSPECTIVE_*`.

No automated test drives the GPU. Verification: build, suite, validation runs in all three modes; the pictures come in Task 5.

- [ ] **Step 1: UBO block.** `scene_common.glsl`, after `mat4 prevViewProj;` inside `CameraBuffer`:

```glsl
    // Environment: EnvironmentUniformData in engine/renderer/atmosphere.h, member for member.
    vec4 sunDirectionAndMode;        // xyz toward the sun, w EnvironmentMode
    vec4 sunIlluminance;             // rgb lux at the top of the atmosphere, w cos(sun angular radius)
    vec4 rayleighScattering;         // rgb per km, w scale height km
    vec4 mieParameters;              // x scattering per km, y extinction per km, z scale height km, w g
    vec4 ozoneAbsorption;            // rgb per km
    vec4 groundAlbedo;               // rgb
    vec4 atmosphereRadii;            // x bottom km, y top km, z aerial perspective distance scale
    vec4 atmosphereCameraPositionKm; // xyz camera relative to the planet centre
    vec4 hdriParameters;             // x intensity, y rotation in turns
```

`uniform_buffer.h`: `#include "../atmosphere.h"`; `CameraUniformData` gains, after `prevViewProj`, `EnvironmentUniformData environment;` with the comment `// Appended last so no earlier member's offset moves.`; extend the size `static_assert` by `+ 9 * 16` and add

```cpp
static_assert(
    offsetof(CameraUniformData, environment) ==
        2 * 64 + 2 * 16 + kMaxSceneLights * 80 + 16 + kShadowCascadeCount * 64 + 3 * 16 + 64 + 64,
    "environment must follow prevViewProj with no padding");
```

`VulkanUniformBuffer::Update` gains a last parameter `const EnvironmentUniformData& environment` and sets `data.environment = environment;`.

- [ ] **Step 2: Set 0 bindings 3 to 6.** `uniform_buffer.h`, after `MaterialTextureBinding`:

```cpp
// The environment images set 0 binds for the fragment shaders: the atmosphere LUTs, kept in
// VK_IMAGE_LAYOUT_GENERAL by VulkanAtmosphere, and the equirectangular HDRI (a 1x1 black map when
// none is loaded), in SHADER_READ_ONLY_OPTIMAL.
struct EnvironmentDescriptorBindings
{
    TextureDescriptorBinding transmittance;
    TextureDescriptorBinding skyView;
    TextureDescriptorBinding aerialPerspective;
    TextureDescriptorBinding environmentMap;
};
```

`VulkanUniformBuffer`'s constructor gains `EnvironmentDescriptorBindings environment` after `TextureDescriptorBinding shadowMap`; store it in `m_environment`. `VulkanFrameDescriptorSetLayout` becomes seven bindings: after binding 2,

```cpp
    // The atmosphere LUTs (3 transmittance, 4 sky-view, 5 aerial perspective) and the HDRI (6),
    // sampled by the sky, lighting and forward fragment shaders.
    for (uint32_t binding = 3; binding <= 6; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
```

(`std::array<VkDescriptorSetLayoutBinding, 7>`). The pool's combined image sampler count becomes `materialSetCount * 13 + imageCount * 5`. In `CreateDescriptorSets`, the frame writes become seven: after the motion write,

```cpp
        const std::array<VkDescriptorImageInfo, 4> environmentInfos = {
            VkDescriptorImageInfo{m_environment.transmittance.sampler, m_environment.transmittance.imageView, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{m_environment.skyView.sampler, m_environment.skyView.imageView, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{m_environment.aerialPerspective.sampler, m_environment.aerialPerspective.imageView, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{m_environment.environmentMap.sampler, m_environment.environmentMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        for (uint32_t index = 0; index < 4; ++index)
        {
            VkWriteDescriptorSet& write = frameWrites[3 + index];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_frameDescriptorSets[i];
            write.dstBinding = 3 + index;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &environmentInfos[index];
        }
```

(`std::array<VkWriteDescriptorSet, 7> frameWrites{}`).

- [ ] **Step 3: Equirectangular maps in `VulkanTexture`.** `texture.h`: include `<engine/asset/texture_loader.h>` (already there); add the constructor

```cpp
    // An equirectangular environment map: R32G32B32A32_SFLOAT when the device filters that format
    // linearly, else packed to R16G16B16A16_SFLOAT (values clamp at 65504). One mip level: the map
    // is magnified, never minified, and a mip chain would seam where the longitude wraps. Repeats
    // in u, clamps in v.
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const FloatTextureData& equirectangular,
        VulkanUploadBatch& uploadBatch);
```

`UploadTexels` gains a trailing `bool generateMips = true`, and `CreateViewAndSampler` a trailing `VkSamplerAddressMode addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT` used for `samplerInfo.addressModeV`. In `UploadTexels` the mip count becomes `canGenerateMips && generateMips ? <existing expression> : 1`, and it passes nothing new to `CreateViewAndSampler` (so existing callers keep repeat). For the environment map, `UploadTexels` needs to create the view with clamped v, so give `UploadTexels` a trailing `VkSamplerAddressMode addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT` too and forward it. The new constructor:

```cpp
VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const FloatTextureData& equirectangular,
    VulkanUploadBatch& uploadBatch)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_textureFormat(VulkanTextureFormat::LinearData)
{
    try
    {
        if (!equirectangular.IsValid())
        {
            throw std::runtime_error("Cannot create an environment map from invalid float data");
        }
        const uint32_t width = static_cast<uint32_t>(equirectangular.width);
        const uint32_t height = static_cast<uint32_t>(equirectangular.height);
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, VK_FORMAT_R32G32B32A32_SFLOAT, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0)
        {
            UploadTexels(
                equirectangular.pixels.data(),
                static_cast<VkDeviceSize>(equirectangular.pixels.size() * sizeof(float)),
                width,
                height,
                VK_FORMAT_R32G32B32A32_SFLOAT,
                uploadBatch,
                false,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        }
        else
        {
            const HalfFloatTextureData packed = PackRgba16Float(equirectangular);
            UploadTexels(
                packed.texels.data(),
                static_cast<VkDeviceSize>(packed.texels.size() * sizeof(std::uint16_t)),
                width,
                height,
                VK_FORMAT_R16G16B16A16_SFLOAT,
                uploadBatch,
                false,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        }
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}
```

- [ ] **Step 4: Shared GLSL.** `shaders/vulkan/atmosphere_common.glsl`:

```glsl
// Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique": the
// medium, ray-sphere intersection, phase functions and LUT parameterisations, shared by the LUT
// compute shaders and the fragment shaders that sample the LUTs. Every coefficient comes from the
// environment block of CameraBuffer, filled by BuildEnvironmentUniformData
// (engine/renderer/atmosphere.h); only the ozone profile and the LUT sizes are constants here.
//
// Units: kilometres, planet centre at the origin, +Y up. Include after scene_common.glsl.
#ifndef ATMOSPHERE_COMMON_GLSL
#define ATMOSPHERE_COMMON_GLSL

// Must match EnvironmentMode in engine/scene/scene_environment.h.
#define ENVIRONMENT_NONE 0u
#define ENVIRONMENT_ATMOSPHERE 1u
#define ENVIRONMENT_HDRI 2u

const float ATMOSPHERE_PI = 3.14159265358979;
// Must match the image sizes in engine/renderer/vulkan/atmosphere.cpp.
const vec2 TRANSMITTANCE_LUT_SIZE = vec2(256.0, 64.0);
const vec2 MULTI_SCATTERING_LUT_SIZE = vec2(32.0, 32.0);
const vec2 SKY_VIEW_LUT_SIZE = vec2(192.0, 108.0);
// Slices are spread quadratically: slice s (texel centre (s + 0.5) / 32) sits at
// ((s + 0.5) / 32)^2 * 32 km, so near slices are fine and the far ones reach 32 km.
const float AERIAL_PERSPECTIVE_SLICE_COUNT = 32.0;
const float AERIAL_PERSPECTIVE_KM_PER_SLICE = 1.0;
// Must match kOzoneCenterAltitudeKm and kOzoneHalfWidthKm in engine/renderer/atmosphere.h.
const float OZONE_CENTER_ALTITUDE_KM = 25.0;
const float OZONE_HALF_WIDTH_KM = 15.0;
// Lifts points off the ground so a ray that starts on it does not hit it again.
const float PLANET_RADIUS_OFFSET_KM = 0.01;

uint EnvironmentMode()
{
    return uint(ubo.sunDirectionAndMode.w + 0.5);
}

float BottomRadius()
{
    return ubo.atmosphereRadii.x;
}

float TopRadius()
{
    return ubo.atmosphereRadii.y;
}

struct MediumSample
{
    vec3 rayleighScattering;
    vec3 mieScattering;
    vec3 scattering;
    vec3 extinction;
};

MediumSample SampleMedium(vec3 positionKm)
{
    float altitude = max(length(positionKm) - BottomRadius(), 0.0);
    float rayleighDensity = exp(-altitude / ubo.rayleighScattering.w);
    float mieDensity = exp(-altitude / ubo.mieParameters.z);
    float ozoneDensity = max(0.0, 1.0 - abs(altitude - OZONE_CENTER_ALTITUDE_KM) / OZONE_HALF_WIDTH_KM);

    MediumSample medium;
    medium.rayleighScattering = ubo.rayleighScattering.rgb * rayleighDensity;
    medium.mieScattering = vec3(ubo.mieParameters.x * mieDensity);
    medium.scattering = medium.rayleighScattering + medium.mieScattering;
    medium.extinction =
        medium.rayleighScattering +
        vec3(ubo.mieParameters.y * mieDensity) +
        ubo.ozoneAbsorption.rgb * ozoneDensity;
    return medium;
}

// Distance along the ray to the nearest intersection in front of the origin, or -1 for none.
float RaySphereIntersectNearest(vec3 origin, vec3 direction, vec3 center, float radius)
{
    vec3 offset = origin - center;
    float b = dot(direction, offset);
    float c = dot(offset, offset) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0)
    {
        return -1.0;
    }
    float root = sqrt(discriminant);
    float near = -b - root;
    float far = -b + root;
    if (near < 0.0 && far < 0.0)
    {
        return -1.0;
    }
    if (near < 0.0)
    {
        return max(0.0, far);
    }
    return max(0.0, near);
}

float RayleighPhase(float cosTheta)
{
    return 3.0 / (16.0 * ATMOSPHERE_PI) * (1.0 + cosTheta * cosTheta);
}

// cosTheta between the view direction and the direction toward the sun: 1 looks into the sun,
// where forward scattering peaks.
float CornetteShanksPhase(float g, float cosTheta)
{
    float k = 3.0 / (8.0 * ATMOSPHERE_PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + cosTheta * cosTheta) / pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1e-4), 1.5);
}

// Keep lookups at texel centres at the LUT edges (Hillaire's sub-UV mapping).
vec2 FromUnitToSubUvs(vec2 uv, vec2 size)
{
    return (uv + 0.5 / size) * (size / (size + 1.0));
}

vec2 FromSubUvsToUnit(vec2 uv, vec2 size)
{
    return (uv - 0.5 / size) * (size / (size - 1.0));
}

// Bruneton's transmittance parameterisation: x the distance to the top boundary between its
// minimum and maximum for this height, y the height as a fraction of the horizon distance.
vec2 TransmittanceLutParamsToUv(float viewHeight, float cosZenith)
{
    float bottom = BottomRadius();
    float top = TopRadius();
    float H = sqrt(max(0.0, top * top - bottom * bottom));
    float rho = sqrt(max(0.0, viewHeight * viewHeight - bottom * bottom));
    float discriminant = viewHeight * viewHeight * (cosZenith * cosZenith - 1.0) + top * top;
    float d = max(0.0, -viewHeight * cosZenith + sqrt(max(discriminant, 0.0)));
    float dMin = top - viewHeight;
    float dMax = rho + H;
    return vec2((d - dMin) / max(dMax - dMin, 1e-6), rho / H);
}

void UvToTransmittanceLutParams(vec2 uv, out float viewHeight, out float cosZenith)
{
    float bottom = BottomRadius();
    float top = TopRadius();
    float H = sqrt(max(0.0, top * top - bottom * bottom));
    float rho = H * uv.y;
    viewHeight = sqrt(rho * rho + bottom * bottom);
    float dMin = top - viewHeight;
    float dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    cosZenith = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * viewHeight * d);
    cosZenith = clamp(cosZenith, -1.0, 1.0);
}

vec3 SampleTransmittance(sampler2D lut, float viewHeight, float cosZenith)
{
    return textureLod(lut, TransmittanceLutParamsToUv(viewHeight, cosZenith), 0.0).rgb;
}

vec3 SampleMultipleScattering(sampler2D lut, float viewHeight, float cosSunZenith)
{
    vec2 uv = clamp(
        vec2(cosSunZenith * 0.5 + 0.5, (viewHeight - BottomRadius()) / (TopRadius() - BottomRadius())),
        vec2(0.0),
        vec2(1.0));
    return textureLod(lut, FromUnitToSubUvs(uv, MULTI_SCATTERING_LUT_SIZE), 0.0).rgb;
}

// Sky-view parameterisation: y concentrates texels at the horizon (above it in [0, 0.5), below in
// [0.5, 1]), x is the azimuth relative to the sun as sqrt of (1 - cos) / 2.
void UvToSkyViewLutParams(vec2 uv, float viewHeight, out float viewZenithCos, out float lightViewCos)
{
    uv = FromSubUvsToUnit(uv, SKY_VIEW_LUT_SIZE);
    float horizonDistance = sqrt(max(0.0, viewHeight * viewHeight - BottomRadius() * BottomRadius()));
    float beta = acos(clamp(horizonDistance / viewHeight, -1.0, 1.0));
    float zenithHorizonAngle = ATMOSPHERE_PI - beta;
    if (uv.y < 0.5)
    {
        float coord = 1.0 - 2.0 * uv.y;
        coord = 1.0 - coord * coord;
        viewZenithCos = cos(zenithHorizonAngle * coord);
    }
    else
    {
        float coord = uv.y * 2.0 - 1.0;
        coord *= coord;
        viewZenithCos = cos(zenithHorizonAngle + beta * coord);
    }
    float coord = uv.x * uv.x;
    lightViewCos = -(coord * 2.0 - 1.0);
}

vec2 SkyViewLutParamsToUv(bool intersectGround, float viewZenithCos, float lightViewCos, float viewHeight)
{
    float horizonDistance = sqrt(max(0.0, viewHeight * viewHeight - BottomRadius() * BottomRadius()));
    float beta = acos(clamp(horizonDistance / viewHeight, -1.0, 1.0));
    float zenithHorizonAngle = ATMOSPHERE_PI - beta;
    float viewZenithAngle = acos(clamp(viewZenithCos, -1.0, 1.0));
    vec2 uv;
    if (!intersectGround)
    {
        float coord = clamp(viewZenithAngle / zenithHorizonAngle, 0.0, 1.0);
        coord = 1.0 - sqrt(max(0.0, 1.0 - coord));
        uv.y = coord * 0.5;
    }
    else
    {
        float coord = clamp((viewZenithAngle - zenithHorizonAngle) / beta, 0.0, 1.0);
        uv.y = sqrt(coord) * 0.5 + 0.5;
    }
    uv.x = sqrt(clamp(-lightViewCos * 0.5 + 0.5, 0.0, 1.0));
    return FromUnitToSubUvs(uv, SKY_VIEW_LUT_SIZE);
}

#endif
```

`shaders/vulkan/atmosphere_integrate.glsl` (compute only):

```glsl
// Hillaire 2020's ray-marched in-scattering, shared by the multiple-scattering, sky-view and
// aerial perspective LUT shaders. Include after atmosphere_common.glsl.
#ifndef ATMOSPHERE_INTEGRATE_GLSL
#define ATMOSPHERE_INTEGRATE_GLSL

struct ScatteringResult
{
    vec3 luminance;
    vec3 transmittance;
    // The multiple-scattering LUT's f_ms: light scattered once more along the ray, for a unit
    // illuminance and an isotropic phase.
    vec3 multiScatteringAs1;
};

// Marches from origin along direction to the nearest of the atmosphere's edge, the ground and
// tMaxLimit, in sampleCount uniform steps, integrating each step's in-scattering analytically.
// anisotropicPhase uses the Rayleigh and Cornette-Shanks phases; otherwise the uniform one.
// useMultiScattering adds the multiple-scattering LUT's contribution; includeGround adds the
// sunlit ground's Lambertian reflection where the ray ends on it.
ScatteringResult IntegrateScatteredLuminance(
    vec3 origin,
    vec3 direction,
    vec3 sunDirection,
    vec3 illuminance,
    sampler2D transmittanceLut,
    sampler2D multiScatteringLut,
    bool includeGround,
    float sampleCount,
    float tMaxLimit,
    bool anisotropicPhase,
    bool useMultiScattering)
{
    ScatteringResult result;
    result.luminance = vec3(0.0);
    result.transmittance = vec3(1.0);
    result.multiScatteringAs1 = vec3(0.0);

    float tBottom = RaySphereIntersectNearest(origin, direction, vec3(0.0), BottomRadius());
    float tTop = RaySphereIntersectNearest(origin, direction, vec3(0.0), TopRadius());
    float tMax;
    if (tBottom < 0.0)
    {
        if (tTop < 0.0)
        {
            return result;
        }
        tMax = tTop;
    }
    else
    {
        tMax = tTop > 0.0 ? min(tTop, tBottom) : tBottom;
    }
    bool endsOnGround = tBottom >= 0.0 && tMax == tBottom && tBottom <= tMaxLimit;
    tMax = min(tMax, tMaxLimit);

    float cosTheta = dot(direction, sunDirection);
    float miePhase = CornetteShanksPhase(ubo.mieParameters.w, cosTheta);
    float rayleighPhase = RayleighPhase(cosTheta);
    const float uniformPhase = 1.0 / (4.0 * ATMOSPHERE_PI);

    float dt = tMax / sampleCount;
    vec3 throughput = vec3(1.0);
    for (float step = 0.0; step < sampleCount; step += 1.0)
    {
        float t = (step + 0.5) * dt;
        vec3 position = origin + t * direction;
        MediumSample medium = SampleMedium(position);
        vec3 extinction = max(medium.extinction, vec3(1e-6));
        vec3 stepTransmittance = exp(-medium.extinction * dt);

        float height = length(position);
        vec3 up = position / height;
        float cosSunZenith = dot(sunDirection, up);
        vec3 transmittanceToSun = SampleTransmittance(transmittanceLut, height, cosSunZenith);
        vec3 phaseTimesScattering = anisotropicPhase
                                        ? medium.mieScattering * miePhase + medium.rayleighScattering * rayleighPhase
                                        : medium.scattering * uniformPhase;
        float tEarth = RaySphereIntersectNearest(position, sunDirection, PLANET_RADIUS_OFFSET_KM * up, BottomRadius());
        float earthShadow = tEarth >= 0.0 ? 0.0 : 1.0;
        vec3 multiScattered = useMultiScattering
                                  ? SampleMultipleScattering(multiScatteringLut, height, cosSunZenith)
                                  : vec3(0.0);

        vec3 inScattering = illuminance * (earthShadow * transmittanceToSun * phaseTimesScattering + multiScattered * medium.scattering);
        // Energy-conserving analytic integration of S * T over the step (Hillaire 2015).
        result.luminance += throughput * (inScattering - inScattering * stepTransmittance) / extinction;
        result.multiScatteringAs1 += throughput * (medium.scattering - medium.scattering * stepTransmittance) / extinction;
        throughput *= stepTransmittance;
    }

    if (includeGround && endsOnGround)
    {
        vec3 position = origin + tBottom * direction;
        float height = length(position);
        vec3 up = position / height;
        float cosSunZenith = dot(sunDirection, up);
        vec3 transmittanceToSun = SampleTransmittance(transmittanceLut, height, cosSunZenith);
        float nDotL = clamp(dot(up, sunDirection), 0.0, 1.0);
        result.luminance += illuminance * transmittanceToSun * throughput * nDotL * ubo.groundAlbedo.rgb / ATMOSPHERE_PI;
    }

    result.transmittance = throughput;
    return result;
}

#endif
```

- [ ] **Step 5: The four compute shaders.** All start with:

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"
#include "atmosphere_common.glsl"
```

and share set 1 (`VulkanAtmosphere`'s layout): binding 0 transmittance storage, 1 multiple scattering storage, 2 sky-view storage, 3 aerial perspective storage (3D), 4 transmittance sampler, 5 multiple-scattering sampler. Each declares only what it uses.

`atmosphere_transmittance.comp`:

```glsl
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D transmittanceOut;

void main()
{
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(texel, ivec2(TRANSMITTANCE_LUT_SIZE))))
    {
        return;
    }
    float viewHeight;
    float cosZenith;
    UvToTransmittanceLutParams((vec2(texel) + 0.5) / TRANSMITTANCE_LUT_SIZE, viewHeight, cosZenith);

    // The parameterisation only covers rays that reach the top boundary, so the march always ends
    // there.
    vec3 origin = vec3(0.0, viewHeight, 0.0);
    vec3 direction = vec3(sqrt(max(0.0, 1.0 - cosZenith * cosZenith)), cosZenith, 0.0);
    float tMax = RaySphereIntersectNearest(origin, direction, vec3(0.0), TopRadius());
    const float stepCount = 40.0;
    float dt = max(tMax, 0.0) / stepCount;
    vec3 opticalDepth = vec3(0.0);
    for (float step = 0.0; step < stepCount; step += 1.0)
    {
        opticalDepth += SampleMedium(origin + (step + 0.5) * dt * direction).extinction * dt;
    }
    imageStore(transmittanceOut, texel, vec4(exp(-opticalDepth), 1.0));
}
```

`atmosphere_multiscattering.comp` (also `#include "atmosphere_integrate.glsl"`):

```glsl
// One workgroup per texel; its 64 invocations each integrate one direction of an 8 x 8 grid that
// is uniform over the sphere, then reduce.
layout(local_size_x = 1, local_size_y = 1, local_size_z = 64) in;

layout(set = 1, binding = 1, rgba16f) uniform writeonly image2D multiScatteringOut;
layout(set = 1, binding = 4) uniform sampler2D transmittanceLut;
layout(set = 1, binding = 5) uniform sampler2D multiScatteringLut;

shared vec3 sharedMultiScatteringAs1[64];
shared vec3 sharedLuminance[64];

void main()
{
    ivec2 texel = ivec2(gl_WorkGroupID.xy);
    uint index = gl_LocalInvocationID.z;

    vec2 uv = FromSubUvsToUnit((vec2(texel) + 0.5) / MULTI_SCATTERING_LUT_SIZE, MULTI_SCATTERING_LUT_SIZE);
    float cosSunZenith = clamp(uv.x * 2.0 - 1.0, -1.0, 1.0);
    float viewHeight = BottomRadius() + clamp(uv.y + PLANET_RADIUS_OFFSET_KM, 0.0, 1.0) * (TopRadius() - BottomRadius() - PLANET_RADIUS_OFFSET_KM);
    vec3 origin = vec3(0.0, viewHeight, 0.0);
    vec3 sunDirection = vec3(0.0, cosSunZenith, -sqrt(max(0.0, 1.0 - cosSunZenith * cosSunZenith)));

    float i = 0.5 + float(index / 8u);
    float j = 0.5 + float(index % 8u);
    float theta = 2.0 * ATMOSPHERE_PI * i / 8.0;
    float phi = acos(1.0 - 2.0 * j / 8.0);
    vec3 direction = vec3(cos(theta) * sin(phi), cos(phi), sin(theta) * sin(phi));

    ScatteringResult result = IntegrateScatteredLuminance(
        origin, direction, sunDirection, vec3(1.0), transmittanceLut, multiScatteringLut,
        true, 20.0, 9.0e9, false, false);
    sharedMultiScatteringAs1[index] = result.multiScatteringAs1 / 64.0;
    sharedLuminance[index] = result.luminance / 64.0;
    barrier();

    for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (index < stride)
        {
            sharedMultiScatteringAs1[index] += sharedMultiScatteringAs1[index + stride];
            sharedLuminance[index] += sharedLuminance[index + stride];
        }
        barrier();
    }

    if (index == 0u)
    {
        // The isotropic phase times the sphere's solid angle is 1, so the averages are the
        // second-order luminance and f_ms; every further order is a geometric series.
        vec3 fms = sharedMultiScatteringAs1[0];
        vec3 luminance = sharedLuminance[0] / max(vec3(1.0) - fms, vec3(1e-4));
        imageStore(multiScatteringOut, texel, vec4(luminance, 1.0));
    }
}
```

`atmosphere_skyview.comp` (includes `atmosphere_integrate.glsl`):

```glsl
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 2, rgba16f) uniform writeonly image2D skyViewOut;
layout(set = 1, binding = 4) uniform sampler2D transmittanceLut;
layout(set = 1, binding = 5) uniform sampler2D multiScatteringLut;

void main()
{
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(texel, ivec2(SKY_VIEW_LUT_SIZE))))
    {
        return;
    }
    vec3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float viewHeight = length(camera);
    float viewZenithCos;
    float lightViewCos;
    UvToSkyViewLutParams((vec2(texel) + 0.5) / SKY_VIEW_LUT_SIZE, viewHeight, viewZenithCos, lightViewCos);

    // A local frame with +Y up and the sun's azimuth along +X; only the angles matter.
    float cosSunZenith = dot(camera / viewHeight, ubo.sunDirectionAndMode.xyz);
    vec3 sunDirection = normalize(vec3(sqrt(max(0.0, 1.0 - cosSunZenith * cosSunZenith)), cosSunZenith, 0.0));
    float viewZenithSin = sqrt(max(0.0, 1.0 - viewZenithCos * viewZenithCos));
    vec3 direction = vec3(
        viewZenithSin * lightViewCos,
        viewZenithCos,
        viewZenithSin * sqrt(max(0.0, 1.0 - lightViewCos * lightViewCos)));

    ScatteringResult result = IntegrateScatteredLuminance(
        vec3(0.0, viewHeight, 0.0), direction, sunDirection, ubo.sunIlluminance.rgb,
        transmittanceLut, multiScatteringLut, false, 30.0, 9.0e9, true, true);
    imageStore(skyViewOut, texel, vec4(result.luminance, 1.0));
}
```

`atmosphere_aerial_perspective.comp` (includes `atmosphere_integrate.glsl`):

```glsl
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 1, binding = 3, rgba16f) uniform writeonly image3D aerialPerspectiveOut;
layout(set = 1, binding = 4) uniform sampler2D transmittanceLut;
layout(set = 1, binding = 5) uniform sampler2D multiScatteringLut;

void main()
{
    ivec3 texel = ivec3(gl_GlobalInvocationID.xyz);
    // The volume covers the view frustum: xy the screen, z the distance (see
    // AERIAL_PERSPECTIVE_KM_PER_SLICE for the quadratic spread).
    vec2 uv = (vec2(texel.xy) + 0.5) / AERIAL_PERSPECTIVE_SLICE_COUNT;
    vec4 farPoint = ubo.invViewProj * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 direction = normalize(farPoint.xyz / farPoint.w - ubo.cameraWorldPosition.xyz);
    vec3 camera = ubo.atmosphereCameraPositionKm.xyz;

    float slice = (float(texel.z) + 0.5) / AERIAL_PERSPECTIVE_SLICE_COUNT;
    slice = slice * slice * AERIAL_PERSPECTIVE_SLICE_COUNT;
    float tMax = slice * AERIAL_PERSPECTIVE_KM_PER_SLICE;
    vec3 end = camera + tMax * direction;
    if (length(end) <= BottomRadius() + PLANET_RADIUS_OFFSET_KM)
    {
        // Below the ground: aim at the point on the ground instead, so the slice still holds
        // plausible haze for geometry drawn there.
        end = normalize(end) * (BottomRadius() + PLANET_RADIUS_OFFSET_KM);
        direction = normalize(end - camera);
        tMax = length(end - camera);
    }

    ScatteringResult result = IntegrateScatteredLuminance(
        camera, direction, ubo.sunDirectionAndMode.xyz, ubo.sunIlluminance.rgb,
        transmittanceLut, multiScatteringLut, false, max(1.0, float(texel.z + 1) * 2.0), tMax, true, true);
    float meanTransmittance = dot(result.transmittance, vec3(1.0 / 3.0));
    imageStore(aerialPerspectiveOut, texel, vec4(result.luminance, meanTransmittance));
}
```

Add the four `.comp` files to `MINIENGINE_SHADER_SOURCES` and `atmosphere_common.glsl`, `atmosphere_integrate.glsl` to `MINIENGINE_SHADER_INCLUDES` in `engine/renderer/CMakeLists.txt`.

- [ ] **Step 6: `VulkanAtmosphere`.** `engine/renderer/vulkan/atmosphere.h`:

```cpp
#pragma once

#include "common.h"
#include "uniform_buffer.h"

#include <engine/renderer/atmosphere.h>

#include <array>
#include <optional>

namespace me
{

// Hillaire 2020's four LUTs: transmittance and multiple scattering (rebuilt when the atmosphere's
// parameters change), sky-view and the aerial perspective volume (every frame). Like the shadow
// map, this is not an IScenePass and its images are not render targets: they have fixed sizes and
// one copy shared by every frame in flight, so they stay in VK_IMAGE_LAYOUT_GENERAL and Record
// orders itself with its own barriers. A barrier's first scope is every command submitted earlier
// on the queue, so the one at the head of Record covers the previous frame's fragment reads.
//
// Set 0 names these images for every draw, whatever the mode, so the first Record moves them out
// of UNDEFINED and clears them even when the atmosphere is off.
class VulkanAtmosphere
{
  public:
    VulkanAtmosphere(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        VkDescriptorSetLayout frameSetLayout);
    ~VulkanAtmosphere();

    VulkanAtmosphere(const VulkanAtmosphere&) = delete;
    VulkanAtmosphere& operator=(const VulkanAtmosphere&) = delete;

    // parameters is null when the frame does not render the atmosphere; the frame descriptor set
    // carries the same parameters in its camera block.
    void Record(VkCommandBuffer commandBuffer, VkDescriptorSet frameDescriptorSet, const AtmosphereParameters* parameters);

    TextureDescriptorBinding GetTransmittanceBinding() const;
    TextureDescriptorBinding GetSkyViewBinding() const;
    TextureDescriptorBinding GetAerialPerspectiveBinding() const;

  private:
    enum Lut : size_t
    {
        kTransmittance,
        kMultiScattering,
        kSkyView,
        kAerialPerspective,
        kLutCount
    };

    struct LutImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    void CreateImages();
    void CreateDescriptors();
    void CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout);
    void Dispatch(VkCommandBuffer commandBuffer, Lut lut, uint32_t x, uint32_t y, uint32_t z) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::array<LutImage, kLutCount> m_images{};
    std::array<VkPipeline, kLutCount> m_pipelines{};
    bool m_imagesInitialized = false;
    // The parameters the static LUTs were last built from; empty until the first build.
    std::optional<AtmosphereParameters> m_staticLutParameters;
};
}
```

`engine/renderer/vulkan/atmosphere.cpp`:

```cpp
#include "atmosphere.h"

#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>

#include <stdexcept>

namespace me
{

namespace
{
constexpr VkFormat kLutFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Must match the sizes in shaders/vulkan/atmosphere_common.glsl.
constexpr std::array<VkExtent3D, 4> kLutExtents = {
    VkExtent3D{256, 64, 1},
    VkExtent3D{32, 32, 1},
    VkExtent3D{192, 108, 1},
    VkExtent3D{32, 32, 32}};
constexpr std::array<const char*, 4> kShaderNames = {
    "atmosphere_transmittance.comp.spv",
    "atmosphere_multiscattering.comp.spv",
    "atmosphere_skyview.comp.spv",
    "atmosphere_aerial_perspective.comp.spv"};

uint32_t GroupCount(uint32_t size, uint32_t groupSize)
{
    return (size + groupSize - 1) / groupSize;
}

void GlobalBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStage,
    VkAccessFlags srcAccess,
    VkPipelineStageFlags dstStage,
    VkAccessFlags dstAccess)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
}

VulkanAtmosphere::VulkanAtmosphere(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    try
    {
        CreateImages();
        CreateDescriptors();
        CreatePipelines(pipelineCache, frameSetLayout);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanAtmosphere::~VulkanAtmosphere()
{
    DestroyHandles();
}

TextureDescriptorBinding VulkanAtmosphere::GetTransmittanceBinding() const
{
    return TextureDescriptorBinding{m_images[kTransmittance].view, m_sampler};
}

TextureDescriptorBinding VulkanAtmosphere::GetSkyViewBinding() const
{
    return TextureDescriptorBinding{m_images[kSkyView].view, m_sampler};
}

TextureDescriptorBinding VulkanAtmosphere::GetAerialPerspectiveBinding() const
{
    return TextureDescriptorBinding{m_images[kAerialPerspective].view, m_sampler};
}

void VulkanAtmosphere::Record(VkCommandBuffer commandBuffer, VkDescriptorSet frameDescriptorSet, const AtmosphereParameters* parameters)
{
    if (!m_imagesInitialized)
    {
        std::array<VkImageMemoryBarrier, kLutCount> barriers{};
        for (size_t lut = 0; lut < kLutCount; ++lut)
        {
            VkImageMemoryBarrier& barrier = barriers[lut];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = m_images[lut].image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            static_cast<uint32_t>(barriers.size()),
            barriers.data());
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (const LutImage& image : m_images)
        {
            vkCmdClearColorImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        }
        m_imagesInitialized = true;
    }

    // The previous frame's fragment reads (and this frame's clear) before this frame's writes.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    if (parameters != nullptr)
    {
        const std::array<VkDescriptorSet, 2> sets = {frameDescriptorSet, m_descriptorSet};
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            m_pipelineLayout,
            0,
            static_cast<uint32_t>(sets.size()),
            sets.data(),
            0,
            nullptr);

        if (!m_staticLutParameters.has_value() || !(*m_staticLutParameters == *parameters))
        {
            Dispatch(commandBuffer, kTransmittance, GroupCount(256, 8), GroupCount(64, 8), 1);
            GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            Dispatch(commandBuffer, kMultiScattering, 32, 32, 1);
            GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            m_staticLutParameters = *parameters;
        }
        Dispatch(commandBuffer, kSkyView, GroupCount(192, 8), GroupCount(108, 8), 1);
        Dispatch(commandBuffer, kAerialPerspective, GroupCount(32, 8), GroupCount(32, 8), 32);
    }

    // This frame's writes before its fragment shaders sample them.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
}

void VulkanAtmosphere::Dispatch(VkCommandBuffer commandBuffer, Lut lut, uint32_t x, uint32_t y, uint32_t z) const
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines[lut]);
    vkCmdDispatch(commandBuffer, x, y, z);
}

void VulkanAtmosphere::CreateImages()
{
    for (size_t lut = 0; lut < kLutCount; ++lut)
    {
        const VkExtent3D extent = kLutExtents[lut];
        const bool volume = extent.depth > 1;
        LutImage& image = m_images[lut];

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        imageInfo.extent = extent;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = kLutFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &image.image), "Failed to create an atmosphere LUT");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, image.image, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &image.memory), "Failed to allocate an atmosphere LUT");
        CheckVulkan(vkBindImageMemory(m_device, image.image, image.memory, 0), "Failed to bind an atmosphere LUT");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image.image;
        viewInfo.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kLutFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &image.view), "Failed to create an atmosphere LUT view");
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    CheckVulkan(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler), "Failed to create the atmosphere sampler");
}

void VulkanAtmosphere::CreateDescriptors()
{
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
    for (uint32_t binding = 0; binding < bindings.size(); ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = binding < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout), "Failed to create the atmosphere set layout");

    const std::array<VkDescriptorPoolSize, 2> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create the atmosphere descriptor pool");

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &m_setLayout;
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, &m_descriptorSet), "Failed to allocate the atmosphere descriptor set");

    std::array<VkDescriptorImageInfo, 6> infos{};
    for (size_t lut = 0; lut < kLutCount; ++lut)
    {
        infos[lut] = VkDescriptorImageInfo{VK_NULL_HANDLE, m_images[lut].view, VK_IMAGE_LAYOUT_GENERAL};
    }
    infos[4] = VkDescriptorImageInfo{m_sampler, m_images[kTransmittance].view, VK_IMAGE_LAYOUT_GENERAL};
    infos[5] = VkDescriptorImageInfo{m_sampler, m_images[kMultiScattering].view, VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 6> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding)
    {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = m_descriptorSet;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = bindings[binding].descriptorType;
        writes[binding].pImageInfo = &infos[binding];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void VulkanAtmosphere::CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout)
{
    const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, m_setLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    CheckVulkan(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout), "Failed to create the atmosphere pipeline layout");

    for (size_t lut = 0; lut < kLutCount; ++lut)
    {
        const VulkanShaderModule shader(m_device, EnginePaths::ShaderRoot() / kShaderNames[lut]);
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader.GetHandle();
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = m_pipelineLayout;
        CheckVulkan(
            vkCreateComputePipelines(m_device, pipelineCache, 1, &pipelineInfo, nullptr, &m_pipelines[lut]),
            "Failed to create an atmosphere pipeline");
    }
}

uint32_t VulkanAtmosphere::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
    for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
    {
        if ((typeFilter & (1u << index)) != 0 && (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties)
        {
            return index;
        }
    }
    throw std::runtime_error("Failed to find a memory type for the atmosphere LUTs");
}

void VulkanAtmosphere::DestroyHandles()
{
    for (VkPipeline& pipeline : m_pipelines)
    {
        if (pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorSet = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    for (LutImage& image : m_images)
    {
        if (image.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, image.view, nullptr);
        }
        if (image.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_device, image.image, nullptr);
        }
        if (image.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, image.memory, nullptr);
        }
        image = LutImage{};
    }
}
}
```

Add `vulkan/atmosphere.cpp`/`vulkan/atmosphere.h` to `engine_renderer` (alphabetical, first after `rhi/...`, before `vulkan/buffer.cpp`: place after `vulkan/ao_pass.h`).

- [ ] **Step 7: Renderer wiring.** `renderer.h`: include `"atmosphere.h"`; members `std::unique_ptr<VulkanAtmosphere> m_atmosphere;` and `std::unique_ptr<VulkanTexture> m_defaultEnvironmentMap;` (beside `m_shadowPass`); private methods `EnvironmentDescriptorBindings BuildEnvironmentBindings() const;` and `EnvironmentMode EffectiveEnvironmentMode(const SceneEnvironment& environment) const;`.

`CreateDeviceResources`, after the shadow pass:

```cpp
    m_atmosphere = std::make_unique<VulkanAtmosphere>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_pipelineCache,
        m_frameSetLayout->GetHandle());

    // Set 0 binding 6 must name a valid image even when no HDRI is loaded.
    VulkanUploadBatch uploadBatch(
        m_device->GetHandle(),
        m_device->GetQueueFamilies().graphicsFamily.value(),
        m_device->GetGraphicsQueue());
    FloatTextureData black{};
    black.width = 1;
    black.height = 1;
    black.pixels = {0.0f, 0.0f, 0.0f, 1.0f};
    m_defaultEnvironmentMap = std::make_unique<VulkanTexture>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        black,
        uploadBatch);
    uploadBatch.Flush();
```

`DestroyDeviceResources`, first lines: `m_defaultEnvironmentMap.reset(); m_atmosphere.reset();`.

```cpp
EnvironmentDescriptorBindings VulkanRenderer::BuildEnvironmentBindings() const
{
    EnvironmentDescriptorBindings bindings{};
    bindings.transmittance = m_atmosphere->GetTransmittanceBinding();
    bindings.skyView = m_atmosphere->GetSkyViewBinding();
    bindings.aerialPerspective = m_atmosphere->GetAerialPerspectiveBinding();
    bindings.environmentMap = TextureDescriptorBinding{m_defaultEnvironmentMap->GetImageView(), m_defaultEnvironmentMap->GetSampler()};
    return bindings;
}

// The mode the frame renders with. An HDRI that is not loaded renders as None (Task 7 loads it).
EnvironmentMode VulkanRenderer::EffectiveEnvironmentMode(const SceneEnvironment& environment) const
{
    return environment.mode == EnvironmentMode::Hdri ? EnvironmentMode::None : environment.mode;
}
```

Both `VulkanUniformBuffer` constructions (`CreateDescriptorResources` and `ApplyRenderContent`) pass `BuildEnvironmentBindings()` after `m_shadowPass->GetSampledBinding()`.

In `DrawFrame`, after the shadow block (after `shadowData` is filled) and before `m_uniformBuffer->Update`:

```cpp
    const SceneEnvironment environment = State().editorWorld ? EditorWorld().GetEnvironment() : SceneEnvironment{};
    const EnvironmentMode environmentMode = EffectiveEnvironmentMode(environment);
    const AtmosphereParameters atmosphereParameters = BuildAtmosphereParameters(environment.atmosphere);
    std::optional<AtmosphereSun> sun;
    if (shadowLightIndex >= 0)
    {
        GpuLightData& sunLight = selectedLights[static_cast<size_t>(shadowLightIndex)];
        sun = AtmosphereSun{
            -glm::normalize(glm::vec3(sunLight.directionAndType)),
            glm::vec3(sunLight.colorAndIntensity) * sunLight.colorAndIntensity.w};
        if (environmentMode == EnvironmentMode::Atmosphere)
        {
            // The light's intensity is the illuminance above the atmosphere; the scene receives
            // what gets through to the camera's altitude.
            const glm::vec3 camera = ToAtmosphereCameraPositionKm(atmosphereParameters, State().camera.position);
            const float cosZenith = glm::dot(sun->directionToSun, glm::normalize(camera));
            const glm::vec3 transmittance = ComputeTransmittanceToSpace(
                atmosphereParameters,
                glm::length(camera) - atmosphereParameters.bottomRadiusKm,
                cosZenith);
            sunLight.colorAndIntensity = glm::vec4(glm::vec3(sunLight.colorAndIntensity) * transmittance, sunLight.colorAndIntensity.w);
        }
    }
    const EnvironmentUniformData environmentData = BuildEnvironmentUniformData(
        environmentMode,
        environment,
        atmosphereParameters,
        sun,
        State().camera.position);
```

(`EditorWorld()` is the accessor the renderer already uses; `selectedLights` must be non-const here — it is a local `std::vector`.) Pass `environmentData` as the new last argument of `m_uniformBuffer->Update`. In the command buffer lambda, after `m_shadowPass->Record(...)` and before `RecordScenePasses`:

```cpp
                                              // Ahead of the scene passes, whose fragment shaders sample the
                                              // LUTs; it orders itself with its own barriers (see
                                              // VulkanAtmosphere).
                                              m_atmosphere->Record(
                                                  commandBuffer,
                                                  frame.frameDescriptorSet,
                                                  environmentMode == EnvironmentMode::Atmosphere ? &atmosphereParameters : nullptr);
```

- [ ] **Step 8: Verify.** Configure, build Debug, full ctest, format check. Validation runs, each with zero messages:
  1. `--frames 300` (startup scene: atmosphere on).
  2. `--frames 300 --model assets\Sponza\Sponza.gltf` with enough frames for textures (use `--frames 20000`).
  3. A scene file with `environment: {mode: none}`: write `%TEMP%\sky_none.yaml` with the YAML from Task 2's emitter (save the startup scene once via a throwaway copy of the test, or hand-write `scene: {version: 3}`, `entities: []`, `lights: []`, `environment: {mode: none}`) and run `--scene %TEMP%\sky_none.yaml --frames 300`.
  The picture is unchanged in this task (nothing samples the LUTs yet), apart from the sun now being tinted: a `--capture` of the startup scene should show slightly warmer cube faces than before this task.

- [ ] **Step 9: Commit.**

```bash
git add shaders/vulkan/scene_common.glsl shaders/vulkan/atmosphere_common.glsl shaders/vulkan/atmosphere_integrate.glsl shaders/vulkan/atmosphere_transmittance.comp shaders/vulkan/atmosphere_multiscattering.comp shaders/vulkan/atmosphere_skyview.comp shaders/vulkan/atmosphere_aerial_perspective.comp engine/renderer/vulkan/atmosphere.h engine/renderer/vulkan/atmosphere.cpp engine/renderer/vulkan/uniform_buffer.h engine/renderer/vulkan/uniform_buffer.cpp engine/renderer/vulkan/texture.h engine/renderer/vulkan/texture.cpp engine/renderer/vulkan/renderer.h engine/renderer/vulkan/renderer.cpp engine/renderer/CMakeLists.txt
git commit -m "feat(renderer): Hillaire 2020 atmosphere LUTs

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Sky and sun disk

**Files:**
- Create: `shaders/vulkan/atmosphere_sampling.glsl`, `shaders/vulkan/sky.vert`, `shaders/vulkan/sky.frag`
- Modify: `engine/renderer/vulkan/pipeline.h`, `engine/renderer/vulkan/pipeline.cpp`, `engine/renderer/vulkan/forward_pass.h`, `engine/renderer/vulkan/forward_pass.cpp`, `engine/renderer/vulkan/renderer.cpp` (`CreateScenePasses`), `engine/renderer/CMakeLists.txt`

**Interfaces:**
- Consumes: set 0 bindings 3 to 6, the environment block, `atmosphere_common.glsl` (Task 4).
- Produces: GLSL `ViewDirectionFromTexCoord(vec2)`, `SampleSky(vec3)`, `SampleEnvironmentMap(vec3)`; `FullscreenPipelineOptions`; `VulkanForwardPass(VkDevice, VkPipelineCache, const SceneRenderTargets&, VkDescriptorSetLayout frameSetLayout)`.

- [ ] **Step 1: Fragment-side sampling.** `shaders/vulkan/atmosphere_sampling.glsl`:

```glsl
// What the fragment shaders read from the environment: the sky (Hillaire 2020 sky-view LUT plus
// the sun disk), the HDRI, and aerial perspective. Include after scene_common.glsl.
#ifndef ATMOSPHERE_SAMPLING_GLSL
#define ATMOSPHERE_SAMPLING_GLSL

#include "atmosphere_common.glsl"

// Set 0 bindings 3 to 6; see VulkanFrameDescriptorSetLayout.
layout(set = 0, binding = 3) uniform sampler2D atmosphereTransmittanceLut;
layout(set = 0, binding = 4) uniform sampler2D atmosphereSkyViewLut;
layout(set = 0, binding = 5) uniform sampler3D atmosphereAerialPerspective;
layout(set = 0, binding = 6) uniform sampler2D environmentMap;

// The world direction through a full-screen texture coordinate (origin top left, as
// fullscreen.vert emits it), from the camera toward the far plane.
vec3 ViewDirectionFromTexCoord(vec2 texCoord)
{
    vec4 farPoint = ubo.invViewProj * vec4(texCoord * 2.0 - 1.0, 1.0, 1.0);
    return normalize(farPoint.xyz / farPoint.w - ubo.cameraWorldPosition.xyz);
}

// The sun's disk, if direction falls inside it: its illuminance spread over its solid angle,
// dimmed by the atmosphere along the view ray and darkened toward the limb.
vec3 SunDisk(vec3 direction, vec3 up, float viewHeight)
{
    vec3 sunDirection = ubo.sunDirectionAndMode.xyz;
    float cosRadius = ubo.sunIlluminance.w;
    if (dot(direction, sunDirection) < cosRadius)
    {
        return vec3(0.0);
    }
    vec3 transmittance = SampleTransmittance(atmosphereTransmittanceLut, viewHeight, dot(direction, up));
    float solidAngle = 2.0 * ATMOSPHERE_PI * (1.0 - cosRadius);
    // asin of the cross product's length stays precise at the tiny angles inside the disk, where
    // acos of the dot product does not.
    float angle = asin(min(length(cross(direction, sunDirection)), 1.0));
    float radius = acos(cosRadius);
    float centerToEdge = clamp(angle / radius, 0.0, 1.0);
    float mu = sqrt(max(0.0, 1.0 - centerToEdge * centerToEdge));
    float limbDarkening = 1.0 - 0.6 * (1.0 - sqrt(mu));
    return ubo.sunIlluminance.rgb / solidAngle * transmittance * limbDarkening;
}

// Sky luminance in cd/m^2 seen from the camera along a world direction.
vec3 SampleSky(vec3 direction)
{
    vec3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float viewHeight = length(camera);
    vec3 up = camera / viewHeight;
    float viewZenithCos = dot(direction, up);

    vec3 sunDirection = ubo.sunDirectionAndMode.xyz;
    float lightViewCos = 1.0;
    vec3 side = cross(up, direction);
    if (dot(side, side) > 1e-10)
    {
        side = normalize(side);
        vec3 forward = normalize(cross(side, up));
        vec2 sunOnPlane = vec2(dot(sunDirection, forward), dot(sunDirection, side));
        float length2 = dot(sunOnPlane, sunOnPlane);
        lightViewCos = length2 > 1e-12 ? sunOnPlane.x * inversesqrt(length2) : 1.0;
    }

    bool intersectGround = RaySphereIntersectNearest(camera, direction, vec3(0.0), BottomRadius()) >= 0.0;
    vec2 uv = SkyViewLutParamsToUv(intersectGround, viewZenithCos, lightViewCos, viewHeight);
    vec3 luminance = textureLod(atmosphereSkyViewLut, uv, 0.0).rgb;
    if (!intersectGround)
    {
        luminance += SunDisk(direction, up, viewHeight);
    }
    return luminance;
}

// The equirectangular HDRI along a world direction: u = 0.5 at -Z, the default camera's view,
// growing toward +X; v = 0 straight up. hdriParameters: x intensity, y rotation in turns.
vec3 SampleEnvironmentMap(vec3 direction)
{
    float u = 0.5 + atan(direction.x, -direction.z) / (2.0 * ATMOSPHERE_PI) + ubo.hdriParameters.y;
    float v = acos(clamp(direction.y, -1.0, 1.0)) / ATMOSPHERE_PI;
    return textureLod(environmentMap, vec2(u, v), 0.0).rgb * ubo.hdriParameters.x;
}

#endif
```

- [ ] **Step 2: Sky shaders.** `shaders/vulkan/sky.vert`:

```glsl
#version 450

// fullscreen.vert's triangle, placed on the far plane: with a LESS_OR_EQUAL depth test it covers
// exactly the pixels no geometry wrote, whose depth is still the 1.0 they were cleared to.
layout(location = 0) out vec2 fragTexCoord;

void main()
{
    const vec2 position = vec2(
        (gl_VertexIndex == 1) ? 3.0 : -1.0,
        (gl_VertexIndex == 2) ? 3.0 : -1.0);
    fragTexCoord = position * 0.5 + 0.5;
    gl_Position = vec4(position, 1.0, 1.0);
}
```

`shaders/vulkan/sky.frag`:

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"

// Must match the push constant VulkanForwardPass::RecordSky pushes.
layout(push_constant) uniform SkyConstants
{
    // xyz = GetBackgroundRadiance(exposure), the flat background of EnvironmentMode::None.
    vec4 backgroundRadiance;
}
skyData;

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    uint mode = EnvironmentMode();
    if (mode == ENVIRONMENT_NONE)
    {
        outColor = vec4(skyData.backgroundRadiance.rgb, 1.0);
        return;
    }
    vec3 direction = ViewDirectionFromTexCoord(fragTexCoord);
    vec3 luminance = mode == ENVIRONMENT_HDRI ? SampleEnvironmentMap(direction) : SampleSky(direction);
    // The sun disk alone is ~1e9 cd/m^2, past half float; the target must never hold infinity.
    outColor = vec4(min(luminance, vec3(65504.0)), 1.0);
}
```

Add `sky.vert`, `sky.frag` to `MINIENGINE_SHADER_SOURCES` and `atmosphere_sampling.glsl` to the includes.

- [ ] **Step 3: Pipeline options.** `pipeline.h`:

```cpp
// What varies between full-screen pipelines beyond the fragment stage.
struct FullscreenPipelineOptions
{
    const char* vertexShaderName = "fullscreen.vert.spv";
    // Test against the pass's depth attachment with LESS_OR_EQUAL, writing nothing: with sky.vert,
    // which places the triangle at depth 1, that draws only where no geometry did.
    bool depthTestAtFarPlane = false;
};
```

and add a trailing `const FullscreenPipelineOptions& options = {}` to `CreateFullscreenPipeline`. In `pipeline.cpp`, load `shaderDir / options.vertexShaderName`, and set the depth state:

```cpp
    depthStencil.depthTestEnable = options.depthTestAtFarPlane ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
```

Update the function comment (`no depth test` becomes `no depth test unless options ask for it`).

- [ ] **Step 4: The forward pass draws the sky.** `forward_pass.h`: constructor `VulkanForwardPass(VkDevice device, VkPipelineCache pipelineCache, const SceneRenderTargets& targets, VkDescriptorSetLayout frameSetLayout);`; private `void RecordSky(VkCommandBuffer commandBuffer, const ScenePassFrameContext& frame) const;`; members `VkPipelineLayout m_skyPipelineLayout = VK_NULL_HANDLE; VkPipeline m_skyPipeline = VK_NULL_HANDLE;`. Update the class comment: it now also draws the sky between its opaque and blend items.

`forward_pass.cpp`: in the constructor's try block, after the render passes and framebuffers:

```cpp
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.size = sizeof(glm::vec4);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &frameSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstantRange;
        CheckVulkan(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_skyPipelineLayout), "Failed to create sky pipeline layout");
        FullscreenPipelineOptions skyOptions{};
        skyOptions.vertexShaderName = "sky.vert.spv";
        skyOptions.depthTestAtFarPlane = true;
        m_skyPipeline = CreateFullscreenPipeline(
            m_device, pipelineCache, m_clearRenderPass, m_skyPipelineLayout, "sky.frag.spv", "sky", skyOptions);
```

(include `"pipeline.h"`; destroy both handles in `DestroyHandles`). `Record` becomes:

```cpp
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    SetViewportAndScissor(commandBuffer, frame.extent);
    // Opaque and Mask first (only when this pass owns the frame; otherwise the lighting pass drew
    // them), then the sky into whatever no geometry covered, then Blend items over both.
    if (ownsFrame)
    {
        RecordMaterialDrawItems(commandBuffer, *frame.forwardPipelines, frame.frameDescriptorSet, frame.OpaqueDrawItems());
    }
    RecordSky(commandBuffer, frame);
    RecordMaterialDrawItems(commandBuffer, *frame.forwardPipelines, frame.frameDescriptorSet, frame.BlendDrawItems());
    vkCmdEndRenderPass(commandBuffer);
```

```cpp
void VulkanForwardPass::RecordSky(VkCommandBuffer commandBuffer, const ScenePassFrameContext& frame) const
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipelineLayout, 0, 1, &frame.frameDescriptorSet, 0, nullptr);
    const glm::vec4 background(GetBackgroundRadiance(frame.exposure), 1.0f);
    vkCmdPushConstants(commandBuffer, m_skyPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(background), &background);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}
```

Check that the forward render passes' subpass dependencies already cover depth reads by the fragment tests (they do for the material draws, which test depth); nothing changes there. `renderer.cpp` `CreateScenePasses`: `std::make_unique<VulkanForwardPass>(m_device->GetHandle(), m_pipelineCache, *m_sceneTargets, m_frameSetLayout->GetHandle())`.

- [ ] **Step 5: Verify with captures.** Configure, build, ctest, format check. Write three scene files in `%TEMP%` (YAML as Task 2 emits it; two cubes are not needed):
  - `sky_noon.yaml`: `environment: {mode: atmosphere}`; one directional light, intensity 120000, rotation `[30, 0, 0]` (travels toward -Z and down: the sun is 60 degrees up behind the camera).
  - `sky_sunset.yaml`: same with rotation `[-88, 0, 0]` (the sun 2 degrees up in front of the camera, which looks down -Z).
  - `sky_none.yaml`: `environment: {mode: none}`, no lights.
  For each: `miniengine_app.exe --backend vulkan --scene %TEMP%\sky_X.yaml --frames 400 --capture %TEMP%\sky_X.png`, zero validation messages, and look at the PNGs:
  - noon: blue sky, brighter toward the horizon, horizon mid-image;
  - sunset: orange-red glow and a sun disk at the horizon in the centre, blue-grey sky above;
  - none: the old flat background, identical to a capture taken on the previous commit.
  If the sky is black, check the sun (the scene's only directional light) and the mode; if it is uniformly grey or NaN-magenta, suspect the LUT parameterisation and inspect the sky-view LUT by temporarily writing `textureLod(atmosphereSkyViewLut, fragTexCoord, 0).rgb * 1e-4` from `sky.frag`.
  Then flip the editor's forward-only switch in the same scenes (or temporarily force `forwardOnly`) and confirm the capture is identical.

- [ ] **Step 6: Commit.**

```bash
git add shaders/vulkan/atmosphere_sampling.glsl shaders/vulkan/sky.vert shaders/vulkan/sky.frag engine/renderer/vulkan/pipeline.h engine/renderer/vulkan/pipeline.cpp engine/renderer/vulkan/forward_pass.h engine/renderer/vulkan/forward_pass.cpp engine/renderer/vulkan/renderer.cpp engine/renderer/CMakeLists.txt
git commit -m "feat(renderer): sky and sun disk from the atmosphere LUTs

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Aerial perspective

**Files:**
- Modify: `shaders/vulkan/atmosphere_sampling.glsl`, `shaders/vulkan/deferred_lighting.frag`, `shaders/vulkan/triangle.frag`

**Interfaces:**
- Consumes: binding 5 and the environment block.
- Produces: GLSL `ApplyAerialPerspective(vec3 color, vec3 worldPosition) -> vec3`.

- [ ] **Step 1: The lookup.** Append to `atmosphere_sampling.glsl`, before `#endif`:

```glsl
// Hazes a surface's radiance by the atmosphere between it and the camera: color * T + L, from the
// aerial perspective volume at the surface's screen position and view distance (times the
// scene's distance scale). The first half slice fades in from nothing, so surfaces right at the
// camera are untouched. Off unless the environment is the atmosphere.
vec3 ApplyAerialPerspective(vec3 color, vec3 worldPosition)
{
    if (EnvironmentMode() != ENVIRONMENT_ATMOSPHERE)
    {
        return color;
    }
    vec4 clip = ubo.proj * ubo.view * vec4(worldPosition, 1.0);
    vec2 uv = clamp(clip.xy / clip.w * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    float distanceKm = length(worldPosition - ubo.cameraWorldPosition.xyz) * 0.001 * ubo.atmosphereRadii.z;
    float slice = distanceKm / AERIAL_PERSPECTIVE_KM_PER_SLICE;
    float weight = 1.0;
    if (slice < 0.5)
    {
        weight = clamp(slice * 2.0, 0.0, 1.0);
        slice = 0.5;
    }
    float w = sqrt(slice / AERIAL_PERSPECTIVE_SLICE_COUNT);
    vec4 aerialPerspective = textureLod(atmosphereAerialPerspective, vec3(uv, w), 0.0);
    float transmittance = 1.0 - weight * (1.0 - aerialPerspective.a);
    return color * transmittance + aerialPerspective.rgb * weight;
}
```

(The texel centre of slice s is at w = (s + 0.5) / 32 and holds distance w^2 * 32 km, so the lookup coordinate is sqrt(distance / 32).)

- [ ] **Step 2: Apply it.** `deferred_lighting.frag`: `#include "atmosphere_sampling.glsl"` after `scene_common.glsl`, and replace `outColor = vec4(color, 1.0);` with

```glsl
    outColor = vec4(ApplyAerialPerspective(color, worldPosition), 1.0);
```

(keep the comment above it). `triangle.frag`: same include; before `outColor`:

```glsl
    // The atmosphere between the surface and the camera, before blending: an approximation for
    // Blend items, exact for the forward-only order's opaque ones.
    color = ApplyAerialPerspective(color, fragWorldPosition);
```

- [ ] **Step 3: Verify.** Build, ctest, format check, validation run. Captures:
  - `ap_sponza.yaml`: atmosphere mode, `aerial_perspective_distance_scale: 100`, the noon sun, and one entity whose model `source_path` is the absolute path of `assets/Sponza/Sponza.gltf` (the entity YAML shape is in `EmitSceneYaml`). Capture with `--frames 20000` (textures load in the background). The far end of the hall must be hazed toward the sky colour; the same scene with scale 1 must look like the scene without aerial perspective (compare with a capture where the mode is `none` and the sun is the same; only the sky and the sun tint differ).
  - Forward-only on the same scene gives the same picture.

- [ ] **Step 4: Commit.**

```bash
git add shaders/vulkan/atmosphere_sampling.glsl shaders/vulkan/deferred_lighting.frag shaders/vulkan/triangle.frag
git commit -m "feat(renderer): aerial perspective

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: HDRI environment maps

**Files:**
- Modify: `engine/renderer/vulkan/uniform_buffer.h`, `engine/renderer/vulkan/uniform_buffer.cpp`, `engine/renderer/vulkan/renderer.h`, `engine/renderer/vulkan/renderer.cpp`

**Interfaces:**
- Consumes: `VulkanTexture(..., const FloatTextureData&, ...)` (Task 4), `TextureLoader::LoadRGBA32F`, `SampleEnvironmentMap` (Task 5).
- Produces: `VulkanUniformBuffer::SetEnvironmentMap(TextureDescriptorBinding)`; renderer members `m_environmentMap`, `m_environmentMapPath`, `m_pendingEnvironmentMap`, `m_pendingEnvironmentMapPath`, `m_failedEnvironmentMapPath`; `VulkanRenderer::UpdateEnvironmentMap(const SceneEnvironment&)`.

- [ ] **Step 1: Rebinding the map.** `VulkanUniformBuffer`:

```cpp
    // Points set 0 binding 6 of every frame set at another environment map. The caller has waited
    // for every frame in flight: the sets must not be in use while they are written.
    void SetEnvironmentMap(TextureDescriptorBinding environmentMap);
```

```cpp
void VulkanUniformBuffer::SetEnvironmentMap(TextureDescriptorBinding environmentMap)
{
    m_environment.environmentMap = environmentMap;
    const VkDescriptorImageInfo info{environmentMap.sampler, environmentMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (VkDescriptorSet set : m_frameDescriptorSets)
    {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 6;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
}
```

- [ ] **Step 2: Loading.** `renderer.h`: include `<future>`; members

```cpp
    // The loaded HDRI and the scene path it came from; empty until one loads.
    std::unique_ptr<VulkanTexture> m_environmentMap;
    std::string m_environmentMapPath;
    // A decode running on a worker thread, and the path it decodes.
    std::future<FloatTextureData> m_pendingEnvironmentMap;
    std::string m_pendingEnvironmentMapPath;
    // The last path that failed, so a bad file is reported once rather than every frame.
    std::string m_failedEnvironmentMapPath;
```

and `void UpdateEnvironmentMap(const SceneEnvironment& environment);`. In `renderer.cpp`:

```cpp
void VulkanRenderer::UpdateEnvironmentMap(const SceneEnvironment& environment)
{
    if (environment.mode != EnvironmentMode::Hdri || environment.hdri.path.empty())
    {
        return;
    }
    const std::string& wanted = environment.hdri.path;

    if (m_pendingEnvironmentMap.valid())
    {
        if (m_pendingEnvironmentMap.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;
        }
        const std::string path = m_pendingEnvironmentMapPath;
        try
        {
            const FloatTextureData image = m_pendingEnvironmentMap.get();
            if (path == wanted)
            {
                VulkanUploadBatch uploadBatch(
                    m_device->GetHandle(),
                    m_device->GetQueueFamilies().graphicsFamily.value(),
                    m_device->GetGraphicsQueue());
                auto texture = std::make_unique<VulkanTexture>(
                    m_device->GetPhysicalDevice(), m_device->GetHandle(), image, uploadBatch);
                uploadBatch.Flush();
                // The frame sets name the old map until rewritten, and may be in use.
                m_commandContext->WaitForAllFrames();
                if (m_uniformBuffer)
                {
                    m_uniformBuffer->SetEnvironmentMap(TextureDescriptorBinding{texture->GetImageView(), texture->GetSampler()});
                }
                m_environmentMap = std::move(texture);
                m_environmentMapPath = path;
                LOG_INFO("Loaded HDRI '{}' ({}x{})", path, image.width, image.height);
            }
        }
        catch (const std::exception& error)
        {
            LOG_ERROR("Failed to load HDRI '{}': {}", path, error.what());
            m_failedEnvironmentMapPath = path;
        }
        m_pendingEnvironmentMapPath.clear();
    }

    if (wanted == m_environmentMapPath || wanted == m_failedEnvironmentMapPath)
    {
        return;
    }
    m_pendingEnvironmentMapPath = wanted;
    m_pendingEnvironmentMap = std::async(
        std::launch::async,
        [path = wanted]()
        {
            return TextureLoader::LoadRGBA32F(path);
        });
}
```

`EffectiveEnvironmentMode` becomes:

```cpp
// The mode the frame renders with: an HDRI that is still loading, or failed to, renders as None.
EnvironmentMode VulkanRenderer::EffectiveEnvironmentMode(const SceneEnvironment& environment) const
{
    if (environment.mode != EnvironmentMode::Hdri)
    {
        return environment.mode;
    }
    const bool loaded = m_environmentMap && !environment.hdri.path.empty() && environment.hdri.path == m_environmentMapPath;
    return loaded ? EnvironmentMode::Hdri : EnvironmentMode::None;
}
```

`BuildEnvironmentBindings` uses `m_environmentMap` when set, else the default. In `DrawFrame`, call `UpdateEnvironmentMap(environment);` right after `environment` is read and before `EffectiveEnvironmentMode`. In the destructor, before `vkDeviceWaitIdle`, nothing extra is needed: the future's destructor waits for the decode. `DestroyDeviceResources` must also reset `m_environmentMap` (and clear `m_environmentMapPath`) before the device goes.

- [ ] **Step 3: Verify.** Build, ctest, format check. Generate a synthetic equirectangular HDRI in the scratchpad with a short standard-library Python script (flat RGBE `.hdr`, 512x256, header as in the float-texture acceptance script): the pixel column at u = 0.5 (-Z) red (4, 0, 0), u = 0.75 (+X) green (0, 4, 0), u = 0.25 (-X) blue (0, 0, 4), u = 0 (+Z) white (4, 4, 4); the top rows (v < 0.1) yellow. Scene `hdri_test.yaml`: `environment: {mode: hdri, hdri: {path: <absolute path>, intensity: 1000, rotation_degrees: 0}}`, no lights. Captures:
  - rotation 0: red at the centre of the image, yellow toward the top edge only if the camera looks up (it does not: expect none);
  - rotation 90: the centre samples u = 0.5 + 0.25 = 0.75, so it shows green (what +X showed at rotation 0);
  - a missing path: the log reports `Failed to load HDRI` once, and the capture is the flat background.
  Then a real Poly Haven `.hdr` and a `.exr` if one is available locally (ask the user for a path only if none is found under `assets/`); zero validation messages throughout.

- [ ] **Step 4: Commit.**

```bash
git add engine/renderer/vulkan/uniform_buffer.h engine/renderer/vulkan/uniform_buffer.cpp engine/renderer/vulkan/renderer.h engine/renderer/vulkan/renderer.cpp
git commit -m "feat(renderer): HDRI environment maps

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Environment editor

**Files:**
- Modify: `engine/editor/ui/editor_scene_panel.cpp`

**Interfaces:**
- Consumes: `IEditorWorld::GetEnvironment/SetEnvironment` (Task 2), `OpenTextureFileDialog()`, `AssetRegistry::GetOrCreateUuid`.

- [ ] **Step 1: The section.** In `editor_scene_panel.cpp`'s anonymous namespace (includes: `<engine/asset/asset_registry.h>`, `<engine/platform/file_dialog/file_dialog.h>` if not already present):

```cpp
bool SceneHasDirectionalLight(const IEditorWorld& scene)
{
    bool found = false;
    scene.ForEachLight(
        [&](entt::entity, const TagComponent&, const TransformComponent&, const LightComponent& light)
        {
            found = found || light.type == LightType::Directional;
        });
    return found;
}

// The scene's sky. Edits a copy and writes it back only when something changed.
void DrawEnvironmentEditor(IEditorWorld& scene)
{
    SceneEnvironment environment = scene.GetEnvironment();

    static constexpr std::array<const char*, 3> kModes = {"None", "Atmosphere", "HDRI"};
    int mode = static_cast<int>(environment.mode);
    if (ImGui::Combo("Sky", &mode, kModes.data(), static_cast<int>(kModes.size())))
    {
        environment.mode = static_cast<EnvironmentMode>(mode);
    }

    if (environment.mode == EnvironmentMode::Atmosphere)
    {
        AtmosphereSettings& atmosphere = environment.atmosphere;
        if (!SceneHasDirectionalLight(scene))
        {
            ImGui::TextDisabled("Add a Directional light: it is the sun.");
        }
        ImGui::ColorEdit3("Ground albedo", &atmosphere.groundAlbedo.x);
        ImGui::DragFloat("Rayleigh density", &atmosphere.rayleighDensityScale, 0.01f, 0.0f, 10.0f, "%.2f");
        ImGui::DragFloat("Mie density", &atmosphere.mieDensityScale, 0.01f, 0.0f, 10.0f, "%.2f");
        ImGui::SliderFloat("Mie anisotropy", &atmosphere.mieAnisotropy, 0.0f, 0.99f, "%.2f");
        ImGui::DragFloat("Ozone density", &atmosphere.ozoneDensityScale, 0.01f, 0.0f, 10.0f, "%.2f");
        ImGui::DragFloat(
            "Aerial perspective scale",
            &atmosphere.aerialPerspectiveDistanceScale,
            1.0f,
            0.0f,
            10000.0f,
            "%.1f",
            ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Sun disk (deg)", &atmosphere.sunAngularDiameterDegrees, 0.1f, 5.0f, "%.3f");
    }
    else if (environment.mode == EnvironmentMode::Hdri)
    {
        HdriSettings& hdri = environment.hdri;
        ImGui::TextWrapped("%s", hdri.path.empty() ? "<no HDRI>" : hdri.path.c_str());
        if (ImGui::Button("Choose HDRI..."))
        {
            if (const std::optional<std::string> path = OpenTextureFileDialog(); path.has_value())
            {
                hdri.path = *path;
                hdri.uuid = AssetRegistry::GetOrCreateUuid(*path);
            }
        }
        ImGui::DragFloat("Intensity (cd/m2)", &hdri.intensity, 10.0f, 0.0f, 1000000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Rotation (deg)", &hdri.rotationDegrees, -180.0f, 180.0f, "%.1f");
    }

    if (!(environment == scene.GetEnvironment()))
    {
        scene.SetEnvironment(environment);
    }
}
```

(Check `ForEachLight`'s callback signature and the `AssetRegistry` namespace spelling against their headers.) In `DrawScenePanel`, just before the `// Scene I/O — always visible` separator:

```cpp
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawEnvironmentEditor(scene);
        }
```

- [ ] **Step 2: Verify.** Build, ctest, format check, validation run. Interactive checks are the user's (Task 9 lists them); a scene saved from the editor after changing the mode reloads with it (save, then load the file with `--scene` and capture).

- [ ] **Step 3: Commit.**

```bash
git add engine/editor/ui/editor_scene_panel.cpp
git commit -m "feat(editor): environment section in the scene panel

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Measurement, documentation, acceptance

**Files:**
- Modify: `README.md`, `docs/superpowers/specs/2026-09-23-sky-atmosphere-design.md`

- [ ] **Step 1: GPU cost (throwaway, never committed).** Temporarily create a timestamp query pool (4 queries) in the renderer, write timestamps around `m_atmosphere->Record` (one frame with the static LUTs rebuilt, then steady frames), read them after `vkDeviceWaitIdle` at shutdown with `timestampPeriod`, and log: static LUTs, sky-view plus aerial perspective. Run the startup scene at a 1920x1080 window with `--frames 2000`. Record the numbers, then `git checkout` the renderer files to discard the instrumentation.

- [ ] **Step 2: README.** A paragraph in the rendering section, after the float texture line: 场景环境（None / Atmosphere / HDRI，随场景保存）；Hillaire 2020 大气：四张 LUT 的尺寸与更新时机、太阳取投射阴影的平行光且强度为大气层顶照度、着色时乘相机高度处的透射率、太阳圆盘与临边昏暗、空气透视及距离缩放；HDRI 的方向约定（图像中心朝 -Z）与强度单位；新场景默认 Atmosphere 并带一盏 120000 lux 的太阳；测得的 GPU 开销；设计文档链接。Also mention `--capture`.

- [ ] **Step 3: Spec amendments.** Add `> **Amended during implementation.**` notes where the implementation differs: aerial perspective slices are spread quadratically over 32 km (Hillaire's reference), not 1 km apart; an HDRI change rewrites binding 6 after `WaitForAllFrames` instead of rebuilding `VulkanUniformBuffer`, and renders as `None` until the map is loaded; the editor has no scene-dirty tracking, so decision 13's "mark the scene dirty" does not apply; the startup scene gains a 120000 lux sun; plus anything else that changed while executing.

- [ ] **Step 4: Final verification.** Full build, full ctest, format check, validation runs of the startup scene, Sponza, and the three `sky_*.yaml` scenes. Captures of noon, sunset, aerial perspective and HDRI kept in the scratchpad for the report.

- [ ] **Step 5: Commit.**

```bash
git add README.md docs/superpowers/specs/2026-09-23-sky-atmosphere-design.md
git commit -m "docs: sky and atmosphere

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 6: Report to the user** in Chinese: what was built, measured costs, the captures, and a manual acceptance checklist (mode switching in the Environment section, dragging the sun's rotation from noon to below the horizon, the aerial perspective slider on Sponza, choosing an HDRI and rotating it, saving and reloading a scene, zero validation messages).
