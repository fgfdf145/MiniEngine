# Asset Lifecycle Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the asset browser doing filesystem work every frame on the importer's mutex, stop the registry losing uuids to transient IO errors, give `ModelCache` a const read handle and a bounded eviction policy, and give rename the reference check delete already has.

**Architecture:** Three internally-coupled groups. Registry safety changes `asset_registry.cpp` and the browser's preview panel. Cache lifecycle changes `ModelCache`'s interface first, then adds byte accounting and a reference-driven LRU wired into the two points where the scene's live set can change. Reference integrity extracts the existing delete-time scan into `asset_references.{h,cpp}` behind an mtime index, then adds rename as a second caller.

**Tech Stack:** C++20, yaml-cpp, Dear ImGui, EnTT, CMake + vcpkg, CTest.

**Spec:** [docs/superpowers/specs/2026-09-12-asset-lifecycle-hardening-design.md](../specs/2026-09-12-asset-lifecycle-hardening-design.md)

## Global Constraints

- Language standard is C++20. Formatting is Allman throughout; `scripts/check-format.ps1` is a gate.
- All engine code lives in `namespace me`.
- Target dependency direction may not be reversed. Everything here lands in `engine_asset` and `engine_editor`, plus two new test targets.
- `CMakePresets.json` is the only source of truth for build parameters. Configure with `cmake --preset vs2026-x64`, build with `cmake --build --preset vs2026-x64-debug --parallel`, run tests as `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`. `CMakePresets.json` defines no `testPresets`, so `ctest --preset ...` does not work in this repo.
- `ISceneWorld::Registry()` is read-only. Entity lifetime, component edits and dirty marks go through the scene interface.
- Access recency is a monotonic counter, never a clock: cheaper, and it makes eviction order deterministic in tests.
- Eviction is safe because `scene_renderables.cpp:88` reloads on a cache miss. Never introduce an eviction path that runs while `state.asyncLoad.IsLoading()` or `state.asyncSceneLoad.IsLoading()` — the fallback reload is guarded against exactly that and would return an empty renderable set instead.

## Honest Testing Note

Four of the eight tasks change ImGui panel code or function-local statics inside a UI translation unit. Those have no unit test here, and this plan does not pretend otherwise: each states what is verified by build and smoke run, and what a human still has to look at. Do not invent a test target for them — the pure logic they depend on (`ModelCache::Trim`, `FindReferencesTo`) is tested directly, which is where the risk actually lives.

## File Structure

**Create:**

| File | Responsibility |
| --- | --- |
| `engine/asset/asset_references.h` / `.cpp` | `AssetReference`, `FindReferencesTo`; an mtime-keyed index of which documents mention which file names. |
| `tests/model_cache_tests.cpp` | Byte accounting, live-key pinning, LRU order, key normalization, the two update entry points. |
| `tests/asset_references_tests.cpp` | Mention detection, sidecar/exclusion skipping, index reuse and invalidation. |

**Modify:**

| File | Change |
| --- | --- |
| `engine/asset/asset_registry.cpp` | Orphan judgment requires a clean `error_code`; `WriteSidecar` becomes atomic. |
| `engine/asset/asset_manager.h` / `.cpp` | Cache the preview uuid; invalidate `ModelCache` on rename; route rename through a reference check and confirmation modal; call `FindReferencesTo` instead of scanning inline. |
| `engine/asset/model_cache.h` / `.cpp` | `Get` returns const; `UpdateMaterial`/`UpdateMaterials`; byte accounting; `Trim`. |
| `engine/editor/services/model_import_service.cpp` | Two mutation sites move to the new update entry points. |
| `engine/editor/services/scene_renderables.cpp` | Const local at the refill site; `TrimModelCache` at the two live-set change points. |
| `engine/editor/ui/editor_model_preview.cpp` | Preview texture cache gains a counter-based LRU. |
| `tests/asset_registry_tests.cpp` | Two cases for the orphan and atomic-write changes. |
| `tests/CMakeLists.txt` | Two new test targets. |
| `README.md` | Dated entry in section 10. |

---

### Task 1: Registry orphan judgment and atomic sidecar writes

**Files:**
- Modify: `engine/asset/asset_registry.cpp`, `tests/asset_registry_tests.cpp`

**Interfaces:**
- Consumes: the `ScopedAssetRoot`, `Require`, `WriteFile`, `Exists` helpers already in `tests/asset_registry_tests.cpp`.
- Produces: no new public API. Behaviour change only.

- [ ] **Step 1: Write the failing tests**

In `tests/asset_registry_tests.cpp`, add inside the anonymous namespace:

```cpp
// A sidecar whose asset is present must survive a rescan. The orphan rule
// keys on "the asset file is absent", and std::filesystem::exists also
// returns false when it could not tell — that must not count as absence.
void OrphanJudgmentRequiresCleanEvidence()
{
    ScopedAssetRoot scope("orphan_evidence");
    const std::filesystem::path present = scope.Root() / "present.png";
    WriteFile(present, "x");
    AssetRegistry::GetOrCreateUuid(present);
    const std::filesystem::path presentSidecar = AssetRegistry::SidecarPathFor(present);
    Require(Exists(presentSidecar), "sidecar was not created");

    AssetRegistry::RescanAssetTree();
    Require(Exists(presentSidecar), "a sidecar whose asset is present was deleted as an orphan");

    // The genuine orphan case still deletes.
    const std::filesystem::path gone = scope.Root() / "gone.png";
    WriteFile(gone, "x");
    AssetRegistry::GetOrCreateUuid(gone);
    const std::filesystem::path goneSidecar = AssetRegistry::SidecarPathFor(gone);

    std::error_code ec;
    std::filesystem::remove(gone, ec);
    AssetRegistry::RescanAssetTree();
    Require(!Exists(goneSidecar), "a genuinely orphaned sidecar survived");
}

// The sidecar write must be atomic: either the old content or the new one,
// never a truncated file, and never a stray temporary left behind.
void SidecarWriteIsAtomic()
{
    ScopedAssetRoot scope("atomic_write");
    const std::filesystem::path asset = scope.Root() / "tex.png";
    WriteFile(asset, "x");

    const std::string uuid = AssetRegistry::GetOrCreateUuid(asset);
    Require(!uuid.empty(), "asset did not receive a uuid");

    const std::filesystem::path sidecar = AssetRegistry::SidecarPathFor(asset);
    Require(Exists(sidecar), "sidecar was not written");

    // No temporary file may be left in the directory.
    std::error_code ec;
    for (std::filesystem::directory_iterator it(scope.Root(), ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        Require(
            name.find(".tmp") == std::string::npos,
            "an atomic-write temporary file was left behind");
    }

    // And the sidecar must round-trip: a rescan re-reads it and keeps the uuid.
    AssetRegistry::RescanAssetTree();
    Require(AssetRegistry::GetOrCreateUuid(asset) == uuid, "sidecar did not round-trip through a rescan");
}
```

Call both from `main()`, after `OrphanSidecarPruning()`:

```cpp
        OrphanJudgmentRequiresCleanEvidence();
        SidecarWriteIsAtomic();
```

- [ ] **Step 2: Run to verify they pass against current code**

```bash
cmake --build --preset vs2026-x64-debug --parallel --target miniengine_asset_registry_tests
ctest --test-dir out/build/vs2026-x64 -C Debug -R miniengine.asset_registry --output-on-failure
```

Expected: **PASS**. These two tests characterize behaviour that is already correct on the happy path — the defects are on the error path, which a portable test cannot trigger (there is no cross-platform way to make `exists` fail or to crash mid-write). They are regression guards for the refactor in Step 3, not red-first tests. Do not skip them: without them, Step 3 could silently break the working case while fixing the error case.

- [ ] **Step 3: Make orphan judgment require clean evidence**

In `engine/asset/asset_registry.cpp`, inside `ScanLocked`, replace:

```cpp
            const std::string assetName = name.substr(0, name.size() - std::strlen(kSidecarSuffix));
            std::error_code existsEc;
            if (assetName.empty() || !std::filesystem::exists(path.parent_path() / assetName, existsEc))
            {
                orphanedSidecars.push_back(path);
            }
```

with:

```cpp
            const std::string assetName = name.substr(0, name.size() - std::strlen(kSidecarSuffix));
            std::error_code existsEc;
            const bool assetPresent =
                !assetName.empty() && std::filesystem::exists(path.parent_path() / assetName, existsEc);
            // `exists` returns false both for "absent" and for "could not
            // tell". Only the first is evidence of an orphan; deleting on the
            // second loses the uuid permanently to a transient IO error.
            if (assetName.empty() || (!assetPresent && !existsEc))
            {
                orphanedSidecars.push_back(path);
            }
```

- [ ] **Step 4: Make the sidecar write atomic**

In `engine/asset/asset_registry.cpp`, replace the body of `WriteSidecar` after the `root["asset"] = asset;` line:

```cpp
    // Write to a temporary and rename over the destination: a crash then
    // leaves either the old sidecar or the new one, never a truncated file
    // that reads back as "no uuid".
    const std::filesystem::path tempPath = sidecarPath.parent_path() /
                                           (sidecarPath.filename().string() + ".tmp");
    {
        std::ofstream out(tempPath, std::ios::trunc);
        if (!out)
        {
            LOG_WARN("Could not write asset sidecar '{}'", tempPath.string());
            return false;
        }
        out << root;
        out.flush();
        if (!out.good())
        {
            out.close();
            std::error_code removeEc;
            std::filesystem::remove(tempPath, removeEc);
            LOG_WARN("Could not write asset sidecar '{}'", tempPath.string());
            return false;
        }
    }

    std::error_code renameEc;
    std::filesystem::rename(tempPath, sidecarPath, renameEc);
    if (renameEc)
    {
        std::error_code removeEc;
        std::filesystem::remove(tempPath, removeEc);
        LOG_WARN(
            "Could not replace asset sidecar '{}': {}",
            sidecarPath.string(), renameEc.message());
        return false;
    }
    return true;
```

The `.tmp` name must end in `kSidecarSuffix + ".tmp"`, which does **not** match `name.ends_with(kSidecarSuffix)`, so a stray temporary is never mistaken for a sidecar by `ScanLocked`. It is also not a registrable extension, so it is ignored by registration.

- [ ] **Step 5: Run tests and the format gate**

```bash
cmake --build --preset vs2026-x64-debug --parallel
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
```

Expected: all tests pass, format clean.

- [ ] **Step 6: Commit**

```bash
git add engine/asset/asset_registry.cpp tests/asset_registry_tests.cpp
git commit -F - <<'EOF'
fix(asset): stop losing uuids to transient IO errors

The orphan rule deleted a sidecar whenever exists() on its asset returned
false, which it also does when it could not tell. One transient error was
enough to lose a uuid permanently.

WriteSidecar truncated and streamed, so a crash mid-write left an empty
sidecar that reads back as no uuid. It now writes a temporary and renames
over the destination. The temporary's name ends in .tmp, which does not match
the sidecar suffix, so a stray one is never mistaken for a sidecar.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 2: Cache the asset browser's preview uuid

No unit test: this is ImGui panel state. Verified by build, smoke run, and the reasoning that a uuid is immutable for a file's lifetime.

**Files:**
- Modify: `engine/asset/asset_manager.h`, `engine/asset/asset_manager.cpp`

**Interfaces:**
- Consumes: `AssetRegistry::GetOrCreateUuid`, `AssetRegistry::IsRegistrableAsset` (existing).
- Produces: no new public API.

- [ ] **Step 1: Add the cache fields**

In `engine/asset/asset_manager.h`, after the `m_anchorIdx` member:

```cpp
    // The focused entry's uuid, recomputed only when the focus moves or the
    // entry list is rebuilt. GetOrCreateUuid takes the global registry mutex
    // and hits the filesystem; a background import holds that same mutex
    // across a whole-tree rescan, so calling it per frame stalls the UI.
    int m_previewUuidIndex = -1;
    std::string m_previewUuid;
```

- [ ] **Step 2: Invalidate on rescan**

In `engine/asset/asset_manager.cpp`, in `ScanCurrentDir`, after `m_renamingIndex = -1;`:

```cpp
    m_previewUuidIndex = -1;
    m_previewUuid.clear();
```

- [ ] **Step 3: Use the cache in the preview panel**

In `engine/asset/asset_manager.cpp`, replace:

```cpp
    if (!entry.isDir && AssetRegistry::IsRegistrableAsset(entry.path))
    {
        const std::string uuid = AssetRegistry::GetOrCreateUuid(entry.path);
        if (!uuid.empty())
        {
            ImGui::TextDisabled("UUID: %s", uuid.c_str());
        }
    }
```

with:

```cpp
    if (!entry.isDir && AssetRegistry::IsRegistrableAsset(entry.path))
    {
        if (m_previewUuidIndex != focusIdx)
        {
            m_previewUuid = AssetRegistry::GetOrCreateUuid(entry.path);
            m_previewUuidIndex = focusIdx;
        }
        if (!m_previewUuid.empty())
        {
            ImGui::TextDisabled("UUID: %s", m_previewUuid.c_str());
        }
    }
```

- [ ] **Step 4: Build, test, format**

```bash
cmake --build --preset vs2026-x64-debug --parallel
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60
```

Expected: build clean, tests pass, format clean, smoke exits 0.

**Still needs a human:** open the Assets panel, click a `.png` or `.glb`, confirm the UUID line still shows and still changes when you click a different file. The smoke run does not open the panel.

- [ ] **Step 5: Commit**

```bash
git add engine/asset/asset_manager.h engine/asset/asset_manager.cpp
git commit -F - <<'EOF'
perf(asset): stop recomputing the preview uuid every frame

The asset browser called GetOrCreateUuid for the focused entry on every
frame: the global registry mutex, an is_regular_file syscall, and a possible
sidecar write. A background import holds that same mutex across a recursive
tree scan with sidecar writes, so the UI thread stalled behind it.

A uuid is immutable for a file's lifetime, so it is now computed when the
focus moves or the entry list is rebuilt.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 3: `ModelCache::Get` returns const; explicit mutation entry points

**Files:**
- Create: `tests/model_cache_tests.cpp`
- Modify: `engine/asset/model_cache.h`, `engine/asset/model_cache.cpp`, `engine/editor/services/model_import_service.cpp`, `engine/editor/services/scene_renderables.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ApplyImportedMaterialInfo(const ModelImportedMaterialInfo&, ModelMaterialData&)` from `engine/asset/material_definition.h`.
- Produces:
  - `std::shared_ptr<const LoadedModelData> me::ModelCache::Get(const std::string& path)`
  - `void me::ModelCache::UpdateMaterial(const std::string& path, uint32_t materialIndex, const ModelImportedMaterialInfo& material)`
  - `void me::ModelCache::UpdateMaterials(const std::string& path, const std::vector<ModelImportedMaterialInfo>& materials)`

  Task 4 adds `EstimateBytes` and `Trim` to the same namespace.

- [ ] **Step 1: Write the failing test**

Create `tests/model_cache_tests.cpp`:

```cpp
#include <engine/asset/model_cache.h>

#include <engine/scene/scene_components.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// A model with `vertexCount` vertices and `indexCount` indices in one submesh,
// carrying one material. Enough for byte accounting and identity checks; the
// geometry is never interpreted.
std::shared_ptr<LoadedModelData> MakeModel(size_t vertexCount, size_t indexCount, const char* materialName)
{
    auto data = std::make_shared<LoadedModelData>();
    ModelSubmeshData submesh;
    submesh.mesh.vertices.resize(vertexCount);
    submesh.mesh.indices.resize(indexCount);
    data->submeshes.push_back(std::move(submesh));

    ModelMaterialData material;
    material.name = materialName;
    data->materials.push_back(std::move(material));
    return data;
}

void UpdateEntryPoints()
{
    const std::string path = "C:/fake/update.glb";
    ModelCache::Invalidate(path);
    ModelCache::Store(path, MakeModel(3, 3, "original"));

    ModelImportedMaterialInfo info;
    info.name = "edited";
    ModelCache::UpdateMaterial(path, 0, info);

    std::shared_ptr<const LoadedModelData> observed = ModelCache::Get(path);
    Require(observed != nullptr, "cached model vanished after UpdateMaterial");
    Require(observed->materials.size() == 1, "UpdateMaterial changed the material count");
    Require(observed->materials[0].name == "edited", "UpdateMaterial did not reach the cached model");

    // Out-of-range index and absent path are both no-ops, not crashes.
    ModelCache::UpdateMaterial(path, 99, info);
    ModelCache::UpdateMaterial("C:/fake/not-cached.glb", 0, info);

    std::vector<ModelImportedMaterialInfo> batch;
    ModelImportedMaterialInfo second;
    second.name = "batch-edited";
    batch.push_back(second);
    ModelCache::UpdateMaterials(path, batch);
    observed = ModelCache::Get(path);
    Require(observed->materials[0].name == "batch-edited", "UpdateMaterials did not reach the cached model");

    ModelCache::UpdateMaterials("C:/fake/not-cached.glb", batch);

    ModelCache::Invalidate(path);
    Require(ModelCache::Get(path) == nullptr, "Invalidate did not remove the entry");
}
}

int main()
{
    try
    {
        UpdateEntryPoints();

        std::cout << "model cache tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "model cache tests failed: " << error.what() << '\n';
        return 1;
    }
}
```

