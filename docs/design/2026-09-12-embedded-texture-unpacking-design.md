# Embedded Texture Unpacking and Asset Registry Coverage Design

## Goal

Make every texture path a model file produces mean exactly one thing — a path
relative to the model's own directory — and make model loading a read-only
operation. Embedded images (`.glb` payloads, `data:` URIs) become real files
inside the model bundle at import time instead of being exported to a
path-keyed cache at load time.

Three defects close together because they share one cause:

1. `.cache/tinygltf/` grows without bound. The cache directory name is keyed
   on the model's absolute path, so every import, copy or rename mints a new
   full-size copy and nothing ever removes the old ones. The working tree
   currently holds 505 MB: five 101 MB directories for one model.
2. `ModelMaterialData`'s texture path fields carry two incompatible path
   conventions. External URIs are model-relative; embedded exports are
   absolute paths into `.cache/`. The absolute form is persisted into
   `.material.yaml` sidecars under `assets/` — which are committed — while
   `.cache/` is gitignored, so those sidecars break on any other machine.
3. `AssetRegistry` has no test coverage, and it holds the densest rules in the
   module: duplicate uuid arbitration, orphan sidecar deletion, and a
   three-tier reference fallback.

## Current State

Facts this design is built on, verified against the tree at `021e043`:

- `ResolveExternalImagePath` returns the URI decoded and normalized, with no
  directory prefix — a model-relative path
  (`engine/asset/gltf_model_loader.cpp:554`). Its comment states the contract:
  callers resolve against the model's current location.
- `ExportEmbeddedImage` writes a PNG into
  `CacheRoot()/tinygltf/<stem>_<FNV1a(absolute model path)>/` and returns
  `outputPath.string()` — an absolute path
  (`engine/asset/gltf_model_loader.cpp:589`, `:608`).
- `ResolveImagePath` returns whichever of the two applies, into the same
  `ModelMaterialData` field (`engine/asset/gltf_model_loader.cpp:611`).
- `SerializeMaterialDefinition` writes `baseColorTexturePath` and its five
  siblings verbatim (`engine/asset/material_definition.cpp:109`).
- `BuildCpuRenderSubmeshes` resolves a texture path against
  `std::filesystem::path(model.sourcePath).parent_path()`
  (`engine/editor/services/scene_renderables.cpp:101`). An absolute path
  passes through `operator/` unchanged, which is why the current mixture works
  on the machine that produced it.
- `.glb` import is a plain `copy_file` with `skip_existing`
  (`engine/asset/model_loader.cpp:97`). Nothing unpacks it.
- `GltfModelLoader::CopyWithSortedReferences` skips `data:` URIs when
  rewriting companion references (`engine/asset/gltf_model_loader.cpp:1270`),
  so `.gltf` files keep their embedded images embedded after import.
- `--model <path>` bypasses import entirely: it is parsed in
  `editor_application.cpp:76` and pushed onto `pendingModelLoads` in
  `editor_backend_base.cpp:623`. The path may point anywhere on disk.
- `pendingModelLoads` is drained in one place,
  `editor_backend_base.cpp:101`, which every model load request passes
  through.
- `CacheRoot()` has exactly two consumers: the startup log line
  (`editor_application.cpp:150`) and `BuildEmbeddedTextureCacheDirectory`.
- `assets/` currently contains **no** `.material.yaml` files. The only bundle
  is `NewSponza_Main_glTF_003`, which references external textures. There is
  no stored sidecar carrying an absolute `.cache` path, so no migration path
  is needed.
- `AssetRegistry::IsUnderRootLocked` (`engine/asset/asset_registry.cpp:85`)
  already implements the "is this under the assets root" test, but it is
  private to the translation unit.
- Tests use a bare `main()`, a local `Require(bool, const char*)` helper,
  `try`/`catch` around the body, and an exit code. No test framework is
  linked.

## Locked Decisions

