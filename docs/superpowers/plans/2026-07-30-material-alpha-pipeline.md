# Material Alpha Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Carry editable Opaque/Mask/Blend semantics from glTF and material sidecars through the CPU render world into six correct Vulkan pipeline variants, with stable submesh-level transparent sorting.

**Architecture:** Put alpha semantics and material-definition I/O in backend-independent scene/asset units, then expose a pure render-core pipeline policy and draw-order helper that can be tested without Vulkan. The Vulkan backend consumes those policies through an all-or-nothing six-variant `VulkanPipelineSet`; editor controls and CPU preview use the same shared alpha rules.

**Tech Stack:** C++20, CMake/CTest, yaml-cpp, tinygltf, GLM, Dear ImGui, Vulkan 1.x, GLSL 450/glslc, Visual Studio 2026 x64 Debug.

## Global Constraints

- Execute in an isolated worktree created with `superpowers:using-git-worktrees`; the root checkout contains protected unrelated changes.
- Do not stage, overwrite, clean, stash, or commit the root checkout's `README.md`, `imgui.ini`, `.claude/`, or `docs/superpowers/plans/2026-07-26-combined-transform-rotation-gizmo.md`.
- The root `README.md` already contains protected uncommitted development-log work that is absent from a new worktree. Prepare the final README hunk during Task 5, but apply or commit it only after the user separately authorizes how to reconcile that protected file.
- `MaterialAlphaMode` is backend-independent and has exactly `Opaque`, `Mask`, and `Blend`.
- Missing or invalid persisted alpha mode falls back to Opaque; missing cutoff defaults to `0.5`.
- Runtime cutoff and opacity are clamped to `[0, 1]`.
- Imported glTF base-color alpha remains in `baseColorFactor[3]`; imported `opacity` is `1.0`, so alpha is not squared.
- Opaque: blending off, depth test/write on, no discard.
- Mask: blending off, depth test/write on, discard below cutoff.
- Blend: source-alpha blending on, depth test on, depth write off, no discard.
- Keep the current 128-byte `ObjectPushConstants`; continue to carry cutoff in `emissiveFactor.a`.
- Transparent sorting is stable, global across culling variants, back-to-front, and limited to submesh bounds centers.
- Do not introduce dynamic-state extensions, OIT, descriptor indexing, descriptor-layout lifetime changes, frustum culling, memory suballocation, shadows, IBL, or antialiasing.
- Automated checks do not replace the user's final GUI acceptance.

---

## File Structure

### New files

- `engine/scene/material_alpha.cpp`: case-insensitive alpha-mode parsing, canonical string emission, clamping, and CPU coverage resolution.
- `engine/asset/material_definition.h`: conversion and sidecar-I/O interfaces shared by loader and editor.
- `engine/asset/material_definition.cpp`: `ModelMaterialData`/`ModelImportedMaterialInfo` synchronization and complete `.material.yaml` read/write.
- `engine/renderer/material_pipeline.h`: backend-neutral pipeline key/state and draw-sort interfaces.
- `engine/renderer/material_pipeline.cpp`: six-variant state mapping and stable render-queue ordering.
- `engine/renderer/vulkan/pipeline_set.h`: ownership and lookup for six Vulkan pipelines.
- `engine/renderer/vulkan/pipeline_set.cpp`: all-or-nothing creation of all variants.
- `tests/material_alpha_tests.cpp`: focused alpha semantics, sidecar, pipeline-policy, bounds, and ordering regression tests.

### Modified files

- `engine/scene/material_graph.h`: declare `MaterialAlphaMode`, add mode/cutoff to PBR settings.
- `engine/scene/CMakeLists.txt`: compile `material_alpha.cpp`.
- `engine/asset/model_loader.h`: mirror alpha mode/cutoff in raw material data.
- `engine/asset/model_loader.cpp`: synchronize PBR fields and apply material sidecars after glTF load.
- `engine/asset/gltf_model_loader.cpp`: preserve glTF alpha mode/cutoff and stop duplicating alpha into opacity.
- `engine/asset/material_graph_runtime.cpp`: serialize/deserialize alpha fields on PBR graph nodes.
- `engine/asset/CMakeLists.txt`: compile material-definition I/O.
- `engine/editor/services/model_import_service.cpp`: use shared material-definition conversion/write helpers.
- `engine/editor/services/scene_renderables.cpp`: propagate alpha mode, cutoff, and local bounds center.
- `engine/editor/ui/editor_material_graph.cpp`: expose Alpha Mode and conditional Alpha Cutoff controls.
- `engine/editor/ui/editor_model_processor_panel.cpp`: use shared conversion and display resolved alpha settings.
- `engine/editor/ui/editor_model_preview.cpp`: include alpha settings in cache signatures and use shared coverage rules.
- `engine/renderer/material.h`: keep the cutoff packing contract explicit.
- `engine/renderer/renderer_world.h`: carry alpha mode and local bounds center in `CpuRenderSubmesh`.
- `engine/renderer/CMakeLists.txt`: compile render policy and Vulkan pipeline set.
- `engine/renderer/vulkan/pipeline.h`: accept `MaterialPipelineKey`.
- `engine/renderer/vulkan/pipeline.cpp`: apply pure pipeline state and Mask specialization.
- `engine/renderer/vulkan/command.h`: carry `MaterialPipelineKey` per draw item.
- `engine/renderer/vulkan/renderer.h`: carry alpha/bounds per uploaded submesh and own a pipeline set.
- `engine/renderer/vulkan/renderer.cpp`: create/swap the pipeline set, calculate view depth, order draws, and select variants.
- `shaders/vulkan/triangle.frag`: restrict cutoff discard to the Mask specialization.
- `tests/CMakeLists.txt`: register `miniengine.material_alpha`.
- `README.md`: protected root file; Task 5 prepares an exact verified hunk and stops for reconciliation authorization before applying it.

---

### Task 1: Shared Alpha Semantics and glTF Import