Add to the end of `tests/CMakeLists.txt`:

```cmake
# model_cache.cpp calls ApplyImportedMaterialInfo, whose translation unit pulls in
# material_graph_runtime and yaml-cpp. Listing that chain by hand would be brittle, so this
# target links engine_asset, like miniengine_model_import_texture_tests.
add_executable(miniengine_model_cache_tests
    model_cache_tests.cpp
)
miniengine_group_target_sources(miniengine_model_cache_tests)

target_link_libraries(miniengine_model_cache_tests
    PRIVATE
        engine_asset
)

if(WIN32)
    add_custom_command(TARGET miniengine_model_cache_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_model_cache_tests>
            $<TARGET_FILE_DIR:miniengine_model_cache_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

add_test(
    NAME miniengine.model_cache
    COMMAND miniengine_model_cache_tests
)

set_target_properties(miniengine_model_cache_tests PROPERTIES FOLDER "Tests")
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug --parallel --target miniengine_model_cache_tests
```

Expected: compile error — `UpdateMaterial` is not a member of `ModelCache`.

- [ ] **Step 3: Change the interface**

In `engine/asset/model_cache.h`, replace the `ModelCache` namespace body:

```cpp
namespace ModelCache
{
bool IsCached(const std::string& path);

// Const on purpose: the cache hands out a shared handle, and writing through
// it from one caller while another reads is how the two mutation sites used to
// work. Mutation goes through the named entry points below.
std::shared_ptr<const LoadedModelData> Get(const std::string& path);

void Store(const std::string& path, std::shared_ptr<LoadedModelData> data);

// Apply an edited material back into the cached model. No-ops when the path is
// not cached or the index is out of range, matching the guards the call sites
// used to carry themselves.
void UpdateMaterial(const std::string& path, uint32_t materialIndex, const ModelImportedMaterialInfo& material);
void UpdateMaterials(const std::string& path, const std::vector<ModelImportedMaterialInfo>& materials);

// Removes the cached entry for `path`. If `path` is a directory, every cached
// model under it is removed as well. Call before deleting assets on disk so
// stale data is not served for a re-imported file at the same path.
void Invalidate(const std::string& path);
}
```

Add the includes it now needs, after `#include "model_loader.h"`:

```cpp
#include <engine/scene/scene_components.h>

#include <vector>
```

- [ ] **Step 4: Implement the new entry points**

In `engine/asset/model_cache.cpp`, add after the existing includes:

```cpp
#include "material_definition.h"
```

Change `Get`'s signature and add the two update functions after `Store`:

```cpp
std::shared_ptr<const LoadedModelData> Get(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(key);
    return it != s_modelCache.end() ? it->second : nullptr;
}

void UpdateMaterial(const std::string& path, uint32_t materialIndex, const ModelImportedMaterialInfo& material)
{
    const std::string key = NormalizeKey(path);
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(key);
    if (it == s_modelCache.end() || !it->second || materialIndex >= it->second->materials.size())
    {
        return;
    }
    ApplyImportedMaterialInfo(material, it->second->materials[materialIndex]);
}

void UpdateMaterials(const std::string& path, const std::vector<ModelImportedMaterialInfo>& materials)
{
    const std::string key = NormalizeKey(path);
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(key);
    if (it == s_modelCache.end() || !it->second)
    {
        return;
    }
    const size_t count = std::min(materials.size(), it->second->materials.size());
    for (size_t i = 0; i < count; ++i)
    {
        ApplyImportedMaterialInfo(materials[i], it->second->materials[i]);
    }
}
```

Add `#include <algorithm>` for `std::min` if it is not already there.

- [ ] **Step 5: Move the two mutation sites**

In `engine/editor/services/model_import_service.cpp`, in `UpdateImportedMaterialDefinition`, replace:

```cpp
    // Update the single material at the given index in the model cache.
    std::shared_ptr<LoadedModelData> cached = ModelCache::Get(modelPath);
    if (cached && materialIndex < cached->materials.size())
    {
        ApplyImportedMaterialInfo(material, cached->materials[materialIndex]);
    }
```

with:

```cpp
    // Update the single material at the given index in the model cache.
    ModelCache::UpdateMaterial(modelPath, materialIndex, material);
```

In `UpdateImportedModelMaterialDefinitions`, replace:

```cpp
    // Propagate user edits into the cached raw model data so that
    // Dirty renderable refresh picks up the new blend graphs and PBR factors.
    std::shared_ptr<LoadedModelData> cached = ModelCache::Get(modelPathString);
    if (cached)
    {
        const size_t count = std::min(materials.size(), cached->materials.size());
        for (size_t i = 0; i < count; ++i)
        {
            ApplyImportedMaterialInfo(materials[i], cached->materials[i]);
        }
    }
```

with:

```cpp
    // Propagate user edits into the cached raw model data so that
    // Dirty renderable refresh picks up the new blend graphs and PBR factors.
    ModelCache::UpdateMaterials(modelPathString, materials);
```

- [ ] **Step 6: Fix the refill site's local type**

In `engine/editor/services/scene_renderables.cpp`, replace:

```cpp
    std::shared_ptr<LoadedModelData> modelDataPtr = ModelCache::Get(model.sourcePath);
    if (!modelDataPtr)
    {
        // Don't do a synchronous load while an async loader is running on another thread:
        // the model loader is not thread-safe and concurrent access to the same file crashes.
        if (state.asyncLoad.IsLoading() || state.asyncSceneLoad.IsLoading())
        {
            return renderSubmeshes;
        }
        modelDataPtr = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(model.sourcePath));
        ModelCache::Store(model.sourcePath, modelDataPtr);
    }
```

with:

```cpp
    std::shared_ptr<const LoadedModelData> modelDataPtr = ModelCache::Get(model.sourcePath);
    if (!modelDataPtr)
    {
        // Don't do a synchronous load while an async loader is running on another thread:
        // the model loader is not thread-safe and concurrent access to the same file crashes.
        if (state.asyncLoad.IsLoading() || state.asyncSceneLoad.IsLoading())
        {
            return renderSubmeshes;
        }
        auto loaded = std::make_shared<LoadedModelData>(ModelLoader::LoadModel(model.sourcePath));
        ModelCache::Store(model.sourcePath, loaded);
        modelDataPtr = loaded;
    }
```

- [ ] **Step 7: Run tests and the format gate**

```bash
cmake --build --preset vs2026-x64-debug --parallel
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
```

Expected: build clean (any remaining `shared_ptr<LoadedModelData> = Get(...)` is now a compile error, which is the point), tests pass, format clean.

- [ ] **Step 8: Commit**

