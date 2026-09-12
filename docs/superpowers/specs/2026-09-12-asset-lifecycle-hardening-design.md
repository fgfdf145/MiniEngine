# Asset Lifecycle Hardening Design

## Goal

Close six defects in the asset module's lifecycle handling, found in the same
review that produced the embedded-texture work. They fall into three groups
that are independent of each other but each internally coupled:

- **Registry safety** — the asset browser does filesystem work every frame on
  the same mutex a background import holds for a whole-tree scan, and the
  registry deletes uuid sidecars on evidence that a transient IO error also
  produces.
- **Cache lifecycle** — `ModelCache` hands out a mutable handle to shared
  data, never evicts anything, and is not invalidated on rename.
- **Reference integrity** — deleting an asset warns about references to it;
  renaming the same asset silently breaks them.

## Current State

Verified against `feat/embedded-texture-unpacking` at `ee78e3b`:

- `AssetManager::DrawPreviewPanel` calls `AssetRegistry::GetOrCreateUuid`
  unconditionally every frame for the focused entry
  (`engine/asset/asset_manager.cpp:755`). That takes the global registry
  mutex, issues an `is_regular_file` syscall, and can mint a uuid and write a
  sidecar.
- `ImportModelIntoAssetDirectory` calls `AssetRegistry::RescanAssetTree` from
  a background thread (`engine/editor/services/model_import_service.cpp:84`),
  holding the same mutex across a recursive directory walk plus sidecar
  writes.
- `ScanLocked` treats a sidecar as orphaned when
  `std::filesystem::exists(assetPath, existsEc)` is false
  (`engine/asset/asset_registry.cpp:264`) and deletes it. `exists` also
  returns false when `existsEc` is set, so an IO error deletes the uuid.
- `WriteSidecar` opens with `std::ios::trunc` and streams
  (`engine/asset/asset_registry.cpp:127`). A crash mid-write leaves an empty
  sidecar, which reads back as no uuid.
- `ModelCache::Get` returns `std::shared_ptr<LoadedModelData>`.
  `model_import_service.cpp:258` and `:291` mutate the cached object through
  it. `scene_renderables.cpp:87` reads through it and refills the cache at
  `:97` when the entry is missing.
- `ModelCache` has no capacity bound and no eviction; only explicit
  `Invalidate` removes anything.
- `AssetManager::CommitRename` (`engine/asset/asset_manager.cpp:923`) moves
  the uuid sidecar and the `.material.yaml` sidecars but never calls
  `ModelCache::Invalidate`. `ModelImportService::DeleteAssetPath` does
  (`model_import_service.cpp:138`).
- `GetPreviewTextureCache` (`engine/editor/ui/editor_model_preview.cpp:278`)
  is a function-local static map of decoded full-resolution RGBA8 textures
  with no bound and no eviction.
- `AssetManager::BuildPendingDeleteWarnings`
  (`engine/asset/asset_manager.cpp:986`) reads every `.gltf`/`.yaml` under the
  assets root up to 64 MB each, fully into memory, and substring-searches for
  up to 256 doomed file names. It runs synchronously on the main thread when
  the user clicks Delete. `CommitRename` has no equivalent.
- `Vertex` is five float arrays totalling 15 floats
  (`engine/asset/mesh.h`), so `sizeof(Vertex)` is 60 with no padding.
- `RebuildSceneRenderables` already iterates every `ModelComponent`
  (`engine/editor/services/scene_renderables.cpp:227`);
  `RefreshDirtySceneRenderables` processes only dirty entities and reports
  whether anything changed (`:274`).
- `scene_renderables.cpp:88` falls back to a synchronous load when the cache
  misses, guarded against running while an async loader is active. Eviction is
  therefore safe: an evicted model is reloaded, not lost.

## Locked Decisions

1. **Eviction is reference-driven with a byte budget.** Models the scene still
   references are never evicted. Everything else is LRU, evicted only when the
   total exceeds the budget. A pure reference-driven policy would make
   deleting an entity and dragging it back re-parse the model — seconds for a
   Sponza-scale asset — which is a usability regression.