**Files:**
- Create: `engine/scene/material_alpha.cpp`
- Modify: `engine/scene/material_graph.h:1-17`
- Modify: `engine/scene/CMakeLists.txt:1-7`
- Modify: `engine/asset/model_loader.h:13-34`
- Modify: `engine/asset/model_loader.cpp:20-62`
- Modify: `engine/asset/gltf_model_loader.cpp:656-718`
- Create: `tests/material_alpha_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: glTF `alphaMode`, `alphaCutoff`, and PBR base-color alpha.
- Produces:
  - `enum class MaterialAlphaMode { Opaque, Mask, Blend };`
  - `std::optional<MaterialAlphaMode> ParseMaterialAlphaMode(std::string_view value);`
  - `const char* ToString(MaterialAlphaMode mode);`
  - `float ClampMaterialAlphaValue(float value);`
  - `float ResolveMaterialCoverageAlpha(MaterialAlphaMode mode, float alpha, float cutoff);`
  - `MaterialPbrSurfaceSettings::alphaMode`
  - `MaterialPbrSurfaceSettings::alphaCutoff`

- [ ] **Step 1: Add the focused test target and failing shared-semantics tests**

Create `tests/material_alpha_tests.cpp` with the initial test harness:

```cpp
#include <material_graph.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool NearlyEqual(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 0.0001f;
}
}

int main()
{
    try
    {
        Require(ParseMaterialAlphaMode("OPAQUE") == MaterialAlphaMode::Opaque, "OPAQUE parse failed");
        Require(ParseMaterialAlphaMode("mask") == MaterialAlphaMode::Mask, "mask parse failed");
        Require(ParseMaterialAlphaMode("Blend") == MaterialAlphaMode::Blend, "Blend parse failed");
        Require(!ParseMaterialAlphaMode("invalid").has_value(), "invalid alpha mode was accepted");
        Require(std::string(ToString(MaterialAlphaMode::Opaque)) == "opaque", "Opaque string mismatch");
        Require(std::string(ToString(MaterialAlphaMode::Mask)) == "mask", "Mask string mismatch");
        Require(std::string(ToString(MaterialAlphaMode::Blend)) == "blend", "Blend string mismatch");

        Require(NearlyEqual(ClampMaterialAlphaValue(-1.0f), 0.0f), "negative alpha was not clamped");
        Require(NearlyEqual(ClampMaterialAlphaValue(2.0f), 1.0f), "alpha above one was not clamped");
        Require(NearlyEqual(
            ResolveMaterialCoverageAlpha(MaterialAlphaMode::Opaque, 0.2f, 0.5f),
            1.0f
        ), "Opaque did not force full coverage");
        Require(NearlyEqual(
            ResolveMaterialCoverageAlpha(MaterialAlphaMode::Mask, 0.49f, 0.5f),
            0.0f
        ), "Mask did not discard below cutoff");
        Require(NearlyEqual(
            ResolveMaterialCoverageAlpha(MaterialAlphaMode::Mask, 0.5f, 0.5f),
            1.0f
        ), "Mask incorrectly discarded at cutoff");
        Require(NearlyEqual(
            ResolveMaterialCoverageAlpha(MaterialAlphaMode::Blend, 0.25f, 0.5f),
            0.25f
        ), "Blend did not preserve alpha");

        std::cout << "material alpha tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "material alpha tests failed: " << error.what() << '\n';
        return 1;
    }
}
```

Append the target to `tests/CMakeLists.txt`:

```cmake
add_executable(miniengine_material_alpha_tests
    material_alpha_tests.cpp
)
miniengine_group_target_sources(miniengine_material_alpha_tests)

target_link_libraries(miniengine_material_alpha_tests
    PRIVATE
        engine_asset
        engine_render_core
)

add_test(
    NAME miniengine.material_alpha
    COMMAND miniengine_material_alpha_tests
)

if(WIN32)
    add_custom_command(TARGET miniengine_material_alpha_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_material_alpha_tests>
            $<TARGET_FILE_DIR:miniengine_material_alpha_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

set_target_properties(miniengine_material_alpha_tests PROPERTIES FOLDER "Tests")
```

- [ ] **Step 2: Configure and run the test to verify the missing API fails**

Run:

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug --target miniengine_material_alpha_tests
```

Expected: compilation fails because `MaterialAlphaMode`,
`ParseMaterialAlphaMode`, `ToString`, `ClampMaterialAlphaValue`, and
`ResolveMaterialCoverageAlpha` do not exist.

- [ ] **Step 3: Add the shared type and implementation**

At the top of `engine/scene/material_graph.h`, add:

```cpp
#include <optional>
#include <string_view>

enum class MaterialAlphaMode
{
    Opaque,
    Mask,
    Blend
};

std::optional<MaterialAlphaMode> ParseMaterialAlphaMode(std::string_view value);
const char* ToString(MaterialAlphaMode mode);
float ClampMaterialAlphaValue(float value);
float ResolveMaterialCoverageAlpha(MaterialAlphaMode mode, float alpha, float cutoff);
```

Add these members to `MaterialPbrSurfaceSettings`:

```cpp
MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
float alphaCutoff = 0.5f;
```

Create `engine/scene/material_alpha.cpp`:

```cpp
#include "material_graph.h"

#include <algorithm>
#include <cctype>
#include <string>

std::optional<MaterialAlphaMode> ParseMaterialAlphaMode(std::string_view value)
{
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    if (normalized == "opaque") return MaterialAlphaMode::Opaque;
    if (normalized == "mask") return MaterialAlphaMode::Mask;
    if (normalized == "blend") return MaterialAlphaMode::Blend;
    return std::nullopt;
}

const char* ToString(MaterialAlphaMode mode)
{
    switch (mode)
    {
    case MaterialAlphaMode::Mask: return "mask";
    case MaterialAlphaMode::Blend: return "blend";
    case MaterialAlphaMode::Opaque:
    default: return "opaque";
    }
}

float ClampMaterialAlphaValue(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

float ResolveMaterialCoverageAlpha(MaterialAlphaMode mode, float alpha, float cutoff)
{
    const float clampedAlpha = ClampMaterialAlphaValue(alpha);
    switch (mode)
    {
    case MaterialAlphaMode::Mask:
        return clampedAlpha < ClampMaterialAlphaValue(cutoff) ? 0.0f : 1.0f;
    case MaterialAlphaMode::Blend:
        return clampedAlpha;
    case MaterialAlphaMode::Opaque:
    default:
        return 1.0f;
    }
}
```

Add `material_alpha.cpp` to `engine_scene` in
`engine/scene/CMakeLists.txt`.

- [ ] **Step 4: Run the focused test green**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_material_alpha_tests
ctest --test-dir .\out\build\vs2026-x64 -C Debug -R miniengine.material_alpha --output-on-failure
```

Expected: `miniengine.material_alpha` passes.

- [ ] **Step 5: Add a failing glTF import regression**

Extend `tests/material_alpha_tests.cpp` with a temporary minimal glTF writer:

```cpp
#include <model_loader.h>

#include <filesystem>
#include <fstream>

std::filesystem::path WriteAlphaModeFixture()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "miniengine_material_alpha_modes.gltf";
    std::ofstream file(path);
    file << R"({
      "asset": { "version": "2.0" },
      "materials": [
        { "name": "opaque", "pbrMetallicRoughness": { "baseColorFactor": [1,1,1,0.25] } },
        { "name": "mask", "alphaMode": "MASK", "alphaCutoff": 0.4,
          "pbrMetallicRoughness": { "baseColorFactor": [1,1,1,0.5] } },
        { "name": "blend", "alphaMode": "BLEND",
          "pbrMetallicRoughness": { "baseColorFactor": [1,1,1,0.75] } }
      ]
    })";
    return path;
}
```

Add assertions before the success print:

```cpp
const std::filesystem::path fixture = WriteAlphaModeFixture();
const LoadedModelData loaded = ModelLoader::LoadModel(fixture.string());
std::error_code removeError;
std::filesystem::remove(fixture, removeError);
Require(loaded.materials.size() == 3, "glTF material count mismatch");
Require(loaded.materials[0].alphaMode == MaterialAlphaMode::Opaque, "default Opaque was lost");
Require(loaded.materials[1].alphaMode == MaterialAlphaMode::Mask, "MASK was lost");
Require(NearlyEqual(loaded.materials[1].alphaCutoff, 0.4f), "MASK cutoff was lost");
Require(loaded.materials[2].alphaMode == MaterialAlphaMode::Blend, "BLEND was lost");
Require(NearlyEqual(loaded.materials[2].baseColor[3], 0.75f), "base alpha changed");
Require(NearlyEqual(loaded.materials[2].opacity, 1.0f), "imported alpha was duplicated into opacity");
```

- [ ] **Step 6: Run the test and verify the importer regression fails**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_material_alpha_tests
ctest --test-dir .\out\build\vs2026-x64 -C Debug -R miniengine.material_alpha --output-on-failure
```