```bash
git add engine/asset/model_cache.h engine/asset/model_cache.cpp engine/editor/services/model_import_service.cpp engine/editor/services/scene_renderables.cpp tests/model_cache_tests.cpp tests/CMakeLists.txt
git commit -F - <<'EOF'
refactor(asset): make ModelCache::Get a const handle

The cache handed out a mutable shared_ptr and two call sites wrote the cached
model through it. Both happen to run on the main thread today, so there is no
live data race, but nothing in the interface said so and a background thread
can Store over the same key at any point.

Get now returns shared_ptr<const LoadedModelData>; UpdateMaterial and
UpdateMaterials are the named mutation entry points, applying under the cache
mutex and no-oping for an absent path or out-of-range index, which is what the
call sites guarded for themselves.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 4: Byte accounting and reference-driven LRU eviction

**Files:**
- Modify: `engine/asset/model_cache.h`, `engine/asset/model_cache.cpp`, `engine/editor/services/scene_renderables.cpp`, `tests/model_cache_tests.cpp`

**Interfaces:**
- Consumes: `Get`, `Store`, `Invalidate`, `MakeModel` test helper from Task 3.
- Produces:
  - `size_t me::ModelCache::EstimateBytes(const LoadedModelData& data)`
  - `void me::ModelCache::Trim(const std::unordered_set<std::string>& liveKeys, size_t budgetBytes)`
  - `size_t me::ModelCache::TotalBytes()` — test observability
  - `constexpr size_t me::kDefaultModelCacheBudgetBytes`

- [ ] **Step 1: Write the failing tests**

In `tests/model_cache_tests.cpp`, add inside the anonymous namespace:

```cpp
void ByteAccounting()
{
    const LoadedModelData data = *MakeModel(10, 20, "m");
    const size_t expected = 10 * sizeof(Vertex) + 20 * sizeof(uint32_t);
    Require(ModelCache::EstimateBytes(data) == expected, "EstimateBytes did not match vertices+indices");

    // TotalBytes reports what the cache actually holds, so a stored model of
    // known size is the one case where its value is fully determined.
    const std::string path = "C:/fake/bytes.glb";
    ModelCache::Invalidate(path);
    const size_t before = ModelCache::TotalBytes();
    ModelCache::Store(path, MakeModel(10, 20, "m"));
    Require(ModelCache::TotalBytes() == before + expected, "TotalBytes did not account for a stored model");
    ModelCache::Invalidate(path);
    Require(ModelCache::TotalBytes() == before, "TotalBytes did not drop after Invalidate");
}

void LiveKeysArePinned()
{
    const std::string live = "C:/fake/live.glb";
    ModelCache::Invalidate(live);
    ModelCache::Store(live, MakeModel(100, 100, "m"));

    // Budget of zero: everything unreferenced goes, everything live stays.
    ModelCache::Trim({live}, 0);
    Require(ModelCache::Get(live) != nullptr, "a live key was evicted despite a zero budget");

    ModelCache::Trim({}, 0);
    Require(ModelCache::Get(live) == nullptr, "an unreferenced entry survived a zero budget");
}

void LruOrder()
{
    const std::string a = "C:/fake/a.glb";
    const std::string b = "C:/fake/b.glb";
    const std::string c = "C:/fake/c.glb";
    for (const std::string& path : {a, b, c})
    {
        ModelCache::Invalidate(path);
    }

    // Equal size each, so the budget arithmetic is unambiguous.
    ModelCache::Store(a, MakeModel(10, 10, "m"));
    ModelCache::Store(b, MakeModel(10, 10, "m"));
    ModelCache::Store(c, MakeModel(10, 10, "m"));
    const size_t one = 10 * sizeof(Vertex) + 10 * sizeof(uint32_t);

    // Touch a and c, leaving b as least recently used.
    Require(ModelCache::Get(a) != nullptr, "a was not stored");
    Require(ModelCache::Get(c) != nullptr, "c was not stored");

    ModelCache::Trim({}, one * 2);
    Require(ModelCache::Get(b) == nullptr, "LRU did not evict the least recently used entry");
    Require(ModelCache::Get(a) != nullptr, "LRU evicted a recently used entry");
    Require(ModelCache::Get(c) != nullptr, "LRU evicted a recently used entry");

    ModelCache::Invalidate(a);
    ModelCache::Invalidate(c);
}

// Trim normalizes what it is given, so a live key spelled with different
// separators than the stored key still pins its entry.
void LiveKeyNormalization()
{
    const std::string stored = "C:/fake/norm/model.glb";
    ModelCache::Invalidate(stored);
    ModelCache::Store(stored, MakeModel(10, 10, "m"));

    ModelCache::Trim({"C:\\fake\\norm\\model.glb"}, 0);
    Require(ModelCache::Get(stored) != nullptr, "a differently-spelled live key failed to pin its entry");

    ModelCache::Invalidate(stored);
}
```

Call all four from `main()`, after `UpdateEntryPoints()`:

```cpp
        ByteAccounting();
        LiveKeysArePinned();
        LruOrder();
        LiveKeyNormalization();
```

Add `#include <cstdint>` and `#include <unordered_set>` to the test's includes.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build --preset vs2026-x64-debug --parallel --target miniengine_model_cache_tests
```

Expected: compile error — `EstimateBytes` and `Trim` are not members of `ModelCache`.

- [ ] **Step 3: Declare the new API**

In `engine/asset/model_cache.h`, add `#include <unordered_set>` and, inside `namespace me` above `namespace ModelCache`:

```cpp
// Parsed model data is the largest thing the editor holds on the CPU. One
// Sponza-scale bundle is hundreds of megabytes, and without a bound every
// model ever loaded stays resident until the process exits.
inline constexpr size_t kDefaultModelCacheBudgetBytes = 1ull << 30; // 1 GiB
```

Inside `namespace ModelCache`, after `Invalidate`:

```cpp
// Vertices plus indices. Ignores the material and name strings: they are
// kilobytes against a mesh's megabytes, and an estimate that is stable and
// cheap beats one that is exact.
size_t EstimateBytes(const LoadedModelData& data);

// Total bytes currently held. Exists for tests and diagnostics.
size_t TotalBytes();

// Evicts until the total is within budget. Entries whose key is in `liveKeys`
// are never evicted, whatever the budget: the scene still references them, and
// evicting one only forces a synchronous reload on the next renderable
// rebuild. Everything else goes least-recently-used first. `liveKeys` are
// normalized with the cache's own key rule, so callers may pass raw paths.
void Trim(const std::unordered_set<std::string>& liveKeys, size_t budgetBytes);
```

- [ ] **Step 4: Implement accounting and eviction**

In `engine/asset/model_cache.cpp`, replace the cache map declaration:

```cpp
std::mutex s_modelCacheMutex;

struct CacheEntry
{
    std::shared_ptr<LoadedModelData> data;
    size_t bytes = 0;
    uint64_t lastAccess = 0;
};

std::unordered_map<std::string, CacheEntry> s_modelCache;

// Monotonic, not a clock: cheaper, and it makes eviction order deterministic
// in tests.
uint64_t s_accessCounter = 0;
```

Every existing access to `it->second` as the shared_ptr becomes `it->second.data`. The functions become:

```cpp
bool IsCached(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(key);
    if (it == s_modelCache.end())
    {
        return false;
    }
    it->second.lastAccess = ++s_accessCounter;
    return true;
}

std::shared_ptr<const LoadedModelData> Get(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(key);
    if (it == s_modelCache.end())
    {
        return nullptr;
    }
    it->second.lastAccess = ++s_accessCounter;
    return it->second.data;
}

void Store(const std::string& path, std::shared_ptr<LoadedModelData> data)
{
    const std::string key = NormalizeKey(path);
    const size_t bytes = data ? EstimateBytes(*data) : 0;
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    s_modelCache[key] = CacheEntry{std::move(data), bytes, ++s_accessCounter};
}
```

`UpdateMaterial` and `UpdateMaterials` change their guards to `it->second.data` and apply into `it->second.data->materials[...]`. They do not bump `lastAccess`: editing a material is not evidence the model is being rendered.

`Invalidate` erases `it` the same way; only the mapped type changed.

Then add:

```cpp
size_t EstimateBytes(const LoadedModelData& data)
{
    size_t bytes = 0;
    for (const ModelSubmeshData& submesh : data.submeshes)
    {
        bytes += submesh.mesh.vertices.size() * sizeof(Vertex);
        bytes += submesh.mesh.indices.size() * sizeof(uint32_t);
    }
    return bytes;
}

size_t TotalBytes()
{
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    size_t total = 0;
    for (const auto& [key, entry] : s_modelCache)
    {
        total += entry.bytes;
    }
    return total;
}

void Trim(const std::unordered_set<std::string>& liveKeys, size_t budgetBytes)
{
    std::unordered_set<std::string> normalizedLive;
    normalizedLive.reserve(liveKeys.size());
    for (const std::string& liveKey : liveKeys)
    {
        normalizedLive.insert(NormalizeKey(liveKey));
    }

    std::lock_guard<std::mutex> lock(s_modelCacheMutex);

    size_t total = 0;
    std::vector<std::pair<uint64_t, std::string>> evictable;
    for (const auto& [key, entry] : s_modelCache)
    {
        total += entry.bytes;
        if (normalizedLive.count(key) == 0)
        {
            evictable.emplace_back(entry.lastAccess, key);
        }
    }

    if (total <= budgetBytes)
    {
        return;
    }

    // Oldest first.
    std::sort(evictable.begin(), evictable.end());
    for (const auto& [lastAccess, key] : evictable)
    {
        if (total <= budgetBytes)
        {
            break;
        }
        const auto it = s_modelCache.find(key);
        if (it == s_modelCache.end())
        {
            continue;
        }
        total -= it->second.bytes;
        s_modelCache.erase(it);
    }
}
```

Add `#include <cstdint>`, `#include <unordered_set>` and `#include <vector>` to the `.cpp`.

- [ ] **Step 5: Run the cache tests**

```bash
cmake --build --preset vs2026-x64-debug --parallel --target miniengine_model_cache_tests
ctest --test-dir out/build/vs2026-x64 -C Debug -R miniengine.model_cache --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Wire eviction to the live-set change points**

In `engine/editor/services/scene_renderables.cpp`, add to the anonymous namespace, after `BuildEntityRenderSubmeshes`:

```cpp
// The scene's live set only changes when renderables are rebuilt or refreshed,
// so eviction is evaluated there rather than per frame, where it would almost
// always be a no-op.
void TrimModelCache(RendererSharedState& state)
{
    const entt::registry& registry = state.GetEditorWorld().Registry();
    std::unordered_set<std::string> liveKeys;
    for (entt::entity entity : registry.view<const ModelComponent>())
    {
        const ModelComponent& model = registry.get<ModelComponent>(entity);
        if (!model.sourcePath.empty())
        {
            liveKeys.insert(model.sourcePath);
        }
    }
    ModelCache::Trim(liveKeys, kDefaultModelCacheBudgetBytes);
}
```

At the end of `RebuildSceneRenderables`, after `state.renderablesDirty = true;`:

```cpp
    TrimModelCache(state);
```

At the end of `RefreshDirtySceneRenderables`, replace `return changed;` with:

```cpp
    if (changed)
    {
        TrimModelCache(state);
    }
    return changed;
```

Confirm `engine/asset/model_cache.h` and `<unordered_set>` are included by this file; add whichever is missing.

- [ ] **Step 7: Run the whole suite, format and smoke**

```bash
cmake --build --preset vs2026-x64-debug --parallel
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60
```

Expected: all pass, smoke exits 0. The default scene is well under 1 GiB, so nothing should be evicted during the smoke run — the log must show no repeated reloads of the same model.

- [ ] **Step 8: Commit**

```bash
git add engine/asset/model_cache.h engine/asset/model_cache.cpp engine/editor/services/scene_renderables.cpp tests/model_cache_tests.cpp
git commit -F - <<'EOF'
feat(asset): bound the model cache with reference-driven LRU eviction

Parsed model data had no capacity bound and no eviction: every model ever
loaded stayed resident until the process exited, and one Sponza-scale bundle
is hundreds of megabytes.

Entries the scene still references are never evicted whatever the budget --
evicting one only forces a synchronous reload on the next rebuild, which is a
usability regression, not a saving. Everything else is evicted
least-recently-used down to a 1 GiB budget.

Eviction runs where the live set can actually change, at the end of
RebuildSceneRenderables and of a RefreshDirtySceneRenderables that reported a
change, rather than per frame where it would almost always be a no-op.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 5: Bound the preview texture cache

No unit test: the cache is a function-local static inside a UI translation unit with no seam to drive it from. The LRU logic here is a direct transcription of `ModelCache::Trim`, which is tested.

**Files:**
- Modify: `engine/editor/ui/editor_model_preview.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: no public API.

- [ ] **Step 1: Give cache entries a recency counter**

In `engine/editor/ui/editor_model_preview.cpp`, add to `CachedPreviewTexture`:

```cpp
struct CachedPreviewTexture
{
    TextureData texture;
    std::filesystem::file_time_type lastWriteTime{};
    bool resolved = false;
    bool available = false;
    uint64_t lastAccess = 0;
};
```

- [ ] **Step 2: Add the budget and the trim**

Directly after `GetPreviewTextureCache()`:

```cpp
// Decoded full-resolution RGBA8. A 4K base color map is 32 MB decoded, and the
// panel touches a new one every time the user clicks a different material.
constexpr size_t kPreviewTextureBudgetBytes = 256ull * 1024 * 1024;

uint64_t& PreviewTextureAccessCounter()
{
    static uint64_t counter = 0;
    return counter;
}

// Same policy as ModelCache::Trim, minus the live set: the preview panel has
// no persistent claim on any texture.
void TrimPreviewTextureCache()
{
    auto& cache = GetPreviewTextureCache();

    size_t total = 0;
    std::vector<std::pair<uint64_t, std::string>> evictable;
    evictable.reserve(cache.size());
    for (const auto& [key, entry] : cache)
    {
        total += entry.texture.pixels.size();
        evictable.emplace_back(entry.lastAccess, key);
    }

    if (total <= kPreviewTextureBudgetBytes)
    {
        return;
    }

    std::sort(evictable.begin(), evictable.end());
    for (const auto& [lastAccess, key] : evictable)
    {
        if (total <= kPreviewTextureBudgetBytes)
        {
            break;
        }
        const auto it = cache.find(key);
        if (it == cache.end())
        {
            continue;
        }
        total -= it->second.texture.pixels.size();
        cache.erase(it);
    }
}
```

- [ ] **Step 3: Bump on access and trim after inserting**

In `ResolvePreviewTexture`, after `CachedPreviewTexture& cached = cache[normalizedPath.string()];`:

```cpp
    cached.lastAccess = ++PreviewTextureAccessCounter();
```

At the end of the `if (reloadRequired)` block, after the `try`/`catch`, add the trim. It must come last, and the function must not hold `cached` across it — trimming can erase entries and invalidate the reference. Restructure the tail of the function to:

```cpp
    if (reloadRequired)
    {
        try
        {
            cached.texture = TextureLoader::LoadRGBA8(normalizedPath.string());
            cached.lastWriteTime = errorCode ? std::filesystem::file_time_type{} : lastWriteTime;
            cached.resolved = true;
            cached.available = cached.texture.IsValid();
        }
        catch (...)
        {
            cached = CachedPreviewTexture{};
            cached.resolved = true;
            cached.available = false;
        }

        // Trim can erase map entries, so re-look-up afterwards rather than
        // holding `cached` across it.
        TrimPreviewTextureCache();
        const auto it = cache.find(normalizedPath.string());
        return (it != cache.end() && it->second.available) ? &it->second.texture : nullptr;
    }

    return cached.available ? &cached.texture : nullptr;
```

Confirm `<algorithm>`, `<cstdint>`, `<utility>` and `<vector>` are included by this file; add whichever is missing.

- [ ] **Step 4: Build, test, format, smoke**

```bash
cmake --build --preset vs2026-x64-debug --parallel
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60
```

**Still needs a human:** open the material graph panel on an imported model and click through several materials with textures. The previews must keep rendering; the budget is far above what a handful of materials reaches, so nothing should visibly change.

- [ ] **Step 5: Commit**

```bash
git add engine/editor/ui/editor_model_preview.cpp
git commit -F - <<'EOF'
feat(editor): bound the preview texture cache

Decoded full-resolution RGBA8 previews were held in a function-local static
map with no bound and no eviction. A 4K base color map is 32 MB decoded and
the panel touches a new one every time the user clicks a different material.

