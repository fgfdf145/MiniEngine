# Embedded Texture Unpacking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move embedded glTF image extraction from load time into import time, make every texture path model-relative, auto-import models referenced from outside the assets root, and give `AssetRegistry` its first tests.

**Architecture:** `GltfModelLoader` gains `UnpackEmbeddedTextures`, called at the end of `ModelLoader::CopyModelWithSortedReferences` for both `.glb` and `.gltf`. `ResolveImagePath` stops writing files and instead derives `textures/<name>` and confirms it exists, so `LoadModel` becomes read-only. The path-keyed cache directory and its FNV1a key derivation are deleted. A guard at the `pendingModelLoads` drain imports any model that is not under the assets root.

**Tech Stack:** C++20, tinygltf 3.0.0, stb_image / stb_image_write, nlohmann::json, yaml-cpp, CMake + vcpkg, CTest.

**Spec:** [docs/superpowers/specs/2026-09-12-embedded-texture-unpacking-design.md](../specs/2026-09-12-embedded-texture-unpacking-design.md)

## Global Constraints

- Language standard is C++20. Formatting is Allman throughout; `scripts/check-format.ps1` is a gate.
- All engine code lives in `namespace me`.
- Target dependency direction may not be reversed. `engine_asset` may not include anything from `engine_editor`; the auto-import guard therefore lives in `engine/editor/`, which may call both.
- `CMakePresets.json` is the only source of truth for build parameters. Configure with `cmake --preset vs2026-x64`, build with `cmake --build --preset vs2026-x64-debug --parallel`, and run tests as `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`. `CMakePresets.json` defines no `testPresets`, so `ctest --preset ...` does not work in this repo.
- Texture loading applies no vertical flip: UV origin is top-left, row 0 is `v0`. Never introduce a `1 - v` compensation.
- World units are metres, per `engine/scene/world_units.h`.
- Import never overwrites an existing destination file. Every new write in this plan skips a destination that already exists.
- `ModelLoader::LoadModel` must not write to the filesystem after Task 3. That is the property Task 3 exists to establish; do not reintroduce a write behind it.

## Path Contract

Stated once, relied on by Tasks 2 through 4:

> Every path in `ModelMaterialData`'s texture fields — and therefore every texture path in a `.material.yaml` — is relative to the directory holding the model file.

## File Structure

**Create:**

| File | Responsibility |
| --- | --- |
| `tests/asset_registry_tests.cpp` | Uuid minting, boundary rejection, duplicate arbitration under both scan orders, orphan sidecar pruning, three-tier reference resolution, rename and removal. |
| `tests/model_import_texture_tests.cpp` | Writes a synthetic `.gltf` carrying a `data:` URI image, then asserts import unpacks it and load reports a model-relative path. |

**Modify:**

| File | Change |
| --- | --- |
| `engine/asset/asset_registry.h` / `.cpp` | Add `IsUnderAssetsRoot`. |
| `engine/asset/gltf_model_loader.h` / `.cpp` | Add `UnpackEmbeddedTextures`; rewrite `ResolveImagePath`'s embedded branch; delete `BuildCacheKey`, `BuildEmbeddedTextureCacheDirectory`, `ExportEmbeddedImage`. |
| `engine/asset/model_loader.cpp` | Call `UnpackEmbeddedTextures` after both copy branches. |
| `engine/editor/editor_backend_base.cpp` | Import models that are not under the assets root before loading them. |
| `tests/CMakeLists.txt` | Two new test targets. |
| `README.md` | Import unpacking rule in section 5; dated entry in section 10. |

## Deferred

Recorded so no task reaches for them. All are findings from the same review, deliberately out of scope:

- Moving `AssetManager` out of `engine/asset` and dropping imgui from that target.
- Per-frame `GetOrCreateUuid` in the asset browser preview panel.
- Orphan sidecar deletion keyed on an `exists()` that also returns false on error; non-atomic sidecar writes.
- Rename not checking references and not invalidating `ModelCache`.
- `ModelCache` / preview texture cache eviction; `ModelCache::Get` returning a mutable handle.
- `STBI_THREAD_LOCAL`; `BuildMaterialDefinitionPath` stem collisions; `ScanCurrentDir` error handling; paste producing broken single-file copies.

---

### Task 1: AssetRegistry test coverage and `IsUnderAssetsRoot`

The registry is the safety net for Tasks 2 through 4, so it gets tested first.

**Files:**
- Create: `tests/asset_registry_tests.cpp`
- Modify: `engine/asset/asset_registry.h`, `engine/asset/asset_registry.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `bool me::AssetRegistry::IsUnderAssetsRoot(const std::filesystem::path& path)` — true only for paths strictly beneath the current assets root. Task 4 calls it.

- [ ] **Step 1: Write the failing test**

Create `tests/asset_registry_tests.cpp`:

```cpp
#include <engine/asset/asset_registry.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

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

// Each case owns an isolated asset tree so a leftover sidecar from one case
// cannot decide another case's arbitration.
class ScopedAssetRoot
{
  public:
    explicit ScopedAssetRoot(const char* caseName)
    {
        std::error_code ec;
        const std::filesystem::path base =
            std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        m_root = (ec ? std::filesystem::temp_directory_path() : base) /
                 "miniengine_asset_registry_tests" / caseName;
        std::filesystem::remove_all(m_root, ec);
        std::filesystem::create_directories(m_root, ec);
        AssetRegistry::Initialize(m_root);
    }

    ~ScopedAssetRoot()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    ScopedAssetRoot(const ScopedAssetRoot&) = delete;
    ScopedAssetRoot& operator=(const ScopedAssetRoot&) = delete;

    const std::filesystem::path& Root() const
    {
        return m_root;
    }

  private:
    std::filesystem::path m_root;
};

void WriteFile(const std::filesystem::path& path, const std::string& contents)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

bool Exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