Expected: compilation fails because `ModelMaterialData::alphaMode` is missing,
or the test fails because MASK/BLEND are not retained and Blend opacity is
`0.75`.

- [ ] **Step 7: Carry alpha mode through raw/PBR material data and fix import**

Add to `ModelMaterialData` in `engine/asset/model_loader.h`:

```cpp
MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
float alphaCutoff = 0.5f;
```

In both `BuildPbrSettingsFromMaterial` and `ApplyPbrSettings` in
`engine/asset/model_loader.cpp`, copy `alphaMode`, clamp `alphaCutoff`, and
clamp `opacity`:

```cpp
pbr.alphaMode = material.alphaMode;
pbr.alphaCutoff = ClampMaterialAlphaValue(material.alphaCutoff);
pbr.opacity = ClampMaterialAlphaValue(material.opacity);
```

and:

```cpp
material.alphaMode = pbr.alphaMode;
material.alphaCutoff = ClampMaterialAlphaValue(pbr.alphaCutoff);
material.opacity = ClampMaterialAlphaValue(pbr.opacity);
```

Replace the current alpha block in `BuildMaterialData`:

```cpp
const std::optional<MaterialAlphaMode> parsedAlphaMode =
    ParseMaterialAlphaMode(material.alphaMode);
materialData.alphaMode = parsedAlphaMode.value_or(MaterialAlphaMode::Opaque);
materialData.alphaCutoff = materialData.alphaMode == MaterialAlphaMode::Mask
    ? ClampMaterialAlphaValue(static_cast<float>(material.alphaCutoff))
    : 0.5f;
materialData.opacity = 1.0f;
```

Do not overwrite `materialData.baseColor[3]`.

- [ ] **Step 8: Run focused tests and commit Task 1**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_material_alpha_tests
ctest --test-dir .\out\build\vs2026-x64 -C Debug -R miniengine.material_alpha --output-on-failure
git diff --check
git add -- engine/scene/material_graph.h engine/scene/material_alpha.cpp engine/scene/CMakeLists.txt engine/asset/model_loader.h engine/asset/model_loader.cpp engine/asset/gltf_model_loader.cpp tests/material_alpha_tests.cpp tests/CMakeLists.txt
git diff --cached --check
git commit -m "feat: preserve material alpha semantics"
```

Expected: focused CTest passes and the commit contains only shared alpha
semantics, glTF import, and its test target.

---

### Task 2: Material Sidecar Round Trip and Editor Controls

**Files:**
- Create: `engine/asset/material_definition.h`
- Create: `engine/asset/material_definition.cpp`
- Modify: `engine/asset/CMakeLists.txt`
- Modify: `engine/asset/model_loader.cpp:108-117`
- Modify: `engine/asset/material_graph_runtime.cpp:496-516,599-610`
- Modify: `engine/editor/services/model_import_service.cpp:20-140,341-400`
- Modify: `engine/editor/services/scene_renderables.cpp:36-52`
- Modify: `engine/editor/ui/editor_model_processor_panel.cpp:44-55,900-930`
- Modify: `engine/editor/ui/editor_material_graph.cpp:877-889`
- Modify: `engine/editor/ui/editor_model_preview.cpp:320-337,640-650,730-750`
- Modify: `tests/material_alpha_tests.cpp`

**Interfaces:**
- Consumes: Task 1 alpha types and `ModelMaterialData`/`ModelImportedMaterialInfo`.
- Produces:
  - `std::filesystem::path BuildMaterialDefinitionPath(const std::filesystem::path&, uint32_t);`
  - `ModelImportedMaterialInfo BuildImportedMaterialInfo(const ModelMaterialData&);`
  - `void ApplyImportedMaterialInfo(const ModelImportedMaterialInfo&, ModelMaterialData&);`
  - `YAML::Node SerializeMaterialDefinition(const ModelImportedMaterialInfo&);`
  - `bool LoadMaterialDefinition(const std::filesystem::path&, ModelImportedMaterialInfo&, std::string& warning);`
  - editor Alpha Mode/Cutoff controls and preview parity.

- [ ] **Step 1: Add failing sidecar round-trip tests**

Extend `tests/material_alpha_tests.cpp`:

```cpp
#include <material_definition.h>

ModelImportedMaterialInfo source{};
source.name = "masked foliage";
source.pbr.alphaMode = MaterialAlphaMode::Mask;
source.pbr.alphaCutoff = 0.37f;
source.pbr.baseColorFactor[3] = 0.6f;
source.pbr.opacity = 0.8f;