Same counter-based LRU as ModelCache::Trim, minus the live set -- the preview
panel has no persistent claim on any texture -- against a 256 MB budget.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 6: Extract the reference scan behind an mtime index

**Files:**
- Create: `engine/asset/asset_references.h`, `engine/asset/asset_references.cpp`, `tests/asset_references_tests.cpp`
- Modify: `engine/asset/CMakeLists.txt`, `engine/asset/asset_manager.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `struct me::AssetReference { std::string referencedName; std::string referencedBy; };`
  - `std::vector<me::AssetReference> me::FindReferencesTo(const std::filesystem::path& root, const std::vector<std::string>& names, const std::unordered_set<std::string>& excludePaths, size_t maxResults = 6)`

  Task 7 calls it a second time from the rename flow.

- [ ] **Step 1: Write the failing test**

Create `tests/asset_references_tests.cpp`:

```cpp
#include <engine/asset/asset_references.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class ScopedTree
{
  public:
    explicit ScopedTree(const char* name)
    {
        std::error_code ec;
        const std::filesystem::path base =
            std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        m_root = (ec ? std::filesystem::temp_directory_path() : base) /
                 "miniengine_asset_references_tests" / name;
        std::filesystem::remove_all(m_root, ec);
        std::filesystem::create_directories(m_root, ec);
    }

    ~ScopedTree()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    ScopedTree(const ScopedTree&) = delete;
    ScopedTree& operator=(const ScopedTree&) = delete;

    const std::filesystem::path& Root() const
    {
        return m_root;
    }

    void Write(const std::string& relativePath, const std::string& contents) const
    {
        const std::filesystem::path full = m_root / relativePath;
        std::error_code ec;
        std::filesystem::create_directories(full.parent_path(), ec);
        std::ofstream out(full, std::ios::binary | std::ios::trunc);
        out << contents;
    }
};

void FindsAMention()
{
    ScopedTree tree("mention");
    tree.Write("mat_0.material.yaml", "material:\n  base_color_texture_path: textures/tex.png\n");
    tree.Write("unrelated.material.yaml", "material:\n  base_color_texture_path: textures/other.png\n");

    const std::vector<AssetReference> found = FindReferencesTo(tree.Root(), {"tex.png"}, {});
    Require(found.size() == 1, "expected exactly one reference to tex.png");
    Require(found[0].referencedName == "tex.png", "reported the wrong referenced name");
    Require(found[0].referencedBy == "mat_0.material.yaml", "reported the wrong referencing document");
}

void SkipsSidecarsAndExclusions()
{
    ScopedTree tree("skips");
    // A uuid sidecar names its own asset by design; that is not a reference.
    tree.Write("tex.png.miniengine_asset.yaml", "asset:\n  uuid: x\n  file: tex.png\n");
    tree.Write("excluded.material.yaml", "material:\n  base_color_texture_path: tex.png\n");

    const std::unordered_set<std::string> excluded{
        (tree.Root() / "excluded.material.yaml").lexically_normal().string()};

    const std::vector<AssetReference> found = FindReferencesTo(tree.Root(), {"tex.png"}, excluded);
    Require(found.empty(), "a uuid sidecar or an excluded path was reported as a reference");
}

// A document whose last_write_time has not moved must be answered from the
// index rather than re-read. Proven by changing the content while restoring
// the timestamp: the stale answer coming back is the evidence.
void IndexIsReused()
{
    ScopedTree tree("index_reuse");
    tree.Write("doc.material.yaml", "material:\n  base_color_texture_path: tex.png\n");

    const std::filesystem::path doc = tree.Root() / "doc.material.yaml";
    std::error_code ec;
    const std::filesystem::file_time_type original = std::filesystem::last_write_time(doc, ec);
    Require(!ec, "could not read the document's last_write_time");

    Require(FindReferencesTo(tree.Root(), {"tex.png"}, {}).size() == 1, "first scan missed the reference");

    // Content no longer mentions tex.png, but the timestamp says unchanged.
    tree.Write("doc.material.yaml", "material:\n  base_color_texture_path: nothing.png\n");
    std::filesystem::last_write_time(doc, original, ec);
    Require(!ec, "could not restore the document's last_write_time");

    Require(
        FindReferencesTo(tree.Root(), {"tex.png"}, {}).size() == 1,
        "the index was not reused: the document was re-read despite an unchanged timestamp");
}

void IndexIsInvalidated()
{
    ScopedTree tree("index_invalidate");
    tree.Write("doc.material.yaml", "material:\n  base_color_texture_path: tex.png\n");
    Require(FindReferencesTo(tree.Root(), {"tex.png"}, {}).size() == 1, "first scan missed the reference");

    // Rewrite and let the timestamp advance normally.
    tree.Write("doc.material.yaml", "material:\n  base_color_texture_path: nothing.png\n");
    std::error_code ec;
    std::filesystem::last_write_time(
        tree.Root() / "doc.material.yaml",
        std::filesystem::file_time_type::clock::now(),
        ec);

    Require(
        FindReferencesTo(tree.Root(), {"tex.png"}, {}).empty(),
        "the index was not invalidated by a changed timestamp");
}
}

int main()
{
    try
    {
        FindsAMention();
        SkipsSidecarsAndExclusions();
        IndexIsReused();
        IndexIsInvalidated();

        std::cout << "asset references tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "asset references tests failed: " << error.what() << '\n';
        return 1;
    }
}
```

Add to the end of `tests/CMakeLists.txt`:

```cmake
# asset_references.cpp depends only on engine_core, so it is compiled directly into the test
# rather than linking engine_asset, keeping imgui out of a unit test. Same reasoning as
# miniengine_asset_registry_tests above.
add_executable(miniengine_asset_references_tests
    asset_references_tests.cpp
    ${PROJECT_SOURCE_DIR}/engine/asset/asset_references.cpp
)

# No miniengine_group_target_sources: that helper's source_group(TREE ...) requires every file
# to sit under the calling directory, and this target deliberately compiles one .cpp from
# engine/asset/.