void MintAndPersist()
{
    ScopedAssetRoot scope("mint");
    const std::filesystem::path asset = scope.Root() / "tex.png";
    WriteFile(asset, "not really a png");

    const std::string uuid = AssetRegistry::GetOrCreateUuid(asset);
    Require(!uuid.empty(), "registrable asset did not receive a uuid");
    Require(Exists(AssetRegistry::SidecarPathFor(asset)), "sidecar was not written");
    Require(AssetRegistry::GetOrCreateUuid(asset) == uuid, "second call minted a different uuid");

    const std::optional<std::filesystem::path> resolved = AssetRegistry::ResolveUuid(uuid);
    Require(resolved.has_value(), "minted uuid did not resolve");
}

void BoundaryRejection()
{
    ScopedAssetRoot scope("boundary");
    const std::filesystem::path inside = scope.Root() / "inside.png";
    const std::filesystem::path wrongType = scope.Root() / "notes.txt";
    const std::filesystem::path outside = scope.Root().parent_path() / "outside.png";
    WriteFile(inside, "x");
    WriteFile(wrongType, "x");
    WriteFile(outside, "x");

    Require(AssetRegistry::GetOrCreateUuid(wrongType).empty(), "non-registrable extension got a uuid");
    Require(AssetRegistry::GetOrCreateUuid(outside).empty(), "asset outside the root got a uuid");
    Require(!AssetRegistry::GetOrCreateUuid(inside).empty(), "asset inside the root was rejected");

    Require(AssetRegistry::IsUnderAssetsRoot(inside), "inside path reported as outside");
    Require(!AssetRegistry::IsUnderAssetsRoot(outside), "outside path reported as inside");
    Require(!AssetRegistry::IsUnderAssetsRoot(scope.Root()), "the root itself reported as under itself");

    std::error_code ec;
    std::filesystem::remove(outside, ec);
}

// An asset copied together with its sidecar leaves two files claiming one
// uuid. Scan order must not decide the winner: the sidecar's recorded file
// name breaks the tie. Asserted in both directions.
void DuplicateArbitration(bool originalFirst)
{
    ScopedAssetRoot scope(originalFirst ? "duplicate_original_first" : "duplicate_copy_first");

    // "aaa" sorts before "zzz", so naming controls which file the recursive
    // scan reaches first.
    const std::string originalName = originalFirst ? "aaa_original.png" : "zzz_original.png";
    const std::string copyName = originalFirst ? "zzz_copy.png" : "aaa_copy.png";

    const std::filesystem::path original = scope.Root() / originalName;
    const std::filesystem::path copy = scope.Root() / copyName;
    WriteFile(original, "pixels");
    WriteFile(copy, "pixels");

    // Both sidecars carry the same uuid, but each records the file name it was
    // written for. Only the original's matches its own file name.
    const std::string sharedUuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    const std::string sidecar = "asset:\n  uuid: " + sharedUuid + "\n  file: " + originalName + "\n";
    WriteFile(AssetRegistry::SidecarPathFor(original), sidecar);
    WriteFile(AssetRegistry::SidecarPathFor(copy), sidecar);

    AssetRegistry::RescanAssetTree();

    const std::string originalUuid = AssetRegistry::GetOrCreateUuid(original);
    const std::string copyUuid = AssetRegistry::GetOrCreateUuid(copy);
    Require(!originalUuid.empty() && !copyUuid.empty(), "arbitration left an asset unregistered");
    Require(originalUuid != copyUuid, "duplicate uuids were not arbitrated");
    Require(originalUuid == sharedUuid, "the original did not keep the shared uuid");

    const std::optional<std::filesystem::path> owner = AssetRegistry::ResolveUuid(sharedUuid);
    Require(owner.has_value(), "the shared uuid resolves to nothing after arbitration");
    Require(owner->filename() == originalName, "the shared uuid resolved to the copy");
}

void OrphanSidecarPruning()
{
    ScopedAssetRoot scope("orphan");
    const std::filesystem::path asset = scope.Root() / "gone.png";
    WriteFile(asset, "x");
    AssetRegistry::GetOrCreateUuid(asset);

    const std::filesystem::path sidecar = AssetRegistry::SidecarPathFor(asset);
    Require(Exists(sidecar), "sidecar was not created before the orphan check");

    std::error_code ec;
    std::filesystem::remove(asset, ec);
    AssetRegistry::RescanAssetTree();

    Require(!Exists(sidecar), "orphaned sidecar survived a rescan");
}

void ReferenceResolution()
{
    ScopedAssetRoot scope("resolve");
    const std::filesystem::path asset = scope.Root() / "models" / "mesh.glb";
    WriteFile(asset, "x");
    const std::string uuid = AssetRegistry::GetOrCreateUuid(asset);

    // Tier 1: a known uuid beats a stale stored path.
    const ResolvedAssetReference byUuid =
        AssetRegistry::ResolveReference(uuid, (scope.Root() / "old" / "mesh.glb").string());
    Require(byUuid.resolved, "known uuid did not resolve");
    Require(byUuid.healed, "uuid resolution against a stale path did not report healed");
    Require(std::filesystem::path(byUuid.path).filename() == "mesh.glb", "uuid resolved to the wrong file");

    // Tier 2: an unknown uuid with a live stored path adopts and registers it.
    const std::filesystem::path adopted = scope.Root() / "models" / "adopted.png";
    WriteFile(adopted, "x");
    const ResolvedAssetReference byPath = AssetRegistry::ResolveReference("", adopted.string());
    Require(byPath.resolved, "live stored path was not adopted");
    Require(!byPath.uuid.empty(), "adopted path was not registered");

    // Tier 3: a unique filename match anywhere in the tree heals a dead path.
    const ResolvedAssetReference byName =
        AssetRegistry::ResolveReference("", (scope.Root() / "moved" / "mesh.glb").string());
    Require(byName.resolved, "unique filename match did not resolve");
    Require(byName.healed, "filename match did not report healed");
    Require(byName.uuid == uuid, "filename match resolved to the wrong asset");

    // Miss: nothing found, and the caller's data comes back untouched.
    const std::string missingPath = (scope.Root() / "nope" / "absent.glb").string();
    const ResolvedAssetReference miss = AssetRegistry::ResolveReference("no-such-uuid", missingPath);
    Require(!miss.resolved, "an unresolvable reference reported success");
    Require(miss.path == missingPath, "an unresolvable reference rewrote the stored path");
    Require(miss.uuid == "no-such-uuid", "an unresolvable reference rewrote the stored uuid");
}