const YAML::Node serialized = SerializeMaterialDefinition(source);
Require(
    serialized["pbr"]["alpha_mode"].as<std::string>() == "mask",
    "sidecar alpha mode was not canonical"
);
Require(
    NearlyEqual(serialized["pbr"]["alpha_cutoff"].as<float>(), 0.37f),
    "sidecar cutoff was not serialized"
);

const std::filesystem::path sidecar =
    std::filesystem::temp_directory_path() / "miniengine_material_alpha.material.yaml";
{
    std::ofstream file(sidecar);
    file << "material:\n"
         << "  name: restored\n"
         << "  pbr:\n"
         << "    alpha_mode: blend\n"
         << "    alpha_cutoff: 2.0\n"
         << "    base_color_factor: [1, 1, 1, 0.4]\n"
         << "    opacity: -1.0\n";
}
ModelImportedMaterialInfo restored{};
std::string warning;
Require(LoadMaterialDefinition(sidecar, restored, warning), "sidecar did not load");
Require(restored.pbr.alphaMode == MaterialAlphaMode::Blend, "sidecar mode did not load");
Require(NearlyEqual(restored.pbr.alphaCutoff, 1.0f), "sidecar cutoff was not clamped");
Require(NearlyEqual(restored.pbr.opacity, 0.0f), "sidecar opacity was not clamped");

{
    std::ofstream file(sidecar);
    file << "material:\n  pbr:\n    alpha_mode: invalid\n";
}
restored = {};
warning.clear();
Require(LoadMaterialDefinition(sidecar, restored, warning), "invalid-mode sidecar did not load");
Require(restored.pbr.alphaMode == MaterialAlphaMode::Opaque, "invalid mode did not fall back");
Require(!warning.empty(), "invalid mode did not report a warning");
std::filesystem::remove(sidecar, removeError);
```

- [ ] **Step 2: Run the focused test and verify the missing I/O API fails**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_material_alpha_tests
```

Expected: compilation fails because `material_definition.h` and its functions
do not exist.

- [ ] **Step 3: Implement shared conversion and material-definition I/O**

Create `engine/asset/material_definition.h`:

```cpp
#pragma once

#include "model_loader.h"

#include <scene_components.h>
#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <filesystem>
#include <string>

std::filesystem::path BuildMaterialDefinitionPath(
    const std::filesystem::path& modelPath,
    uint32_t materialIndex
);
ModelImportedMaterialInfo BuildImportedMaterialInfo(const ModelMaterialData& material);
void ApplyImportedMaterialInfo(const ModelImportedMaterialInfo& source, ModelMaterialData& destination);
YAML::Node SerializeMaterialDefinition(const ModelImportedMaterialInfo& material);
bool LoadMaterialDefinition(
    const std::filesystem::path& path,
    ModelImportedMaterialInfo& material,
    std::string& warning
);
```

Implement `material_definition.cpp` by moving the duplicate flat/PBR copies
from `model_import_service.cpp`, emitting every existing texture/PBR/graph
field, and parsing them with their current value as fallback. The alpha block
must be exactly:

```cpp
const std::string storedMode = pbrNode["alpha_mode"].as<std::string>("opaque");
if (const std::optional<MaterialAlphaMode> parsed = ParseMaterialAlphaMode(storedMode))
{
    material.pbr.alphaMode = *parsed;
}
else
{
    material.pbr.alphaMode = MaterialAlphaMode::Opaque;
    warning = "Unknown material alpha_mode '" + storedMode + "'; using opaque";
}
material.pbr.alphaCutoff = ClampMaterialAlphaValue(
    pbrNode["alpha_cutoff"].as<float>(0.5f)
);
material.pbr.opacity = ClampMaterialAlphaValue(
    pbrNode["opacity"].as<float>(material.pbr.opacity)
);
```

The serializer's PBR block must contain:

```cpp
pbr["alpha_mode"] = ToString(material.pbr.alphaMode);
pbr["alpha_cutoff"] = ClampMaterialAlphaValue(material.pbr.alphaCutoff);
pbr["opacity"] = ClampMaterialAlphaValue(material.pbr.opacity);
```

Use `DeserializeMaterialShaderGraph` for the existing graph block and preserve
all existing texture-graph fields. Add `material_definition.cpp` and
`material_definition.h` to `engine_asset`.

- [ ] **Step 4: Run sidecar tests green**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_material_alpha_tests
ctest --test-dir .\out\build\vs2026-x64 -C Debug -R miniengine.material_alpha --output-on-failure
```

Expected: focused test passes.

- [ ] **Step 5: Add a failing restart-persistence test**

Extend the test to place a sidecar next to a temporary glTF and reload it:

```cpp
const std::filesystem::path reloadModel =
    std::filesystem::temp_directory_path() / "miniengine_material_reload.gltf";
{
    std::ofstream file(reloadModel);
    file << R"({
      "asset": { "version": "2.0" },
      "materials": [
        { "name": "editable", "pbrMetallicRoughness": { "baseColorFactor": [1,1,1,0.5] } }
      ]
    })";
}
const std::filesystem::path reloadSidecar = BuildMaterialDefinitionPath(reloadModel, 0);
{
    std::ofstream file(reloadSidecar);
    file << "material:\n  pbr:\n    alpha_mode: mask\n    alpha_cutoff: 0.42\n";
}
const LoadedModelData reloaded = ModelLoader::LoadModel(reloadModel.string());
Require(reloaded.materials[0].pbr.alphaMode == MaterialAlphaMode::Mask, "restart lost alpha mode");
Require(NearlyEqual(reloaded.materials[0].pbr.alphaCutoff, 0.42f), "restart lost cutoff");
std::filesystem::remove(reloadSidecar, removeError);
std::filesystem::remove(reloadModel, removeError);
```

- [ ] **Step 6: Run the test and verify restart persistence fails**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_material_alpha_tests
ctest --test-dir .\out\build\vs2026-x64 -C Debug -R miniengine.material_alpha --output-on-failure
```

Expected: the test fails with `restart lost alpha mode`.

- [ ] **Step 7: Apply sidecars during model load and replace editor duplication**

After glTF material synchronization in `ModelLoader::LoadModel`, apply each
existing sidecar:

```cpp
for (size_t materialIndex = 0; materialIndex < modelData.materials.size(); ++materialIndex)
{
    ModelMaterialData& rawMaterial = modelData.materials[materialIndex];
    ModelImportedMaterialInfo editable = BuildImportedMaterialInfo(rawMaterial);
    const std::filesystem::path sidecar =
        BuildMaterialDefinitionPath(modelPath, static_cast<uint32_t>(materialIndex));
    if (std::filesystem::exists(sidecar))
    {
        std::string warning;
        if (LoadMaterialDefinition(sidecar, editable, warning))
        {
            ApplyImportedMaterialInfo(editable, rawMaterial);
        }
        if (!warning.empty())
        {
            LOG_WARN("{}: {}", sidecar.string(), warning);
        }
    }
}
```