1. **Unpack at import only.** `ModelLoader::LoadModel` becomes read-only: it
   writes no file under any circumstance. Unpacking is a step in
   `CopyModelWithSortedReferences`.
2. **Every texture path is model-relative.** No exceptions, no second
   convention, no absolute paths in `ModelMaterialData` or in
   `.material.yaml`.
3. **Models outside `assets/` are imported automatically.** The invariant
   "a model that can be rendered lives under `assets/`" holds without
   exception. `--model` pointing outside the assets root runs the existing
   import first and loads the imported copy.
4. **`.glb` stays a faithful copy of its source.** Import writes the `.glb`
   unchanged and adds `textures/` alongside it. The textures exist twice on
   disk (once inside the `.glb`, once unpacked), which is a bounded one-time
   cost that does not grow with path changes. Rewriting `.glb` into
   `.gltf` + `buffers/` + `textures/` would halve the disk cost but makes the
   import product a conversion rather than a copy, and risks losing extension
   data or drifting precision through a tinygltf write round-trip.
5. **`.cache/tinygltf/` is deleted, not migrated.** It is gitignored derived
   data with no remaining writer after this change.
6. **`EnginePaths::CacheRoot()` stays.** The startup log still reports it and
   a future shader cache is a plausible consumer. Only the `tinygltf/`
   subdirectory and its key derivation go away.

## Design

### Path contract

One rule, stated once, enforced on both sides:

> Every path in `ModelMaterialData`'s texture fields — and therefore every
> texture path in a `.material.yaml` — is relative to the directory holding
> the model file.

`BuildCpuRenderSubmeshes`'s existing `modelDir / texturePath` becomes the
complete resolution rule rather than a rule with one silent exception.

### Import side

`ModelLoader::CopyModelWithSortedReferences` gains a step after the copy:

```
copy model (+ companions for .gltf)   [unchanged]
UnpackEmbeddedTextures(copiedModelPath)   [new]
```

`UnpackEmbeddedTextures` loads the copied model with tinygltf, walks
`model.images`, and for each image carrying pixel data writes
`<bundle>/textures/<BuildEmbeddedTextureFileName(image, index)>`.

- The file name comes from the existing `BuildEmbeddedTextureFileName`, so
  import and load derive the same name from the same input. No second naming
  rule enters the system.
- Writing skips a destination that already exists, matching both the current
  `ExportEmbeddedImage` behavior and the `skip_existing` convention the import
  path uses everywhere else. This also means an external URI already sorted
  into `textures/` is never clobbered by a same-named embedded export.
- `image.bits > 8` keeps its current behavior: warn and skip, producing no
  file. The load side then reports the texture as missing, which is the
  honest outcome for a format the engine cannot store.
- Both `.glb` and `.gltf` go through it. A `.gltf` whose images are `data:`
  URIs is unpacked the same way as a `.glb`.

`BuildCacheKey`, `BuildEmbeddedTextureCacheDirectory` and the write half of
`ExportEmbeddedImage` are deleted.

Cost: importing a `.glb` now parses it once with tinygltf, on top of the
existing copy. Import already runs on a background thread via
`StartAsyncImport`, so this lands off the UI thread.

### Load side

`ResolveImagePath`'s embedded branch stops writing and starts deriving:

```cpp
const std::string relative =
    (std::filesystem::path("textures") /
     BuildEmbeddedTextureFileName(image, sourceIndex)).generic_string();

std::error_code ec;
if (std::filesystem::exists(modelPath.parent_path() / relative, ec) && !ec)
{
    return relative;
}

LOG_WARN(...);  // bundle was never unpacked; re-import to fix
return {};
```

A bundle imported before this change degrades to an untextured material plus
one actionable warning, rather than crashing or silently rendering wrong.

`ExportEmbeddedImage` is removed; `stb_image_write` stays included in the
translation unit only if `UnpackEmbeddedTextures` needs it (it does).