void RenameKeepsIdentity()
{
    ScopedAssetRoot scope("rename");
    const std::filesystem::path before = scope.Root() / "before.png";
    WriteFile(before, "x");
    const std::string uuid = AssetRegistry::GetOrCreateUuid(before);

    const std::filesystem::path after = scope.Root() / "after.png";
    std::error_code ec;
    std::filesystem::rename(before, after, ec);
    Require(!ec, "test could not rename the asset");
    AssetRegistry::OnAssetRenamed(before, after);

    Require(AssetRegistry::GetOrCreateUuid(after) == uuid, "file rename did not preserve the uuid");
    Require(Exists(AssetRegistry::SidecarPathFor(after)), "sidecar did not follow the rename");
    Require(!Exists(AssetRegistry::SidecarPathFor(before)), "old sidecar was left behind");

    // Directory rename: every uuid beneath it survives.
    const std::filesystem::path nested = scope.Root() / "bundle" / "nested.png";
    WriteFile(nested, "x");
    const std::string nestedUuid = AssetRegistry::GetOrCreateUuid(nested);

    const std::filesystem::path renamedDir = scope.Root() / "bundle_renamed";
    std::filesystem::rename(scope.Root() / "bundle", renamedDir, ec);
    Require(!ec, "test could not rename the directory");
    AssetRegistry::OnAssetRenamed(scope.Root() / "bundle", renamedDir);

    Require(
        AssetRegistry::GetOrCreateUuid(renamedDir / "nested.png") == nestedUuid,
        "directory rename did not preserve a nested uuid");
}

void RemovalPrunes()
{
    ScopedAssetRoot scope("removal");
    const std::filesystem::path asset = scope.Root() / "doomed.png";
    WriteFile(asset, "x");
    const std::string uuid = AssetRegistry::GetOrCreateUuid(asset);
    const std::filesystem::path sidecar = AssetRegistry::SidecarPathFor(asset);

    std::error_code ec;
    std::filesystem::remove(asset, ec);
    AssetRegistry::OnAssetRemoved(asset);

    Require(!AssetRegistry::ResolveUuid(uuid).has_value(), "removed asset still resolves by uuid");
    Require(!Exists(sidecar), "sidecar outlived its asset");

    // Directory removal prunes the whole subtree.
    const std::filesystem::path nested = scope.Root() / "bundle" / "nested.png";
    WriteFile(nested, "x");
    const std::string nestedUuid = AssetRegistry::GetOrCreateUuid(nested);

    std::filesystem::remove_all(scope.Root() / "bundle", ec);
    AssetRegistry::OnAssetRemoved(scope.Root() / "bundle");

    Require(!AssetRegistry::ResolveUuid(nestedUuid).has_value(), "directory removal left a nested entry");
}
}

int main()
{
    try
    {
        MintAndPersist();
        BoundaryRejection();
        DuplicateArbitration(true);
        DuplicateArbitration(false);
        OrphanSidecarPruning();
        ReferenceResolution();
        RenameKeepsIdentity();
        RemovalPrunes();

        std::cout << "asset registry tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "asset registry tests failed: " << error.what() << '\n';
        return 1;
    }
}
```

Add to the end of `tests/CMakeLists.txt`:

```cmake
# asset_registry.cpp depends only on engine_core and yaml-cpp, so it is compiled directly into
# the test rather than linking engine_asset. engine_asset links imgui PUBLIC because
# asset_manager.cpp is an ImGui panel that still lives in the asset layer; compiling the one
# translation unit under test keeps that out of a unit test. Same reasoning as
# miniengine_scene_pass_tests above.
add_executable(miniengine_asset_registry_tests
    asset_registry_tests.cpp
    ${PROJECT_SOURCE_DIR}/engine/asset/asset_registry.cpp
)

# No miniengine_group_target_sources: that helper's source_group(TREE ...) requires every file
# to sit under the calling directory, and this target deliberately compiles one .cpp from
# engine/asset/.

target_include_directories(miniengine_asset_registry_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}"
)

target_link_libraries(miniengine_asset_registry_tests
    PRIVATE
        engine_core
        yaml-cpp::yaml-cpp
)