In `model_import_service.cpp`, replace private conversion/serialization
helpers with the shared functions and write:

```cpp
YAML::Node root(YAML::NodeType::Map);
root["material"] = SerializeMaterialDefinition(material);
```

Use `BuildMaterialDefinitionPath` for the output filename. Replace the local
conversion helpers in `scene_renderables.cpp` and
`editor_model_processor_panel.cpp` with `BuildImportedMaterialInfo`.

- [ ] **Step 8: Add alpha fields to shader-graph PBR serialization**

Where a `MaterialShaderNode::pbr` block is written, add:

```cpp
pbr["alpha_mode"] = ToString(node.pbr.alphaMode);
pbr["alpha_cutoff"] = ClampMaterialAlphaValue(node.pbr.alphaCutoff);
```

Where it is read, add:

```cpp
node.pbr.alphaMode = ParseMaterialAlphaMode(
    pbrNode["alpha_mode"].as<std::string>("opaque")
).value_or(MaterialAlphaMode::Opaque);
node.pbr.alphaCutoff = ClampMaterialAlphaValue(
    ReadFloatOrFallback(pbrNode["alpha_cutoff"], 0.5f)
);
node.pbr.opacity = ClampMaterialAlphaValue(
    ReadFloatOrFallback(pbrNode["opacity"], node.pbr.opacity)
);
```

- [ ] **Step 9: Add editor controls and CPU preview parity**

Add to `DrawMaterialPbrControls` after Base Factor:

```cpp
const char* alphaModes[] = { "Opaque", "Mask", "Blend" };
int alphaModeIndex = static_cast<int>(pbr.alphaMode);
if (ImGui::Combo("Alpha Mode", &alphaModeIndex, alphaModes, 3))
{
    pbr.alphaMode = static_cast<MaterialAlphaMode>(alphaModeIndex);
    changed = true;
}
if (pbr.alphaMode == MaterialAlphaMode::Mask)
{
    changed |= ImGui::SliderFloat("Alpha Cutoff", &pbr.alphaCutoff, 0.0f, 1.0f, "%.2f");
}
```

In the resolved-material summary, display mode and conditional cutoff. In
`HashPreviewPbrSurfaceSettings`, hash:

```cpp
HashCombine(seed, static_cast<uint32_t>(pbr.alphaMode));
HashQuantizedFloat(seed, pbr.alphaCutoff);
```

Replace the preview return alpha with:

```cpp
const float previewAlpha = ResolveMaterialCoverageAlpha(
    material.pbr.alphaMode,
    albedo.a,
    material.pbr.alphaCutoff
);
return glm::vec4(color, previewAlpha);
```

- [ ] **Step 10: Run focused and full existing tests, then commit Task 2**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_material_alpha_tests miniengine_scene_identity_tests miniengine_gizmo_settings_tests
ctest --test-dir .\out\build\vs2026-x64 -C Debug --output-on-failure
git diff --check
git add -- engine/asset/material_definition.h engine/asset/material_definition.cpp engine/asset/CMakeLists.txt engine/asset/model_loader.cpp engine/asset/material_graph_runtime.cpp engine/editor/services/model_import_service.cpp engine/editor/services/scene_renderables.cpp engine/editor/ui/editor_model_processor_panel.cpp engine/editor/ui/editor_material_graph.cpp engine/editor/ui/editor_model_preview.cpp tests/material_alpha_tests.cpp
git diff --cached --check
git commit -m "feat: persist editable material alpha modes"
```

Expected: all registered CTest targets pass and the commit contains only
sidecar/editor/preview alpha work.

---

### Task 3: Backend-Neutral Pipeline Policy and Draw Ordering

**Files:**
- Create: `engine/renderer/material_pipeline.h`
- Create: `engine/renderer/material_pipeline.cpp`
- Modify: `engine/renderer/CMakeLists.txt:6-25`
- Modify: `engine/renderer/renderer_world.h:29-39`
- Modify: `engine/editor/services/scene_renderables.cpp:150-215`
- Modify: `tests/material_alpha_tests.cpp`

**Interfaces:**
- Consumes: `MaterialAlphaMode`, `MeshData`, and per-submesh model/view data.
- Produces:
  - `MaterialPipelineKey`
  - `MaterialPipelineState`
  - `GetMaterialPipelineState(MaterialPipelineKey)`
  - `GetMaterialPipelineIndex(MaterialPipelineKey)`
  - `MaterialDrawSortKey`
  - `BuildMaterialDrawOrder(std::span<const MaterialDrawSortKey>)`
  - `ComputeMeshBoundsCenter(const MeshData&)`

- [ ] **Step 1: Add failing pipeline-state and ordering tests**

Append:

```cpp
#include <material_pipeline.h>

#include <array>
#include <vector>

const MaterialPipelineState opaqueState = GetMaterialPipelineState({
    MaterialAlphaMode::Opaque, false
});
Require(!opaqueState.blendEnabled, "Opaque blending was enabled");
Require(opaqueState.depthWriteEnabled, "Opaque depth writes were disabled");
Require(!opaqueState.alphaMaskEnabled, "Opaque mask discard was enabled");
Require(opaqueState.cullBackFaces, "single-sided Opaque stopped culling");

const MaterialPipelineState maskState = GetMaterialPipelineState({
    MaterialAlphaMode::Mask, true
});
Require(!maskState.blendEnabled, "Mask blending was enabled");
Require(maskState.depthWriteEnabled, "Mask depth writes were disabled");
Require(maskState.alphaMaskEnabled, "Mask discard was disabled");
Require(!maskState.cullBackFaces, "double-sided Mask still culled");

const MaterialPipelineState blendState = GetMaterialPipelineState({
    MaterialAlphaMode::Blend, false
});
Require(blendState.blendEnabled, "Blend blending was disabled");
Require(!blendState.depthWriteEnabled, "Blend depth writes were enabled");
Require(!blendState.alphaMaskEnabled, "Blend mask discard was enabled");