2. **`ModelCache::Get` returns a const handle.** Mutation goes through
   explicit named entry points. The compiler then prevents the accidental
   write the current signature invites.
3. **Rename gets the same reference check delete has, and the scan is fixed
   rather than duplicated.** Extracting the scan is what makes it affordable
   in a rename, which is an F2-frequency operation.
4. **Texture references in materials stay path-only.** Giving them uuids would
   heal renames through the registry and remove the need for a scan entirely,
   but it changes the `.material.yaml` format, `ModelMaterialData` and the
   resolution path — a larger change than the other five items combined. It
   stays a known gap, already recorded in README section 7.
5. **Access recency is a monotonic counter, not a clock.** Cheaper, and it
   makes eviction order deterministic in tests.

## Design

### Group A: registry safety

**Per-frame uuid lookup.** `AssetManager` caches the focused entry's uuid in
`m_previewUuid`, keyed by `m_previewUuidIndex`. It recomputes only when the
focused index changes or a rescan invalidates the entry list. A uuid is stable
for the life of a file, so recomputing it per frame buys nothing and costs a
lock the background importer wants.

**Orphan deletion needs clean evidence.** The sidecar is deleted only when the
asset file is confirmed absent:

```cpp
std::error_code existsEc;
const bool assetPresent = std::filesystem::exists(path.parent_path() / assetName, existsEc);
if (!assetName.empty() && !assetPresent && !existsEc)
{
    orphanedSidecars.push_back(path);
}
```

An `existsEc` that is set means "could not determine", which is not evidence
of absence. Keeping the sidecar in that case costs nothing; deleting it loses
the uuid permanently.

**Atomic sidecar writes.** `WriteSidecar` writes to `<sidecar>.tmp`, flushes,
closes, checks the stream, then `std::filesystem::rename`s over the
destination. A crash then leaves either the old sidecar or the new one, never
an empty file. `rename` over an existing file is atomic on both NTFS and POSIX.

### Group B: cache lifecycle

**Const handle first**, because the eviction work touches the same structures.

```cpp
std::shared_ptr<const LoadedModelData> Get(const std::string& path);

// Mutating entry points, replacing writes through Get's handle.
void UpdateMaterial(const std::string& path, uint32_t materialIndex, const ModelImportedMaterialInfo& material);
void UpdateMaterials(const std::string& path, const std::vector<ModelImportedMaterialInfo>& materials);
```

`Store` keeps taking `std::shared_ptr<LoadedModelData>`;
`shared_ptr<T>` converts implicitly to `shared_ptr<const T>`, so the refill at
`scene_renderables.cpp:97` needs only a local variable's type changed, not a
restructure. Both update functions apply `ApplyImportedMaterialInfo` under the
cache mutex and are no-ops when the entry is absent, matching what the current
`if (cached && index < size)` guards do at the call sites.

**Eviction.** Each cache entry carries its byte size and a last-access
counter:

```cpp
size_t EstimateBytes(const LoadedModelData& data);  // Σ vertices*sizeof(Vertex) + indices*sizeof(uint32_t)
void Trim(const std::unordered_set<std::string>& liveKeys, size_t budgetBytes);
```

`Get` and `IsCached` bump the entry's counter. `Trim` keeps every entry whose
normalized key is in `liveKeys` regardless of budget, then evicts the
remaining entries oldest-first until the total is within budget. `liveKeys`
must be normalized with the cache's own key function, so `Trim` normalizes
what it is given rather than trusting the caller.

A new `TrimModelCache(RendererSharedState&)` in `scene_renderables.cpp`
collects live source paths from `registry.view<const ModelComponent>()` and
calls `Trim`. It runs at the end of `RebuildSceneRenderables` unconditionally
and at the end of `RefreshDirtySceneRenderables` only when that call reported
a change — those are exactly the moments the live set can have changed, so
tying eviction to them avoids per-frame work that is almost always a no-op.
Budget: 1 GB.

**Preview texture cache.** Same counter-based LRU, budget 256 MB, but with no
live set — the preview panel has no persistent claim on a texture.
`CachedPreviewTexture` gains a `lastAccess` counter; `ResolvePreviewTexture`
bumps it and trims after inserting.