if(WIN32)
    add_custom_command(TARGET miniengine_asset_registry_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_asset_registry_tests>
            $<TARGET_FILE_DIR:miniengine_asset_registry_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

add_test(
    NAME miniengine.asset_registry
    COMMAND miniengine_asset_registry_tests
)

set_target_properties(miniengine_asset_registry_tests PROPERTIES FOLDER "Tests")
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug --parallel --target miniengine_asset_registry_tests
```

Expected: compile error — `IsUnderAssetsRoot` is not a member of `AssetRegistry`. The test also needs `<optional>`, which `asset_registry.h` already includes.

- [ ] **Step 3: Add `IsUnderAssetsRoot`**

In `engine/asset/asset_registry.h`, directly above `bool IsRegistrableAsset(...)`:

```cpp
// True only for paths strictly beneath the assets root. The root itself is
// not "under" itself.
bool IsUnderAssetsRoot(const std::filesystem::path& path);
```

In `engine/asset/asset_registry.cpp`, directly above `bool IsRegistrableAsset(...)`:

```cpp
bool IsUnderAssetsRoot(const std::filesystem::path& path)
{
    std::lock_guard lock(State().mutex);
    EnsureInitializedLocked();
    return IsUnderRootLocked(NormalizeKey(path));
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build --preset vs2026-x64-debug --parallel --target miniengine_asset_registry_tests
ctest --test-dir out/build/vs2026-x64 -C Debug -R miniengine.asset_registry --output-on-failure
```

Expected: PASS, printing `asset registry tests passed`.

If `DuplicateArbitration` fails in exactly one of its two orders, that is the real bug the test exists to catch — the `file`-name tiebreak in `RegisterFileLocked` is not doing its job. Fix the registry, not the test.

- [ ] **Step 5: Run the whole suite and the format gate**

```bash
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
```

Expected: all tests pass, format clean.

- [ ] **Step 6: Commit**

```bash
git add tests/asset_registry_tests.cpp tests/CMakeLists.txt engine/asset/asset_registry.h engine/asset/asset_registry.cpp
git commit -F - <<'EOF'
test(asset): cover the uuid registry's arbitration and healing rules

The registry decides which of two files claiming one uuid keeps it, deletes
sidecars it judges orphaned, and heals scene references through three
fallback tiers. None of that was tested.

Duplicate arbitration is asserted under both scan orders, since scan order
deciding the winner is exactly what the sidecar file-name tiebreak exists to
prevent.

IsUnderAssetsRoot exposes the existing internal root test; the auto-import
guard needs it next.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 2: Unpack embedded textures at import

After this task the import writes `textures/` into the bundle. Load still goes through the old cache path, so behaviour is unchanged and the tree stays working.

**Files:**
- Create: `tests/model_import_texture_tests.cpp`
- Modify: `engine/asset/gltf_model_loader.h`, `engine/asset/gltf_model_loader.cpp`, `engine/asset/model_loader.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `void me::GltfModelLoader::UnpackEmbeddedTextures(const std::filesystem::path& modelPath)` — writes every embedded image in `modelPath` to `modelPath.parent_path()/"textures"/`, skipping destinations that already exist. Task 3 depends on the file names it produces.

- [ ] **Step 1: Write the failing test**

Create `tests/model_import_texture_tests.cpp`:

```cpp
#include <engine/asset/gltf_model_loader.h>
#include <engine/asset/model_loader.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

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

// A 1x1 red PNG and a single-triangle buffer, both as base64 data URIs, so the
// fixture is a string in the test rather than a checked-in binary.
constexpr const char* kEmbeddedPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC";
constexpr const char* kTriangleBufferBase64 =
    "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIAAAA=";

// Positions at bytes 0..35, UVs at 36..59, indices at 60..65; buffer is 68
// bytes so the index view lands on a 4-byte boundary.
std::string BuildFixtureGltf()
{
    return std::string(R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2, "material": 0 } ] } ],
  "materials": [ { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ],
  "textures": [ { "source": 0 } ],
  "images": [ { "name": "", "mimeType": "image/png", "uri": "data:image/png;base64,)") +
           kEmbeddedPngBase64 + R"(" } ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 24 },
    { "buffer": 0, "byteOffset": 60, "byteLength": 6 }
  ],
  "buffers": [ { "byteLength": 68, "uri": "data:application/octet-stream;base64,)" +
           kTriangleBufferBase64 + R"(" } ]
})";
}

class ScopedDir
{
  public:
    explicit ScopedDir(const char* name)
    {
        std::error_code ec;
        const std::filesystem::path base =
            std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        m_path = (ec ? std::filesystem::temp_directory_path() : base) /
                 "miniengine_model_import_texture_tests" / name;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);
    }

    ~ScopedDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    ScopedDir(const ScopedDir&) = delete;
    ScopedDir& operator=(const ScopedDir&) = delete;

    const std::filesystem::path& Path() const
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

size_t CountFilesIn(const std::filesystem::path& directory)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec))
    {
        return 0;
    }
    size_t count = 0;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && !fileEc)
        {
            ++count;
        }
    }
    return count;
}

void ImportUnpacksEmbeddedTextures()
{
    ScopedDir scope("unpack");
    const std::filesystem::path source = scope.Path() / "source" / "fixture.gltf";
    std::error_code ec;
    std::filesystem::create_directories(source.parent_path(), ec);
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        out << BuildFixtureGltf();
    }

    const std::filesystem::path bundle = scope.Path() / "bundle";
    std::filesystem::create_directories(bundle, ec);
    const std::filesystem::path imported = ModelLoader::CopyModelWithSortedReferences(source, bundle);

    Require(std::filesystem::exists(imported, ec) && !ec, "import did not produce a model file");

    const std::filesystem::path textures = imported.parent_path() / "textures";
    Require(CountFilesIn(textures) == 1, "import did not unpack exactly one embedded texture");
}

void UnpackIsIdempotent()
{
    ScopedDir scope("idempotent");
    const std::filesystem::path model = scope.Path() / "fixture.gltf";
    {
        std::ofstream out(model, std::ios::binary | std::ios::trunc);
        out << BuildFixtureGltf();
    }

    GltfModelLoader::UnpackEmbeddedTextures(model);
    const std::filesystem::path textures = scope.Path() / "textures";
    Require(CountFilesIn(textures) == 1, "first unpack did not produce one texture");

    std::error_code ec;
    const std::filesystem::path unpacked = *std::filesystem::directory_iterator(textures, ec);
    const auto firstWrite = std::filesystem::last_write_time(unpacked, ec);

    GltfModelLoader::UnpackEmbeddedTextures(model);
    Require(CountFilesIn(textures) == 1, "second unpack duplicated the texture");
    Require(
        std::filesystem::last_write_time(unpacked, ec) == firstWrite,
        "second unpack rewrote an existing texture instead of skipping it");
}
}

int main()
{
    try
    {
        ImportUnpacksEmbeddedTextures();
        UnpackIsIdempotent();

        std::cout << "model import texture tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "model import texture tests failed: " << error.what() << '\n';
        return 1;
    }
}
```

Add to the end of `tests/CMakeLists.txt`:

```cmake
# Unlike the registry test, this one drives the glTF loader, whose translation unit pulls in
# tinygltf, stb and nlohmann::json. Listing those by hand would be brittle, so this target links
# engine_asset and accepts the transitive imgui dependency that asset_manager.cpp still imposes
# on that library.
add_executable(miniengine_model_import_texture_tests
    model_import_texture_tests.cpp
)
miniengine_group_target_sources(miniengine_model_import_texture_tests)

target_link_libraries(miniengine_model_import_texture_tests
    PRIVATE
        engine_asset
)