target_include_directories(miniengine_asset_references_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(miniengine_asset_references_tests
    PRIVATE
        engine_core
)

if(WIN32)
    add_custom_command(TARGET miniengine_asset_references_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_asset_references_tests>
            $<TARGET_FILE_DIR:miniengine_asset_references_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

add_test(
    NAME miniengine.asset_references
    COMMAND miniengine_asset_references_tests
)

set_target_properties(miniengine_asset_references_tests PROPERTIES FOLDER "Tests")
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --preset vs2026-x64
```

Expected: CMake error — `engine/asset/asset_references.cpp` does not exist.

- [ ] **Step 3: Write the header**

Create `engine/asset/asset_references.h`:

```cpp
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace me
{

// One document mentioning one doomed file name.
struct AssetReference
{
    std::string referencedName; // the file name that was searched for
    std::string referencedBy;   // file name of the document mentioning it
};

// Scans .gltf/.yaml documents under `root` for mentions of any of `names`,
// skipping uuid sidecars and anything in `excludePaths` (each entry spelled as
// lexically_normal().string()).
//
// Substring matching on the file name: it can flag a same-named file in
// another folder, but a spurious warning is cheap next to a silently broken
// reference.
//
// Backed by a process-wide index keyed on each document's last_write_time, so
// repeat scans re-read only what changed. The first scan still walks the whole
// tree; it is the repeat case -- which is what a rename triggers -- that the
// index makes affordable.
std::vector<AssetReference> FindReferencesTo(
    const std::filesystem::path& root,
    const std::vector<std::string>& names,
    const std::unordered_set<std::string>& excludePaths,
    size_t maxResults = 6);
}
```

- [ ] **Step 4: Write the implementation**

Create `engine/asset/asset_references.cpp`:

```cpp
#include "asset_references.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace me
{

namespace
{
constexpr std::string_view kSidecarSuffix = ".miniengine_asset.yaml";

// A document over this size is skipped rather than read into memory.
constexpr std::uintmax_t kMaxScanFileBytes = 64ull * 1024 * 1024;

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
                   {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool IsScannableDocument(const std::filesystem::path& path)
{
    const std::string extension = ToLowerCopy(path.extension().string());
    if (extension != ".gltf" && extension != ".yaml" && extension != ".yml")
    {
        return false;
    }
    // A uuid sidecar names its own asset by design; that is not a reference.
    return !path.filename().string().ends_with(kSidecarSuffix);
}

struct IndexedDocument
{
    std::filesystem::file_time_type lastWriteTime{};
    std::string content;
};

struct ReferenceIndex
{
    std::mutex mutex;
    std::unordered_map<std::string, IndexedDocument> documents; // normalized path -> content
};

ReferenceIndex& Index()
{
    static ReferenceIndex index;
    return index;
}

// Returns the document's content, from the index when its timestamp is
// unchanged, otherwise by reading it. Returns false when it could not be read,
// in which case the caller skips it.
//
// Caller must hold Index().mutex: this touches the index without locking.
bool ReadDocument(const std::filesystem::path& path, std::string& contentOut)
{
    const std::string key = path.lexically_normal().string();

    std::error_code timeEc;
    const std::filesystem::file_time_type lastWriteTime = std::filesystem::last_write_time(path, timeEc);

    ReferenceIndex& index = Index();
    if (!timeEc)
    {
        const auto it = index.documents.find(key);
        if (it != index.documents.end() && it->second.lastWriteTime == lastWriteTime)
        {
            contentOut = it->second.content;
            return true;
        }
    }

    std::error_code sizeEc;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeEc);
    if (sizeEc || size > kMaxScanFileBytes)
    {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }
    contentOut.assign(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());

    // A timestamp we could not read means we cannot index it: re-read next
    // time rather than trusting a stale entry.
    if (!timeEc)
    {
        index.documents[key] = IndexedDocument{lastWriteTime, contentOut};
    }
    return true;
}
}

std::vector<AssetReference> FindReferencesTo(
    const std::filesystem::path& root,
    const std::vector<std::string>& names,
    const std::unordered_set<std::string>& excludePaths,
    size_t maxResults)
{
    std::vector<AssetReference> found;
    if (names.empty() || maxResults == 0)
    {
        return found;
    }

    std::lock_guard lock(Index().mutex);

    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator
             it(root, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end;
         it.increment(ec))
    {
        if (found.size() >= maxResults)
        {
            break;
        }

        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc)
        {
            continue;
        }
        const std::filesystem::path& path = it->path();
        if (!IsScannableDocument(path))
        {
            continue;
        }
        if (excludePaths.count(path.lexically_normal().string()) > 0)
        {
            continue;
        }

        std::string content;
        if (!ReadDocument(path, content))
        {
            continue;
        }

        for (const std::string& name : names)
        {
            if (content.find(name) == std::string::npos)
            {
                continue;
            }
            found.push_back(AssetReference{name, path.filename().string()});
            if (found.size() >= maxResults)
            {
                break;
            }
        }
    }

    return found;
}
}
```

Add both files to `engine/asset/CMakeLists.txt`'s `add_library(engine_asset ...)` source list, keeping the list alphabetical — `asset_references.cpp` and `asset_references.h` go directly after `asset_registry.h`.

- [ ] **Step 5: Run the reference tests**

```bash
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug --parallel --target miniengine_asset_references_tests
ctest --test-dir out/build/vs2026-x64 -C Debug -R miniengine.asset_references --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Make the delete flow a caller**

In `engine/asset/asset_manager.cpp`, add `#include "asset_references.h"` after the `asset_registry.h` include, and replace the whole scanning half of `BuildPendingDeleteWarnings` — everything from the `// Look through the files that can hold references` comment to the end of the function — with:

```cpp
    // Delegated so the rename flow can ask the same question without paying
    // for a second whole-tree read.
    const std::vector<AssetReference> references =
        FindReferencesTo(m_root, deletedNames, deletedPaths, kMaxWarnings);
    for (const AssetReference& reference : references)
    {
        m_pendingDeleteWarnings.push_back(
            "'" + reference.referencedName + "' is referenced by " + reference.referencedBy);
    }
}
```

`kMaxWarnings` is already declared in that function; keep its declaration and delete `kMaxScanFileBytes`, which now lives in `asset_references.cpp`. `deletedPaths` is already an `std::unordered_set<std::string>` of `lexically_normal().string()` entries, which is exactly what `excludePaths` wants.

- [ ] **Step 7: Run the whole suite, format and smoke**

```bash
cmake --build --preset vs2026-x64-debug --parallel
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60
```

- [ ] **Step 8: Commit**

```bash
git add engine/asset/asset_references.h engine/asset/asset_references.cpp engine/asset/CMakeLists.txt engine/asset/asset_manager.cpp tests/asset_references_tests.cpp tests/CMakeLists.txt
git commit -F - <<'EOF'
refactor(asset): extract the reference scan behind an mtime index

Clicking Delete read every .gltf and .yaml under the assets root fully into
memory, synchronously on the main thread, and substring-searched each for up
to 256 doomed names. That was already slow for delete; it is unaffordable for
rename, which is an F2-frequency operation.

FindReferencesTo is now a unit of its own, backed by a process-wide index
keyed on each document's last_write_time. The first scan still walks the tree;
repeat scans re-read only what changed, which is what makes the rename caller
in the next commit viable. A document whose timestamp cannot be read is
re-read rather than trusted from the index.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 7: Rename checks references and invalidates the model cache

No unit test: this is ImGui panel state and a modal flow. The logic it depends on (`FindReferencesTo`, `ModelCache::Invalidate`) is tested in Tasks 6 and 3.

**Files:**
- Modify: `engine/asset/asset_manager.h`, `engine/asset/asset_manager.cpp`

**Interfaces:**
- Consumes: `FindReferencesTo` (Task 6), `ModelCache::Invalidate` (existing).
- Produces: no public API.

- [ ] **Step 1: Add the pending-rename state**

In `engine/asset/asset_manager.h`, add `#include <optional>` if absent, and after the delete-modal members:

```cpp
    // A rename whose target is referenced by other documents is staged here
    // until the user confirms it, mirroring the delete flow. Paths, not
    // indices: the entry list can be rescanned between staging and confirming.
    struct PendingRename
    {
        std::string sourcePath;
        std::string newName;
        bool isDir = false;
    };
    std::optional<PendingRename> m_pendingRename;
    std::vector<std::string> m_pendingRenameWarnings;
    bool m_openRenameModal = false;
```

Declare the two new methods next to `CommitRename`:

```cpp
    void PerformRename(const PendingRename& rename);
    void DrawRenameConfirmModal();
```

- [ ] **Step 2: Split the rename into check and perform**

In `engine/asset/asset_manager.cpp`, replace `CommitRename`'s tail — everything from `const std::filesystem::path target = ...` to the end — with:

```cpp
    const std::filesystem::path target = entry.path.parent_path() / newName;
    std::error_code ec;
    if (std::filesystem::exists(target, ec))
    {
        return; // never clobber an existing file/folder
    }

    const PendingRename rename{entry.path.string(), newName, entry.isDir};

    // Renaming a file breaks every path-based reference to its old name, the
    // same breakage deleting it causes. Delete warns; rename used to go
    // through silently.
    const std::vector<AssetReference> references =
        FindReferencesTo(m_root, {entry.name}, {entry.path.lexically_normal().string()});
    if (!references.empty())
    {
        m_pendingRenameWarnings.clear();
        for (const AssetReference& reference : references)
        {
            m_pendingRenameWarnings.push_back(
                "'" + reference.referencedName + "' is referenced by " + reference.referencedBy);
        }
        m_pendingRename = rename;
        m_openRenameModal = true;
        return; // the modal performs the rename on confirmation
    }

    PerformRename(rename);
}

void AssetManager::PerformRename(const PendingRename& rename)
{
    const std::filesystem::path source(rename.sourcePath);
    const std::filesystem::path target = source.parent_path() / rename.newName;

    std::error_code ec;
    std::filesystem::rename(source, target, ec);
    if (!ec)
    {
        // Parsed model data is keyed on path. Without this, a model re-imported
        // later at the old path is served the previous file's data.
        ModelCache::Invalidate(rename.sourcePath);

        // Keep the uuid registry and companion sidecars pointing at the new name.
        AssetRegistry::OnAssetRenamed(source, target);
        if (!rename.isDir)
        {
            RenameModelMaterialSidecars(source, target);
        }
    }
    m_needsScan = true;
}
```

Add `#include "model_cache.h"` next to the other local includes at the top of the file.

- [ ] **Step 3: Draw the confirmation modal**

Add after `DrawDeleteConfirmModal`:

```cpp
void AssetManager::DrawRenameConfirmModal()
{
    constexpr const char* kTitle = "Rename Referenced Asset?";

    if (m_openRenameModal)
    {
        ImGui::OpenPopup(kTitle);
        m_openRenameModal = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_pendingRename.has_value())
        {
            ImGui::Text(
                "Rename '%s' to '%s'?",
                std::filesystem::path(m_pendingRename->sourcePath).filename().string().c_str(),
                m_pendingRename->newName.c_str());
        }
        ImGui::Spacing();

        for (const std::string& warning : m_pendingRenameWarnings)
        {
            ImGui::TextColored(ImVec4(1.00f, 0.55f, 0.35f, 1.0f), "%s", warning.c_str());
        }
        ImGui::TextDisabled("Those references are paths, not uuids: renaming breaks them.");
        ImGui::Separator();

        if (ImGui::Button("Rename Anyway", ImVec2(140.0f, 0.0f)))
        {
            if (m_pendingRename.has_value())
            {
                PerformRename(*m_pendingRename);
            }
            m_pendingRename.reset();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        {
            m_pendingRename.reset();
            m_needsScan = true; // refresh the list; the inline edit already closed
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();

        ImGui::EndPopup();
    }
    else if (m_pendingRename.has_value())
    {
        // Dismissed without an explicit choice (e.g. Escape): treat as cancel.
        m_pendingRename.reset();
        m_needsScan = true;
    }
}
```

In `AssetManager::Draw`, after `DrawDeleteConfirmModal(result);`:

```cpp
    DrawRenameConfirmModal();
```

- [ ] **Step 4: Build, test, format, smoke**

```bash
cmake --build --preset vs2026-x64-debug --parallel
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60
```

**Still needs a human**, and this is the task that most needs it:

1. In the Assets panel, rename a file nothing references (a spare `.png` in an empty folder). It must rename immediately with no modal.
2. Rename a texture that a `.gltf` references. The modal must appear, list the referencing document, and only rename after "Rename Anyway".
3. Cancel one. Nothing renames, and the inline edit field goes away.

- [ ] **Step 5: Commit**

```bash
git add engine/asset/asset_manager.h engine/asset/asset_manager.cpp
git commit -F - <<'EOF'
feat(asset): check references and invalidate the cache on rename

Deleting an asset warned about documents referencing it; renaming the same
asset broke exactly the same references in silence. Rename now asks the same
question through FindReferencesTo and routes through a confirmation modal when
the answer is not empty.

Rename also invalidates ModelCache, which only the delete path did. Without
it, a model re-imported later at the old path is served the previous file's
parsed data.

The staged rename holds paths rather than entry indices: the entry list can be
rescanned between staging and confirming.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 8: Record the change

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add the development record**

In section 10, directly above the `### 2026-09-12 — 嵌入式贴图改为导入时解包` entry:

```markdown
### 2026-09-12 — 资产生命周期加固

同一轮审查里的六条缺陷，分三组，组内耦合、组间独立。

**注册表安全性**

- 资产浏览器预览面板此前**每帧**调 `GetOrCreateUuid`：全局注册表互斥锁 + 一次 `is_regular_file` + 可能写 sidecar。而后台导入线程在同一把锁上跑整棵资产树的 `RescanAssetTree`，UI 线程被它按住。UUID 在文件生命周期内不变，现在只在焦点移动或条目列表重建时算一次。
- 孤儿 sidecar 的判定此前是 `exists()` 返回 false，而它在"无法判断"时同样返回 false——一次瞬时 IO 错误就永久丢掉 UUID。现在要求 `error_code` 干净才算证据。
- `WriteSidecar` 此前截断后直接流式写，中途崩溃留下空文件，读回来就是"没有 UUID"。改成写临时文件 + `rename` 覆盖。临时文件名以 `.tmp` 结尾，不匹配 sidecar 后缀，因此不会被扫描误认。

**缓存生命周期**

- `ModelCache::Get` 此前返回可变 `shared_ptr`，两处调用方直接改缓存里的材质。今天两处都在主线程，没有真的 data race，但接口没这么说，而后台线程随时可以 `Store` 覆盖同一个键。`Get` 改为返回 `shared_ptr<const LoadedModelData>`，新增 `UpdateMaterial`/`UpdateMaterials` 两个显式写入口，在缓存锁内应用，路径缺失或索引越界时是 no-op——正是调用方原先自己带的那两个守卫。
- 缓存此前无上限无淘汰，Sponza 级别的一个包就是几百 MB，进去就常驻到进程退出。现在按字节计量 + LRU 淘汰，预算 1 GiB。**场景仍引用的条目永不淘汰**，无论预算：淘汰它只会在下次重建 renderable 时触发一次同步重新加载，那是体验倒退不是节省。淘汰在 live set 真正可能变化的两个点评估——`RebuildSceneRenderables` 末尾，以及报告了变化的 `RefreshDirtySceneRenderables` 末尾——而不是每帧空跑。
- 预览贴图缓存（解码后的全分辨率 RGBA8，一张 4K 基色图解码就是 32 MB）用同样的计数器 LRU，预算 256 MB，但没有 live set——预览面板对任何一张贴图都没有持久占有。

**引用完整性**

- 点一次 Delete 此前是主线程同步把资产树下每个 ≤64 MB 的 `.gltf`/`.yaml` 全文读进内存，对最多 256 个文件名逐个子串搜索。这对删除已经慢，对重命名（F2 高频操作）根本不可行。
- 扫描提取成 `engine/asset/asset_references.{h,cpp}` 的 `FindReferencesTo`，背后是按 `last_write_time` 索引的文档内容缓存：首次仍走全树，重复扫描只重读变化的文档。时间戳读不到的文档会重读而不是信任索引。
- 重命名由此能用上同一个检查：有引用就弹和删除同款的确认框。材质里的贴图引用是纯路径（见第 7 节），重命名必然打断它们——此前是静默打断。
- 重命名还会 `ModelCache::Invalidate`，此前只有删除路径做了。否则之后在老路径上重新导入的模型会被喂上一个文件的解析数据。

设计见 [docs/superpowers/specs/2026-09-12-asset-lifecycle-hardening-design.md](docs/superpowers/specs/2026-09-12-asset-lifecycle-hardening-design.md)。给材质贴图引用加 UUID 能让重命名经注册表自动修复、彻底不需要扫描，但那要改 `.material.yaml` 格式、`ModelMaterialData` 和解析路径，比其余五项加起来还大，仍是已知缺口。

```

- [ ] **Step 2: Verify the documented symbols exist**

```bash
grep -n "FindReferencesTo" engine/asset/asset_references.h engine/asset/asset_manager.cpp
grep -n "UpdateMaterial\|EstimateBytes\|Trim" engine/asset/model_cache.h
grep -n "TrimModelCache" engine/editor/services/scene_renderables.cpp
```

Expected: every symbol the record names exists where it says.

- [ ] **Step 3: Final full verification**

```bash
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60
```

Expected: all tests pass, format clean, smoke exits 0.

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -F - <<'EOF'
docs(asset): record the asset lifecycle hardening

Section 10 gains a dated entry covering all six defects, why eviction pins the
scene's live set rather than being purely reference-driven, and why texture
references inside materials stay path-only for now.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

## Verification Summary

| Gate | Command | Covers |
| --- | --- | --- |
| Registry rules | `ctest -R miniengine.asset_registry` | Task 1 |
| Cache interface and eviction | `ctest -R miniengine.model_cache` | Tasks 3, 4 |
| Reference scan and index | `ctest -R miniengine.asset_references` | Task 6 |
| Whole suite | `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure` | every task |
| Formatting | `.\scripts\check-format.ps1` | every task |
| Startup path | `miniengine_app.exe --backend vulkan --frames 60` | Tasks 2, 4, 5, 7 |

Tasks 2, 5 and 7 change UI code that no automated gate here exercises. Their "Still needs a human" steps are the actual acceptance criteria for those tasks; a green suite does not stand in for them.