const std::array<MaterialDrawSortKey, 6> sortKeys{ {
    { { MaterialAlphaMode::Blend, false }, 2.0f },
    { { MaterialAlphaMode::Mask, false }, 8.0f },
    { { MaterialAlphaMode::Blend, true }, 9.0f },
    { { MaterialAlphaMode::Opaque, true }, 1.0f },
    { { MaterialAlphaMode::Blend, false }, 9.0f },
    { { MaterialAlphaMode::Blend, false }, 4.0f }
} };
const std::vector<size_t> order = BuildMaterialDrawOrder(sortKeys);
Require(order == std::vector<size_t>({ 1, 3, 2, 4, 5, 0 }), "draw order mismatch");
```

This expected order preserves the original Opaque/Mask sequence, puts all
Blend items after it, sorts Blend far-to-near, and preserves source order for
equal depths across culling variants.

- [ ] **Step 2: Run the test and verify the policy API is missing**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_material_alpha_tests
```

Expected: compilation fails because `material_pipeline.h` does not exist.

- [ ] **Step 3: Implement pure pipeline state and ordering**

Create `engine/renderer/material_pipeline.h`:

```cpp
#pragma once

#include <material_graph.h>

#include <cstddef>
#include <span>
#include <vector>

struct MaterialPipelineKey
{
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    bool doubleSided = false;

    bool operator==(const MaterialPipelineKey&) const = default;
};

struct MaterialPipelineState
{
    bool blendEnabled = false;
    bool depthWriteEnabled = true;
    bool alphaMaskEnabled = false;
    bool cullBackFaces = true;
};

struct MaterialDrawSortKey
{
    MaterialPipelineKey pipeline;
    float viewDepth = 0.0f;
};

inline constexpr size_t kMaterialPipelineVariantCount = 6;

MaterialPipelineState GetMaterialPipelineState(MaterialPipelineKey key);
size_t GetMaterialPipelineIndex(MaterialPipelineKey key);
std::vector<size_t> BuildMaterialDrawOrder(std::span<const MaterialDrawSortKey> keys);
```

Create `engine/renderer/material_pipeline.cpp`:

```cpp
#include "material_pipeline.h"

#include <algorithm>
#include <numeric>

MaterialPipelineState GetMaterialPipelineState(MaterialPipelineKey key)
{
    MaterialPipelineState state{};
    state.cullBackFaces = !key.doubleSided;
    state.alphaMaskEnabled = key.alphaMode == MaterialAlphaMode::Mask;
    state.blendEnabled = key.alphaMode == MaterialAlphaMode::Blend;
    state.depthWriteEnabled = key.alphaMode != MaterialAlphaMode::Blend;
    return state;
}

size_t GetMaterialPipelineIndex(MaterialPipelineKey key)
{
    return static_cast<size_t>(key.alphaMode) * 2u + (key.doubleSided ? 1u : 0u);
}

std::vector<size_t> BuildMaterialDrawOrder(std::span<const MaterialDrawSortKey> keys)
{
    std::vector<size_t> order(keys.size());
    std::iota(order.begin(), order.end(), size_t{ 0 });
    const auto blendBegin = std::stable_partition(order.begin(), order.end(), [&](size_t index)
    {
        return keys[index].pipeline.alphaMode != MaterialAlphaMode::Blend;
    });
    std::stable_sort(blendBegin, order.end(), [&](size_t lhs, size_t rhs)
    {
        return keys[lhs].viewDepth > keys[rhs].viewDepth;
    });
    return order;
}
```

Add both files to `engine_render_core`.

- [ ] **Step 4: Run policy tests green**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_material_alpha_tests
ctest --test-dir .\out\build\vs2026-x64 -C Debug -R miniengine.material_alpha --output-on-failure
```

Expected: focused test passes.

- [ ] **Step 5: Add failing mesh-center tests**

Append:

```cpp
#include <renderer_world.h>

MeshData boundsMesh{};
boundsMesh.vertices = {
    { { -2.0f, -4.0f, -6.0f } },
    { { 6.0f, 8.0f, 10.0f } }
};
Require(
    glm::length(ComputeMeshBoundsCenter(boundsMesh) - glm::vec3(2.0f, 2.0f, 2.0f)) < 0.0001f,
    "mesh bounds center mismatch"
);
Require(
    ComputeMeshBoundsCenter(MeshData{}) == glm::vec3(0.0f),
    "empty mesh did not use local origin"
);
```

- [ ] **Step 6: Implement bounds-center computation and propagate render data**

Declare in `renderer_world.h`:

```cpp
glm::vec3 ComputeMeshBoundsCenter(const MeshData& mesh);
```

Add to `CpuRenderSubmesh`:

```cpp
MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
glm::vec3 localBoundsCenter{ 0.0f };
```

Implement in `renderer_world.cpp`:

```cpp
glm::vec3 ComputeMeshBoundsCenter(const MeshData& mesh)
{
    if (mesh.vertices.empty())
    {
        return glm::vec3(0.0f);
    }
    glm::vec3 minimum(mesh.vertices.front().position[0], mesh.vertices.front().position[1], mesh.vertices.front().position[2]);
    glm::vec3 maximum = minimum;
    for (const Vertex& vertex : mesh.vertices)
    {
        const glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
        minimum = glm::min(minimum, position);
        maximum = glm::max(maximum, position);
    }
    return (minimum + maximum) * 0.5f;
}
```

When building every `CpuRenderSubmesh`, including the default cube path, set:

```cpp
renderSubmesh.alphaMode = material.alphaMode;
renderSubmesh.localBoundsCenter = ComputeMeshBoundsCenter(renderSubmesh.mesh);
renderSubmesh.material.emissiveFactor[3] =
    ClampMaterialAlphaValue(material.alphaCutoff);
```

- [ ] **Step 7: Run tests and commit Task 3**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target miniengine_material_alpha_tests
ctest --test-dir .\out\build\vs2026-x64 -C Debug -R miniengine.material_alpha --output-on-failure
git diff --check
git add -- engine/renderer/material_pipeline.h engine/renderer/material_pipeline.cpp engine/renderer/renderer_world.h engine/renderer/renderer_world.cpp engine/renderer/CMakeLists.txt engine/editor/services/scene_renderables.cpp tests/material_alpha_tests.cpp
git diff --cached --check
git commit -m "feat: define material pipeline ordering policy"
```

Expected: focused CTest passes and the commit contains only backend-neutral
render policy, bounds propagation, and tests.

---

### Task 4: Vulkan Six-Variant Pipeline Integration