if(WIN32)
    add_custom_command(TARGET miniengine_model_import_texture_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:miniengine_model_import_texture_tests>
            $<TARGET_FILE_DIR:miniengine_model_import_texture_tests>
        COMMAND_EXPAND_LISTS
    )
endif()

add_test(
    NAME miniengine.model_import_texture
    COMMAND miniengine_model_import_texture_tests
)

set_target_properties(miniengine_model_import_texture_tests PROPERTIES FOLDER "Tests")
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug --parallel --target miniengine_model_import_texture_tests
```

Expected: compile error — `UnpackEmbeddedTextures` is not a member of `GltfModelLoader`.

- [ ] **Step 3: Declare `UnpackEmbeddedTextures`**

In `engine/asset/gltf_model_loader.h`, inside `class GltfModelLoader`, after `CopyWithSortedReferences`:

```cpp
    // Writes every image whose pixels live inside the model file (.glb
    // payloads and data: URIs) into "<model directory>/textures/", naming each
    // file the way ResolveImagePath derives it at load time. External URIs are
    // left alone. A destination that already exists is kept, never rewritten.
    // Does nothing for a model with no embedded images. Throws only if the
    // model itself cannot be parsed.
    static void UnpackEmbeddedTextures(const std::filesystem::path& modelPath);
```

- [ ] **Step 4: Implement the shared embedded-image predicate and the unpacker**

In `engine/asset/gltf_model_loader.cpp`, inside the anonymous namespace, directly above `ResolveExternalImagePath`:

```cpp
// True when the image's pixels live inside the model file rather than in a
// companion file: a .glb bufferView, or a data: URI. A remote URI ("://") is
// neither embedded nor a local companion — the engine cannot fetch it, so it
// resolves to no texture at all.
bool IsEmbeddedImage(const tinygltf::Image& image)
{
    return image.uri.empty() || image.uri.starts_with("data:");
}