### `--model` auto-import

The check goes at the `pendingModelLoads` drain
(`editor_backend_base.cpp:101`), not at argument parsing — that is the single
point every load request funnels through, so one guard covers `--model`, UI
loads and batch loads alike.

```
request = pendingModelLoads.front()
if (!AssetRegistry::IsUnderAssetsRoot(request.path))
    request.path = ImportModelIntoAssetDirectory(request.path, AssetsRoot() / "models")
... existing load flow
```

Import runs synchronously here. It is a startup-path, once-per-invocation
operation, and the alternative — re-queueing behind an async import — adds a
state machine for no gain at this scale.

`asset_registry.h` gains:

```cpp
bool IsUnderAssetsRoot(const std::filesystem::path& path);
```

backed by the existing `IsUnderRootLocked`, taking the registry mutex and
initializing lazily like every other entry point.

### Testing

New target `miniengine_asset_registry_tests`, linking `engine_asset`,
following the existing bare-`main()` style. Each case builds an isolated asset
tree under `std::filesystem::temp_directory_path()`, calls
`AssetRegistry::Initialize(root)` to point the registry at it, and removes the
tree afterwards.

| Case | Assertion |
| --- | --- |
| Mint and persist | A new registrable file gets a uuid and a sidecar; a second call returns the same uuid |
| Boundary rejection | Files outside the assets root and non-registrable extensions return `""` |
| Duplicate arbitration | Asset copied together with its sidecar: the copy gets a fresh uuid and the original keeps its own — asserted under **both** scan orders, which is exactly what the `file`-name tiebreak in `RegisterFileLocked` exists to guarantee |
| Orphan sidecars | A sidecar whose asset is gone is deleted by `RescanAssetTree` |
| `ResolveReference` tier 1 | A known uuid wins over a stale stored path and sets `healed` |
| `ResolveReference` tier 2 | An unknown uuid with a live stored path adopts and registers that path |
| `ResolveReference` tier 3 | A unique filename match heals; an ambiguous one does not |
| `ResolveReference` miss | `resolved == false`, and the stored path and uuid come back untouched |
| Rename | File rename keeps the uuid and moves the sidecar; directory rename keeps every uuid beneath it |
| Removal | Entry and sidecar both go; removing a directory prunes its whole subtree |

`engine_asset` links `imgui::imgui` PUBLIC because `asset_manager.cpp` is an
ImGui panel living in the asset layer. `asset_registry.cpp` depends only on
`engine_core` and yaml-cpp, so the registry test compiles that one translation
unit directly and links neither — the same technique
`miniengine_scene_pass_tests` already uses. The import test does link
`engine_asset`, because listing the glTF loader's tinygltf, stb and
nlohmann::json dependencies by hand would be brittle. Moving `AssetManager`
into `engine/editor/ui/` is separate work.

### Disk and documentation

- `.cache/tinygltf/` (505 MB) is deleted once no writer remains.
- `.gitignore` keeps `/.cache/`.
- README section 5 gains the unpacking rule under "模型导入"; section 10 gains
  a dated entry.

## Out of Scope

Recorded so no task reaches for them. All are real findings from the same
review, deliberately deferred:

- Moving `AssetManager` out of `engine/asset` and dropping the imgui
  dependency from that target.
- Per-frame `GetOrCreateUuid` in the asset browser preview panel.
- Orphan sidecar deletion keyed on an `exists()` that also returns false on
  error; non-atomic sidecar writes.
- Rename not checking references, and not invalidating `ModelCache`.
- `ModelCache` and the preview texture cache having no eviction.
- `ModelCache::Get` handing out a mutable shared handle.
- `STBI_THREAD_LOCAL` not being defined while decodes run concurrently.
- `BuildMaterialDefinitionPath` colliding for same-stem models.
- `ScanCurrentDir`'s ineffective `error_code` handling.
- Paste producing a broken single-file copy.
