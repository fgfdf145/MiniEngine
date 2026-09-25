# Material Variants (Phase 3b of the Complete BRDF Program)

## Goal

`KHR_materials_variants`: one glTF carries several material bindings per primitive (a shoe's or a
car's colour options) and the scene picks one by name. Import and selection only; the shading does
not change. See `2026-09-25-complete-brdf-program.md`.

## Current State

- Every material in the glTF is loaded into `LoadedModelData::materials`, used or not; each
  primitive becomes a `ModelSubmeshData` with one `materialIndex`.
- `BuildEntityRenderSubmeshes` (`scene_renderables.cpp`) builds each draw from
  `modelData.materials[submesh.materialIndex]`, per entity, and is rerun when an entity is marked
  `ModelRenderableDirty`. The base colour texture override on `ModelComponent` already works this way.

## Decisions

1. **Import.** The root extension's `variants` array becomes `LoadedModelData::materialVariants`, a
   list of names (a blank one is called `Variant <n>`; the extension requires names). Each primitive's `mappings` become
   `ModelSubmeshData::variantMaterialIndices`, one entry per variant, starting as the primitive's own
   material and overwritten where a mapping names the variant. A mapping with an out-of-range
   material or variant index is skipped with a warning; a variant named twice for one primitive keeps
   the first mapping (the extension says each variant appears at most once). A model without the
   extension has no variants and empty lists.
2. **Choosing a material.** `ResolveSubmeshMaterialIndex(submesh, variantIndex)` returns the variant's
   material, or `materialIndex` for no variant (`std::nullopt`) or an index past the list.
   `FindMaterialVariant(model, name)` maps a name to its index; the empty name is the default.
3. **The choice lives on the entity**, not the model: `ModelComponent::materialVariant`, a name,
   empty for the glTF's default bindings. Two entities of one model can wear different variants.
   Scenes store it as `material_variant` under `model:`; an older scene without the key loads the
   default. By name, not index, so a re-exported glTF that reorders its variants keeps the choice; a
   name the model does not have falls back to the default with a warning.
4. **Rendering** resolves each submesh's material through the entity's variant; everything that
   read `submesh.materialIndex` there (factors, flags, textures, the texture override's UV check)
   reads the resolved index. Variant materials are ordinary materials in the model's list: the
   material panel edits them and their sidecars save as for any other.
5. **Editor.** `EditorModelMetadataComponent` gains the variant names (through `UpdateModelInfo`);
   the scene panel shows a `Material Variant` combo (`Default` plus the names) for a model that has
   any. Picking one sends a UI action; `EntityEditService::ApplySelectedModelMaterialVariant` sets the
   name and marks the entity dirty, restoring the old value if the rebuild throws, as the texture
   override does.

## Automated Verification

- Import: names (a blank name becomes `Variant <n>`), mappings over two primitives, a primitive with no
  mappings keeps its material for every variant, an out-of-range material index is skipped, a
  repeated variant keeps its first mapping, a model without the extension has no variants.
- `ResolveSubmeshMaterialIndex` for no variant, each variant, an index past the list;
  `FindMaterialVariant` for a known name, an unknown name and the empty name.
- Scene round trip keeps `material_variant`; a scene without the key loads the default.

## Manual Acceptance (by image)

1. Khronos' `MaterialsVariantsShoe` shows each of its three variants (`midnight`, `beach`, `street`)
   and the default, compared side by side with the Khronos sample viewer showing the same variant
   (the reference comparison, `2026-09-26-khronos-reference-comparison-design.md`).
2. Sponza: within the run-to-run noise of the previous build.