**Files:**
- Create: `engine/renderer/vulkan/pipeline_set.h`
- Create: `engine/renderer/vulkan/pipeline_set.cpp`
- Modify: `engine/renderer/CMakeLists.txt:36-80`
- Modify: `engine/renderer/vulkan/pipeline.h:5-27`
- Modify: `engine/renderer/vulkan/pipeline.cpp:8-146`
- Modify: `engine/renderer/vulkan/command.h:7-16`
- Modify: `engine/renderer/vulkan/renderer.h:25-104`
- Modify: `engine/renderer/vulkan/renderer.cpp:309-337,709-825`
- Modify: `shaders/vulkan/triangle.frag:1-25,230-235`

**Interfaces:**
- Consumes: Task 3 `MaterialPipelineKey`, state mapping, draw ordering, and per-submesh center.
- Produces:
  - `VulkanPipeline(VkDevice, VkExtent2D, VkRenderPass, VkDescriptorSetLayout, MaterialPipelineKey)`
  - `VulkanPipelineSet::Get(MaterialPipelineKey) const`
  - atomically replaceable six-pipeline ownership.

- [ ] **Step 1: Replace the pipeline constructor with the tested key**

Change `pipeline.h`:

```cpp
VulkanPipeline(
    VkDevice device,
    VkExtent2D extent,
    VkRenderPass renderPass,
    VkDescriptorSetLayout descriptorSetLayout,
    MaterialPipelineKey key
);
```

Include `<material_pipeline.h>`. At the start of the implementation:

```cpp
const MaterialPipelineState state = GetMaterialPipelineState(key);
```

Map it to Vulkan:

```cpp
rasterizer.cullMode = state.cullBackFaces ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
depthStencil.depthWriteEnable = state.depthWriteEnabled ? VK_TRUE : VK_FALSE;
colorBlendAttachment.blendEnable = state.blendEnabled ? VK_TRUE : VK_FALSE;
```

Keep depth test enabled, `VK_COMPARE_OP_LESS`, and the existing source-alpha
blend factors.

- [ ] **Step 2: Add the Mask fragment specialization**

In `engine/renderer/material.h`, document the existing packed field without
changing layout:

```cpp
float emissiveFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // rgb emissive, a alpha cutoff
```

At shader scope in `triangle.frag`, add:

```glsl
layout(constant_id = 0) const bool kAlphaMask = false;
```

Replace:

```glsl
if (alphaCutoff > 0.0 && albedo.a < alphaCutoff)
```

with:

```glsl
if (kAlphaMask && albedo.a < alphaCutoff)
```

In `pipeline.cpp`, attach specialization data only through the fragment stage:

```cpp
const VkBool32 alphaMaskEnabled = state.alphaMaskEnabled ? VK_TRUE : VK_FALSE;
const VkSpecializationMapEntry alphaMaskEntry{ 0, 0, sizeof(alphaMaskEnabled) };
const VkSpecializationInfo alphaMaskSpecialization{
    1,
    &alphaMaskEntry,
    sizeof(alphaMaskEnabled),
    &alphaMaskEnabled
};
fragmentShaderStageInfo.pSpecializationInfo = &alphaMaskSpecialization;
```

- [ ] **Step 3: Compile the shader specialization before changing renderer call sites**

Run:

```powershell
cmake --build --preset vs2026-x64-debug --target engine_renderer_shaders
```

Expected: shader build succeeds and `glslc` accepts the specialization
constant. The C++ renderer is intentionally built only after Steps 4-7 update
all constructor call sites atomically.

- [ ] **Step 4: Implement all-or-nothing `VulkanPipelineSet`**

Create `pipeline_set.h`:

```cpp
#pragma once

#include "pipeline.h"

#include <array>
#include <memory>

class VulkanPipelineSet
{
public:
    VulkanPipelineSet(
        VkDevice device,
        VkExtent2D extent,
        VkRenderPass renderPass,
        VkDescriptorSetLayout descriptorSetLayout
    );

    const VulkanPipeline& Get(MaterialPipelineKey key) const;

private:
    std::array<std::unique_ptr<VulkanPipeline>, kMaterialPipelineVariantCount> m_pipelines;
};
```

Create `pipeline_set.cpp`:

```cpp
#include "pipeline_set.h"

VulkanPipelineSet::VulkanPipelineSet(
    VkDevice device,
    VkExtent2D extent,
    VkRenderPass renderPass,
    VkDescriptorSetLayout descriptorSetLayout
)
{
    for (MaterialAlphaMode mode : {
        MaterialAlphaMode::Opaque,
        MaterialAlphaMode::Mask,
        MaterialAlphaMode::Blend
    })
    {
        for (bool doubleSided : { false, true })
        {
            const MaterialPipelineKey key{ mode, doubleSided };
            m_pipelines[GetMaterialPipelineIndex(key)] = std::make_unique<VulkanPipeline>(
                device,
                extent,
                renderPass,
                descriptorSetLayout,
                key
            );
        }
    }
}

const VulkanPipeline& VulkanPipelineSet::Get(MaterialPipelineKey key) const
{
    return *m_pipelines.at(GetMaterialPipelineIndex(key));
}
```

Because construction completes before a `unique_ptr<VulkanPipelineSet>` is
assigned, an exception destroys every already-created temporary variant and
leaves the live set untouched.

- [ ] **Step 5: Replace the renderer's two pipelines with the set**

Add to both `CpuRenderSubmesh` upload data and `RenderSubmesh`:

```cpp
MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
glm::vec3 localBoundsCenter{ 0.0f };
```

Replace the two pipeline pointers in `renderer.h` with:

```cpp
std::unique_ptr<VulkanPipelineSet> m_graphicsPipelines;
```

In `CreatePipelineResources`, construct:

```cpp
m_graphicsPipelines = std::make_unique<VulkanPipelineSet>(
    m_device->GetHandle(),
    m_sceneViewportLayer->GetExtent(),
    m_sceneViewportLayer->GetRenderPass(),
    m_uniformBuffer->GetDescriptorSetLayout()
);
```

In `ApplyRenderContent`, construct a local
`std::unique_ptr<VulkanPipelineSet> newGraphicsPipelines`, wait for existing
frames only after all new resources exist, then move the complete set into the
live member.

- [ ] **Step 6: Carry keys and stable transparent order into draw items**

Replace `VulkanDrawItem::doubleSided` with:

```cpp
MaterialPipelineKey pipelineKey;
```

While uploading each submesh, copy `alphaMode` and `localBoundsCenter`.

In `BuildDrawItems`, first build items and sort keys:

```cpp
std::vector<VulkanDrawItem> unsorted;
std::vector<MaterialDrawSortKey> sortKeys;
unsorted.reserve(m_renderSubmeshes.size());
sortKeys.reserve(m_renderSubmeshes.size());

for (const RenderSubmesh& renderSubmesh : m_renderSubmeshes)
{
    ObjectPushConstants drawConstants{};
    drawConstants.model = State().rendererWorld.GetModelMatrix(renderSubmesh.entity);
    drawConstants.material = renderSubmesh.material;

    const MaterialPipelineKey pipelineKey{
        renderSubmesh.alphaMode,
        renderSubmesh.doubleSided
    };
    const glm::vec4 viewCenter =
        State().viewportMatrices.view *
        drawConstants.model *
        glm::vec4(renderSubmesh.localBoundsCenter, 1.0f);
    sortKeys.push_back({ pipelineKey, -viewCenter.z });
    unsorted.push_back(VulkanDrawItem{
        renderSubmesh.buffer->GetVertexHandle(),
        renderSubmesh.buffer->GetIndexHandle(),
        renderSubmesh.buffer->GetIndexCount(),
        m_uniformBuffer->GetDescriptorSet(imageIndex, renderSubmesh.materialBindingIndex),
        drawConstants,
        pipelineKey
    });
}

std::vector<VulkanDrawItem> ordered;
ordered.reserve(unsorted.size());
for (size_t index : BuildMaterialDrawOrder(sortKeys))
{
    ordered.push_back(std::move(unsorted[index]));
}
return ordered;
```

- [ ] **Step 7: Select the required pipeline per draw**

In `RecordSceneLayer`, replace the double-sided selection with:

```cpp
const VulkanPipeline* boundPipeline = nullptr;
for (const VulkanDrawItem& drawItem : drawItems)
{
    const VulkanPipeline& requiredPipeline =
        m_graphicsPipelines->Get(drawItem.pipelineKey);
    if (&requiredPipeline != boundPipeline)
    {
        vkCmdBindPipeline(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            requiredPipeline.GetHandle()
        );
        boundPipeline = &requiredPipeline;
    }
```

Use `boundPipeline->GetLayout()` for descriptors and push constants as the
existing loop does.

- [ ] **Step 8: Run focused tests, full build, CTest, and smoke**

Run:

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug --parallel
ctest --test-dir .\out\build\vs2026-x64 -C Debug --output-on-failure
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --frames 60
```

Expected: configure/build exit 0, all four CTest targets pass, and the app
exits 0 after 60 frames.

- [ ] **Step 9: Review and commit Task 4**

Run:

```powershell
git diff --check
git status --short
git diff -- engine/renderer shaders/vulkan/triangle.frag
git add -- engine/renderer/material.h engine/renderer/CMakeLists.txt engine/renderer/vulkan/pipeline.h engine/renderer/vulkan/pipeline.cpp engine/renderer/vulkan/pipeline_set.h engine/renderer/vulkan/pipeline_set.cpp engine/renderer/vulkan/command.h engine/renderer/vulkan/renderer.h engine/renderer/vulkan/renderer.cpp shaders/vulkan/triangle.frag
git diff --cached --check
git commit -m "feat: render material alpha pipeline variants"
```

Expected: only Vulkan integration and shader files are committed.

---

### Task 5: Final Verification, Protected README Handoff, and GUI Acceptance

**Files:**
- Prepare but do not apply without separate authorization: `README.md:16-25,138-210`

**Interfaces:**
- Consumes: completed Tasks 1-4 and their fresh verification evidence.
- Produces: fresh automated evidence, an exact README patch for reconciliation,
  and a manual acceptance handoff.

- [ ] **Step 1: Re-run the complete automated gate from a clean task worktree**

Run:

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug --parallel
ctest --test-dir .\out\build\vs2026-x64 -C Debug --output-on-failure
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --frames 60
git diff --check
```

Expected: all commands exit 0 and CTest reports four passing tests:
`miniengine.scene_identity`, `miniengine.gizmo_settings`,
`miniengine.material_alpha`, and `miniengine.vcpkg_layout_contract`.

- [ ] **Step 2: Prepare the exact README hunk without modifying the protected file**

In the current-capabilities section, state that Opaque/Mask/Blend are carried
from glTF/material sidecars into six Vulkan variants and Blend is sorted per
submesh.

In known limitations, replace the old “no alphaMode classification and
sorting” item with the remaining boundary:

```markdown
- 透明材质采用 submesh 包围盒中心的稳定后到前排序；相交透明几何和单个 submesh 内的自交仍需要网格拆分或后续 OIT。
```

Prepare this dated development record containing only verified facts:

```markdown
### 2026-07-30 — 材质 Alpha 管线

- glTF 与 `.material.yaml` 的 Opaque/Mask/Blend、alpha cutoff 和独立 opacity 已贯通材质编辑、CPU 预览与 Vulkan；导入 alpha 不再重复写入 factor 与 opacity。
- Vulkan 使用 alpha mode × single/double-sided 的六条静态管线：Opaque/Mask 写深度且不混合，Mask 执行 cutoff discard，Blend 混合但不写深度。
- Blend 在 Opaque/Mask 后按 submesh 包围盒中心稳定后到前排序；逐三角形排序、OIT 与相交透明几何不在本阶段范围。
- x64 Debug 构建、完整 CTest 和 Vulkan 60 帧冒烟测试通过；这些自动化结果不代替 GUI 与视觉验收。
```

Do not write exact pass counts until Step 1 confirms them. Save the proposed
hunk in the task handoff; do not apply it in the isolated worktree because
that would omit and later conflict with the root's protected 2026-07-30 audit.

- [ ] **Step 3: Verify code scope and stop for README reconciliation**

Run:

```powershell
git diff --check
git status --short
git diff --cached --name-only
```

Expected: the task worktree contains no uncommitted code changes and no
protected root files. Present the exact README hunk and ask whether the user
wants it applied to the root's existing uncommitted README, committed
separately after the root changes are resolved, or deferred.

- [ ] **Step 4: Hand off manual GUI acceptance**

Ask the user to verify:

1. An alpha-bearing Opaque texture remains completely opaque.
2. Mask cutoff changes visible cutout edges immediately and cutouts occlude
   correctly.
3. Two Blend submeshes composite correctly as camera depth order changes.
4. Alpha Mode/Cutoff persist after save and restart.
5. Single/double-sided Opaque, Mask, and Blend all render with correct culling.
6. Sponza Opaque dirt/decal materials no longer enter the blend path solely
   because their textures contain alpha.

State explicitly that implementation is automated-test complete but not
visually accepted until the user confirms these checks.