**Rename invalidation.** `CommitRename` calls `ModelCache::Invalidate(oldPath)`
after a successful rename, before the registry and sidecar updates.
`AssetManager` and `ModelCache` are both in `engine_asset`, so this is a
direct call.

### Group C: reference scanning

New unit `engine/asset/asset_references.h` / `.cpp`:

```cpp
struct AssetReference
{
    std::string referencedName;  // the doomed file's name
    std::string referencedBy;    // file name of the referencing document
};

// Scans .gltf/.yaml documents under `root` for mentions of any of `names`,
// skipping uuid sidecars and anything in `excludePaths` (each entry spelled as
// lexically_normal().string(), matching what BuildPendingDeleteWarnings
// already builds today). Substring matching on the file name: it can flag a
// same-named file in another folder, but a spurious warning is cheap next to a
// silently broken reference.
std::vector<AssetReference> FindReferencesTo(
    const std::filesystem::path& root,
    const std::vector<std::string>& names,
    const std::unordered_set<std::string>& excludePaths,
    size_t maxResults = 6);
```

Behind it, a process-wide index mapping each scanned document to its
`last_write_time` and the set of file names it mentions. A scan re-reads only
documents whose `last_write_time` changed since the last scan; everything else
is answered from the index. The first scan still walks the tree, so the worst
case is unchanged, but the repeat case — which is what a rename triggers —
becomes a stat per document.

The index is a cache of derived data: when a document's `last_write_time`
cannot be read, it is re-read rather than trusted from the index.

`BuildPendingDeleteWarnings` becomes a caller of `FindReferencesTo`, and
`CommitRename` becomes a second one: it collects references to the old name
and, when any exist, routes through the same confirmation modal the delete
flow uses rather than renaming immediately.

### Testing

`asset_registry_tests` gains:

| Case | Assertion |
| --- | --- |
| Orphan judgment needs clean evidence | A sidecar whose asset is present survives a rescan; the existing orphan case still deletes one whose asset is gone |
| Atomic sidecar write | After `GetOrCreateUuid`, no `.tmp` file is left beside the sidecar, and the sidecar parses |

New `model_cache_tests` (compiles `model_cache.cpp` directly; it depends on
nothing but `model_loader.h`'s types):

| Case | Assertion |
| --- | --- |
| Byte accounting | `EstimateBytes` matches vertices×`sizeof(Vertex)` + indices×4 for a hand-built `LoadedModelData` |
| Live keys pin | An entry in `liveKeys` survives `Trim` with a budget of 0 |
| LRU order | With three unreferenced entries and a budget fitting two, the least recently `Get`-ed one is evicted |
| Normalization | A `liveKey` spelled with different separators than the stored key still pins its entry |
| Update entry points | `UpdateMaterial` changes what a later `Get` observes; both are no-ops for an absent path |

New `asset_references_tests` (compiles `asset_references.cpp` directly):

| Case | Assertion |
| --- | --- |
| Finds a mention | A `.material.yaml` naming `tex.png` is reported when `tex.png` is scanned for |
| Skips sidecars and excluded paths | A uuid sidecar naming its own asset is not a reference; a file in `excludePaths` is not one either |
| Index reuse | Between two scans, a document's content is changed and its `last_write_time` restored with `std::filesystem::last_write_time`'s setter. The second scan returns the first scan's answer, proving the document was not re-read. Deterministic and portable, unlike making a file unreadable. |
| Index invalidation | Rewriting a document between scans, leaving its `last_write_time` to advance normally, changes the result |

## Out of Scope

Deferred findings from the same review, recorded so no task reaches for them:

- Moving `AssetManager` out of `engine/asset` and dropping imgui from that
  target.
- The five separate path-normalization helpers.
- `BuildMaterialDefinitionPath` colliding for same-stem models.
- `STBI_THREAD_LOCAL` not being defined while decodes run concurrently.
- `ScanCurrentDir`'s ineffective `error_code` handling.
- Paste producing a broken single-file copy.
- Uuids for texture references inside materials (locked decision 4).