// The subdirectory embedded images are unpacked into, relative to the model.
constexpr const char* kUnpackedTextureDirectory = "textures";
```

Then replace the whole `ExportEmbeddedImage` function with:

```cpp
// Writes one decoded image to disk as PNG. Returns false when the image
// carries nothing writable, which is not an error: the caller reports the
// resulting texture as missing.
bool WriteUnpackedImage(const tinygltf::Image& image, const std::filesystem::path& outputPath)
{
    if (image.image.empty() || image.width <= 0 || image.height <= 0 ||
        image.component <= 0 || image.component > 4)
    {
        return false;
    }
    if (image.bits > 8)
    {
        LOG_WARN(
            "Skipping embedded image '{}' because {}-bit textures are not yet supported.",
            outputPath.string(),
            image.bits);
        return false;
    }

    std::error_code existsEc;
    if (std::filesystem::exists(outputPath, existsEc) && !existsEc)
    {
        return true; // already unpacked; never overwrite
    }

    const int writeResult = stbi_write_png(
        outputPath.string().c_str(),
        image.width,
        image.height,
        image.component,
        image.image.data(),
        image.width * image.component);
    if (writeResult == 0)
    {
        throw std::runtime_error("Failed to unpack embedded glTF texture: " + outputPath.string());
    }
    return true;
}
```

Delete `BuildCacheKey` and `BuildEmbeddedTextureCacheDirectory` entirely. Keep `BuildEmbeddedTextureFileName` — it is now the shared naming rule between import and load.

Add the public definition at the end of the file, next to `CopyWithSortedReferences`:

```cpp
void GltfModelLoader::UnpackEmbeddedTextures(const std::filesystem::path& modelPath)
{
    const std::string extension = ToLowerCopy(modelPath.extension().string());

    tinygltf::TinyGLTF loader;
    loader.SetPreserveImageChannels(true);

    tinygltf::Model model;
    std::string warnings;
    std::string errors;
    bool loaded = false;

    if (extension == ".glb")
    {
        loaded = loader.LoadBinaryFromFile(&model, &errors, &warnings, modelPath.string());
    }
    else if (extension == ".gltf")
    {
        loaded = loader.LoadASCIIFromFile(&model, &errors, &warnings, modelPath.string());
    }
    else
    {
        return; // nothing else carries embedded glTF images
    }

    if (!loaded)
    {
        throw std::runtime_error(
            "Failed to parse '" + modelPath.string() + "' while unpacking embedded textures" +
            (errors.empty() ? std::string{} : ": " + errors));
    }

    const std::filesystem::path textureDirectory =
        modelPath.parent_path() / kUnpackedTextureDirectory;

    size_t unpacked = 0;
    for (size_t imageIndex = 0; imageIndex < model.images.size(); ++imageIndex)
    {
        const tinygltf::Image& image = model.images[imageIndex];
        if (!IsEmbeddedImage(image))
        {
            continue;
        }

        std::error_code mkdirEc;
        std::filesystem::create_directories(textureDirectory, mkdirEc);
        if (mkdirEc)
        {
            throw std::runtime_error(
                "Failed to create '" + textureDirectory.string() + "': " + mkdirEc.message());
        }

        const std::filesystem::path outputPath =
            textureDirectory / BuildEmbeddedTextureFileName(image, imageIndex);
        if (WriteUnpackedImage(image, outputPath))
        {
            ++unpacked;
        }
    }

    if (unpacked > 0)
    {
        LOG_INFO("Unpacked {} embedded texture(s) for '{}'", unpacked, modelPath.string());
    }
}
```

`ToLowerCopy` already exists in this translation unit at
`gltf_model_loader.cpp:53` and `LoadModel` already uses it the same way — do
not add a second one.

- [ ] **Step 5: Call it from the import path**

In `engine/asset/model_loader.cpp`, rewrite `CopyModelWithSortedReferences` so both branches converge on one unpack:

```cpp
std::filesystem::path ModelLoader::CopyModelWithSortedReferences(
    const std::filesystem::path& modelPath,
    const std::filesystem::path& targetDirectory)
{
    const std::string extension = ToLowerCopy(modelPath.extension().string());

    std::filesystem::path dst;
    if (extension == ".gltf")
    {
        dst = GltfModelLoader::CopyWithSortedReferences(modelPath, targetDirectory);
    }
    else
    {
        // .glb (and anything else) is self-contained: plain copy into the folder.
        dst = targetDirectory / modelPath.filename();
        std::error_code ec;
        std::filesystem::copy_file(modelPath, dst, std::filesystem::copy_options::skip_existing, ec);
        if (ec)
        {
            throw std::runtime_error(
                "Failed to copy '" + modelPath.string() + "' to '" + dst.string() + "': " + ec.message());
        }
    }

    // Images whose pixels live inside the model file become real files in the
    // bundle, so every texture path the loader produces is model-relative and
    // loading never has to write anything.
    GltfModelLoader::UnpackEmbeddedTextures(dst);
    return dst;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build --preset vs2026-x64-debug --parallel --target miniengine_model_import_texture_tests
ctest --test-dir out/build/vs2026-x64 -C Debug -R miniengine.model_import_texture --output-on-failure
```

Expected: PASS, printing `model import texture tests passed`.

- [ ] **Step 7: Run the whole suite and the format gate**

```bash
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
```

- [ ] **Step 8: Commit**

```bash
git add engine/asset/gltf_model_loader.h engine/asset/gltf_model_loader.cpp engine/asset/model_loader.cpp tests/model_import_texture_tests.cpp tests/CMakeLists.txt
git commit -F - <<'EOF'
feat(asset): unpack embedded glTF textures during import

Images whose pixels live inside the model file now become real files under
<bundle>/textures/ when the model is imported, named the way the loader will
derive them. Both .glb and .gltf go through it, so a data: URI is unpacked
the same way as a .glb payload.

The path-keyed cache directory and its FNV1a key are gone. Loading still
reads through the old branch; the next commit switches it over.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 3: Make loading read-only and model-relative

**Files:**
- Modify: `engine/asset/gltf_model_loader.cpp`, `tests/model_import_texture_tests.cpp`

**Interfaces:**
- Consumes: `GltfModelLoader::UnpackEmbeddedTextures` and `BuildEmbeddedTextureFileName` from Task 2.
- Produces: the path contract — every `ModelMaterialData` texture path is model-relative.

- [ ] **Step 1: Write the failing test**

Add to `tests/model_import_texture_tests.cpp`, inside the anonymous namespace:

```cpp
void LoadReportsModelRelativeTexturePath()
{
    ScopedDir scope("relative");
    const std::filesystem::path source = scope.Path() / "source" / "fixture.gltf";
    std::error_code ec;
    std::filesystem::create_directories(source.parent_path(), ec);
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        out << BuildFixtureGltf();
    }

    const std::filesystem::path bundle = scope.Path() / "bundle";
    std::filesystem::create_directories(bundle, ec);
    const std::filesystem::path imported = ModelLoader::CopyModelWithSortedReferences(source, bundle);

    const LoadedModelData data = ModelLoader::LoadModel(imported.string());
    Require(!data.materials.empty(), "loaded model carried no material");

    const std::string& texturePath = data.materials[0].baseColorTexturePath;
    Require(!texturePath.empty(), "embedded base color texture resolved to nothing");
    Require(
        !std::filesystem::path(texturePath).is_absolute(),
        "embedded texture path is absolute; it must be relative to the model");
    Require(
        std::filesystem::exists(imported.parent_path() / texturePath, ec) && !ec,
        "texture path did not resolve against the model directory");
}

// The property Task 3 establishes: parsing a model writes nothing.
void LoadWritesNothing()
{
    ScopedDir scope("readonly");
    const std::filesystem::path source = scope.Path() / "source" / "fixture.gltf";
    std::error_code ec;
    std::filesystem::create_directories(source.parent_path(), ec);
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        out << BuildFixtureGltf();
    }

    const std::filesystem::path bundle = scope.Path() / "bundle";
    std::filesystem::create_directories(bundle, ec);
    const std::filesystem::path imported = ModelLoader::CopyModelWithSortedReferences(source, bundle);

    size_t before = 0;
    for (std::filesystem::recursive_directory_iterator it(bundle, ec), end; !ec && it != end; it.increment(ec))
    {
        ++before;
    }

    ModelLoader::LoadModel(imported.string());

    size_t after = 0;
    for (std::filesystem::recursive_directory_iterator it(bundle, ec), end; !ec && it != end; it.increment(ec))
    {
        ++after;
    }

    Require(before == after, "loading a model created or removed files in the bundle");
}
```

Call both from `main()`, after `UnpackIsIdempotent()`:

```cpp
        LoadReportsModelRelativeTexturePath();
        LoadWritesNothing();
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset vs2026-x64-debug --parallel --target miniengine_model_import_texture_tests
ctest --test-dir out/build/vs2026-x64 -C Debug -R miniengine.model_import_texture --output-on-failure
```

Expected: FAIL with `embedded texture path is absolute; it must be relative to the model` — `ResolveImagePath` still returns the cache path.

- [ ] **Step 3: Rewrite `ResolveImagePath`'s embedded branch**

In `engine/asset/gltf_model_loader.cpp`, replace the body of `ResolveImagePath` after the existing index checks:

```cpp
    EnsureIndexInRange(static_cast<size_t>(texture.source), model.images.size(), "image");
    const tinygltf::Image& image = model.images[static_cast<size_t>(texture.source)];

    if (const std::string externalPath = ResolveExternalImagePath(modelPath, image); !externalPath.empty())
    {
        return externalPath;
    }

    if (!IsEmbeddedImage(image))
    {
        // A remote URI the engine cannot fetch. Nothing to point at.
        LOG_WARN("Ignoring remote texture URI '{}' in '{}'", image.uri, modelPath.string());
        return {};
    }

    // Embedded: import unpacked this image into the bundle. Derive the same
    // name import wrote and confirm it is there, so a bundle imported before
    // unpacking existed degrades to an untextured material with one warning
    // rather than to a path that resolves to nothing.
    const std::string relativePath =
        (std::filesystem::path(kUnpackedTextureDirectory) /
         BuildEmbeddedTextureFileName(image, static_cast<size_t>(texture.source)))
            .generic_string();

    std::error_code ec;
    if (std::filesystem::exists(modelPath.parent_path() / relativePath, ec) && !ec)
    {
        return relativePath;
    }

    LOG_WARN(
        "'{}' has an embedded texture that was never unpacked (expected '{}'); re-import the model",
        modelPath.string(),
        relativePath);
    return {};
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build --preset vs2026-x64-debug --parallel --target miniengine_model_import_texture_tests
ctest --test-dir out/build/vs2026-x64 -C Debug -R miniengine.model_import_texture --output-on-failure
```

Expected: PASS.

- [ ] **Step 5: Confirm no writer remains**

```bash
grep -rn "CacheRoot\|BuildCacheKey\|BuildEmbeddedTextureCacheDirectory\|ExportEmbeddedImage" engine/
```

Expected: exactly one hit — `EnginePaths::CacheRoot()` in `engine/core/paths/engine_paths.cpp` and its declaration, plus the startup log line in `engine/application/editor_application.cpp`. No hit inside `engine/asset/`.

- [ ] **Step 6: Run the whole suite, the format gate, and a smoke run**

```bash
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60
```

Expected: tests pass, format clean, the smoke run exits 0. The smoke run only proves the startup path; GUI and visual confirmation stay manual.

- [ ] **Step 7: Commit**

```bash
git add engine/asset/gltf_model_loader.cpp tests/model_import_texture_tests.cpp
git commit -F - <<'EOF'
fix(asset): resolve embedded textures to model-relative paths

ResolveImagePath derived an absolute path into a path-keyed cache directory
while external URIs stayed model-relative, so one field carried two path
conventions and a material sidecar under assets/ persisted a machine-local
path into gitignored derived data.

It now derives the name import wrote under <bundle>/textures/ and confirms
the file is there. A bundle imported before unpacking existed degrades to an
untextured material plus one actionable warning.

LoadModel writes nothing to the filesystem now, asserted by a test that
counts bundle entries across a load.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 4: Import models referenced from outside the assets root

**Files:**
- Modify: `engine/editor/editor_backend_base.cpp`

**Interfaces:**
- Consumes: `AssetRegistry::IsUnderAssetsRoot` from Task 1; `ModelImportService::ImportModelIntoAssetDirectory` (existing).
- Produces: the invariant — every model that reaches `EntityEditService` lives under the assets root.

- [ ] **Step 1: Add the guard**

In `engine/editor/editor_backend_base.cpp`, inside the `pendingModelLoads` block, replace the body of the `try` with:

```cpp
        try
        {
            std::string path = request.path;

            // Unpacking embedded textures happens at import, so a model that
            // was never imported has none. Import it first; the invariant is
            // that anything reaching the loader lives under the assets root.
            if (!AssetRegistry::IsUnderAssetsRoot(path))
            {
                LOG_INFO("Model '{}' is outside the assets root; importing it first", path);
                path = ModelImportService::ImportModelIntoAssetDirectory(
                    path, (EnginePaths::AssetsRoot() / "models").string());
            }

            LOG_INFO("Loading model: {}", path);
            if (!request.placeAsNewEntity && EditorWorld().HasSelection())
            {
                EntityEditService::LoadSelectedModel(State(), path);
            }
            else
            {
                EntityEditService::PlaceModelIntoScene(State(), path, glm::vec3(0.0f));
            }
            renderablesDirty = true;
        }
```

No include changes are needed: `engine/asset/asset_registry.h` (line 8), `engine/core/paths/engine_paths.h` (line 10) and `services/model_import_service.h` (line 4) are already included by this file.

- [ ] **Step 2: Build**

```bash
cmake --build --preset vs2026-x64-debug --parallel
```

Expected: clean build.

- [ ] **Step 3: Verify against a model outside the assets root**

```powershell
Copy-Item -Recurse .\assets\NewSponza_Main_glTF_003 $env:TEMP\me_outside_model
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60 --model "$env:TEMP\me_outside_model\NewSponza_Main_glTF_003.gltf"
```

Expected: the log shows `is outside the assets root; importing it first`, then `Imported model ... -> ...assets\models\...`, then a normal load. Process exits 0. Afterwards `assets/models/NewSponza_Main_glTF_003/` exists.

Clean up the copy and the import afterwards:

```powershell
Remove-Item -Recurse -Force $env:TEMP\me_outside_model, .\assets\models\NewSponza_Main_glTF_003
```

- [ ] **Step 4: Confirm the in-tree path still works**

```powershell
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60 --model .\assets\NewSponza_Main_glTF_003\NewSponza_Main_glTF_003.gltf
```

Expected: no import line in the log; the model loads directly. Process exits 0.

- [ ] **Step 5: Run the whole suite and the format gate**

```bash
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
```

- [ ] **Step 6: Commit**

```bash
git add engine/editor/editor_backend_base.cpp
git commit -F - <<'EOF'
feat(editor): import models referenced from outside the assets root

Embedded textures are unpacked at import, so a model loaded straight off disk
via --model would have none. The guard sits at the pendingModelLoads drain,
the one point every load request funnels through, so it covers --model, UI
loads and batch loads alike.

Establishes the invariant the path contract rests on: a model that can be
rendered lives under the assets root.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

### Task 5: Delete the dead cache and update the documentation

**Files:**
- Delete: `.cache/tinygltf/` (untracked, gitignored)
- Modify: `README.md`, `docs/superpowers/specs/2026-09-12-embedded-texture-unpacking-design.md`

- [ ] **Step 1: Confirm nothing writes the directory any more**

```bash
grep -rn "tinygltf\"" engine/ ; grep -rn "CacheRoot" engine/
```

Expected: no hit joining `CacheRoot()` with `"tinygltf"`. `CacheRoot` itself survives in `engine_paths` and the startup log.

- [ ] **Step 2: Delete it**

```bash
du -sh .cache/tinygltf
rm -rf .cache/tinygltf
```

Expected: roughly 505 MB reclaimed. The directory is gitignored derived data with no remaining writer, so nothing tracked changes.

- [ ] **Step 3: Correct the spec's note about the test target**

In `docs/superpowers/specs/2026-09-12-embedded-texture-unpacking-design.md`, replace the paragraph beginning `engine_asset currently links imgui::imgui PUBLIC` with:

```markdown
`engine_asset` links `imgui::imgui` PUBLIC because `asset_manager.cpp` is an
ImGui panel living in the asset layer. `asset_registry.cpp` depends only on
`engine_core` and yaml-cpp, so the registry test compiles that one translation
unit directly and links neither — the same technique
`miniengine_scene_pass_tests` already uses. The import test does link
`engine_asset`, because listing the glTF loader's tinygltf, stb and
nlohmann::json dependencies by hand would be brittle. Moving `AssetManager`
into `engine/editor/ui/` is separate work.
```

- [ ] **Step 4: Update the README**

In section 5, under the `**模型导入**` bullet, append:

```markdown
导入还会把模型文件内部的图片（`.glb` 负载、`data:` URI）解包成 `<bundle>/textures/` 下的真实文件；加载只读取，不写盘。因此贴图路径一律相对于模型文件所在目录——外部 URI 和嵌入图片没有例外。`--model` 指向 `assets/` 之外时会先自动导入，再加载导入后的副本。
```

In section 7, replace the bullet `- 启动默认场景配置与部分引用刷新仍以路径为主，未覆盖所有 UUID 解析路径。` with:

```markdown
- 启动默认场景配置与部分引用刷新仍以路径为主，未覆盖所有 UUID 解析路径；材质内的贴图引用是纯路径，没有 UUID 参与，重命名贴图会静默打断引用。
```

In section 10, add a dated entry above the 2026-09-12 Vulkan entry:

```markdown
### 2026-09-12 — 嵌入式贴图改为导入时解包

嵌入图片此前在**加载时**导出到 `.cache/tinygltf/<stem>_<模型绝对路径的 FNV1a>/`。缓存键是路径，所以每次导入、复制或重命名都新建一份全尺寸副本且从不清理——工作区里曾经是同一个模型的五份 101 MB 目录，共 505 MB。导出返回的还是绝对路径，而外部 URI 返回的是模型相对路径：同一个 `ModelMaterialData` 字段承载两种语义，`.material.yaml`（在 `assets/` 下，可提交）于是持久化了指向 gitignored 派生数据的本机路径。

- 解包移到 `ModelLoader::CopyModelWithSortedReferences` 末尾，`.glb` 与 `.gltf` 两条分支合流后统一调 `GltfModelLoader::UnpackEmbeddedTextures`，写进 `<bundle>/textures/`。命名由 `BuildEmbeddedTextureFileName` 一处决定，导入和加载从同一个输入推出同一个名字。
- `ResolveImagePath` 改为推导相对路径并确认存在，`LoadModel` 因此不再写任何文件。解包之前导入的旧包会退化成无贴图材质加一条明确的 warning，而不是指向不存在的路径。
- `.glb` 仍原样保留，是源文件的忠实副本，贴图在磁盘上有两份（包内嵌一份、解包一份）。这是一次性有界成本，不随路径变化增长；把 `.glb` 转写成 `.gltf` 能省掉那一份，但导入产物就不再是副本，且要冒 tinygltf 写回丢 extension、漂精度的风险。
- 解包只在导入时发生，所以 `--model` 指向 `assets/` 外的模型会先自动导入。守卫放在 `editor_backend_base.cpp` 的 `pendingModelLoads` 出口——所有加载请求的唯一收口，一处覆盖 `--model`、UI 加载和批量加载。由此确立不变式：能渲染的模型一定在 `assets/` 下。
- `AssetRegistry` 补上首批测试：UUID 铸造与持久化、边界拒绝、重复 UUID 仲裁（**两种扫描顺序都断言**，因为扫描顺序决定归属正是 sidecar 文件名仲裁要防的事）、孤儿 sidecar 清理、引用解析三级回退、重命名与删除。

`EnginePaths::CacheRoot()` 保留（启动日志仍在报告它），只是 `tinygltf/` 子目录和那套键推导没有了。
```

- [ ] **Step 5: Verify the documentation claims against the code**

```bash
grep -n "UnpackEmbeddedTextures" engine/asset/model_loader.cpp engine/asset/gltf_model_loader.h engine/asset/gltf_model_loader.cpp
grep -n "IsUnderAssetsRoot" engine/editor/editor_backend_base.cpp engine/asset/asset_registry.h
```

Expected: every symbol the README and the record name exists where they say it does.

- [ ] **Step 6: Final full verification**

```bash
ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure
```

```powershell
.\scripts\check-format.ps1
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60
```

Expected: all tests pass, format clean, smoke run exits 0.

- [ ] **Step 7: Commit**

```bash
git add README.md docs/superpowers/specs/2026-09-12-embedded-texture-unpacking-design.md
git commit -F - <<'EOF'
docs(asset): record the embedded texture unpacking change

README section 5 gains the path contract and the auto-import rule, section 7
names the material texture reference gap that remains, and section 10 records
why the cache directory existed and what replaced it.

The spec's note about the registry test linking imgui is corrected: that test
compiles asset_registry.cpp directly and links neither engine_asset nor imgui.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
```

---

## Verification Summary

| Gate | Command | Covers |
| --- | --- | --- |
| Registry rules | `ctest -R miniengine.asset_registry` | Task 1 |
| Import + load contract | `ctest -R miniengine.model_import_texture` | Tasks 2, 3 |
| Whole suite | `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure` | every task |
| Formatting | `.\scripts\check-format.ps1` | every task |
| Startup path | `miniengine_app.exe --backend vulkan --frames 60` | Tasks 3, 4 |
| Outside-root import | `--frames 60 --model <path outside assets>` | Task 4 |

A 60-frame exit and a green CTest prove compilation and automated regression only. GUI behaviour and visual results still need manual confirmation, and this plan claims neither.
