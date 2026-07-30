# Material Alpha Pipeline Design

## Goal

Make material alpha behavior correct and editable from glTF import through the
material sidecar and editor to Vulkan rendering. Opaque materials must not
blend, alpha-mask materials must use a configurable cutoff while writing
depth, and alpha-blend materials must render after opaque geometry in stable
back-to-front submesh order.

This design deliberately stops at material correctness. Dynamic
viewport/scissor state, descriptor-layout ownership, frustum culling, memory
suballocation, shadows, IBL, and antialiasing remain separate follow-up work.

## Current Problems

- The Vulkan pipeline enables alpha blending and depth writes for every
  material.
- `alphaMode` is not carried past glTF import, so the renderer cannot
  distinguish Opaque, Mask, and Blend materials.
- The glTF loader copies the same base-color alpha into both `baseColor[3]`
  and `opacity`; the render path multiplies them and squares imported alpha.
- A positive `alphaCutoff` currently triggers fragment discard without an
  explicit Mask mode.
- Draw items retain submesh insertion order, so blended geometry is not
  sorted.
- The material editor and `.material.yaml` sidecar cannot inspect or modify
  alpha mode and cutoff.

## Shared Material Model

Add a backend-independent enum to the scene material model:

```cpp
enum class MaterialAlphaMode
{
    Opaque,
    Mask,
    Blend
};
```

`MaterialPbrSurfaceSettings` owns the canonical editable values:

```cpp
MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
float alphaCutoff = 0.5f;
```

`ModelMaterialData` keeps matching flat values because the existing model
loading and renderable-building path mirrors PBR fields in two forms. This
implementation must keep both representations synchronized but must not use
the alpha work as an excuse for a broader material-model rewrite.

The backend-independent `CpuRenderSubmesh` carries:

- `MaterialAlphaMode alphaMode`;
- the existing `doubleSided` flag;
- a local-space submesh bounds center used for transparent sorting.

The bounds center is computed once from the CPU mesh AABB. An empty mesh uses
the local origin as a safe fallback. Vulkan consumes these values without
depending on glTF or editor-only types.

## Import and Alpha Semantics

glTF values map exactly as follows:

| glTF `alphaMode` | MiniEngine mode |
| --- | --- |
| `OPAQUE` or missing | `Opaque` |
| `MASK` | `Mask` |
| `BLEND` | `Blend` |

For Mask, import glTF `alphaCutoff`; glTF's default is `0.5`. Values entering
runtime rendering are clamped to `[0, 1]`.

The imported base-color factor, including `baseColorFactor[3]`, remains
unchanged. The editor's `opacity` remains an independent multiplier and is
initialized to `1.0` for glTF materials. Final fragment alpha is therefore:

```text
sampled base-color alpha * baseColorFactor.a * opacity
```

No import path copies the same alpha into both factor and opacity.

Opaque ignores final alpha for coverage. Mask compares final alpha with
`alphaCutoff`. Blend uses final alpha for source-alpha blending.

## Material Sidecar Compatibility

The canonical `.material.yaml` representation places both values in `pbr`:

```yaml
pbr:
  alpha_mode: mask
  alpha_cutoff: 0.5
```

The writer emits lowercase `opaque`, `mask`, or `blend`. The loader treats a
missing `alpha_mode` as Opaque and a missing `alpha_cutoff` as `0.5`, so
existing sidecars remain compatible. An unknown mode logs a warning and falls
back to Opaque. Cutoff and opacity are clamped before runtime use.

Material cache updates, sidecar writes, and shader-graph compilation must copy
the new PBR fields along with the existing PBR values. Saving an old sidecar
normalizes it to the canonical fields.

## Editor and Preview Behavior

The PBR material controls add an Alpha Mode combo with Opaque, Mask, and
Blend. Alpha Cutoff is visible and editable only for Mask. A change uses the
existing material-save and dirty-renderable path so the viewport refreshes
without a new update mechanism.

The CPU material preview follows the same coverage rules:

- Opaque returns alpha `1.0`.
- Mask returns alpha `0.0` below cutoff and `1.0` otherwise.
- Blend returns the computed final alpha.

The preview material signature includes alpha mode and cutoff so changing
either value invalidates the cached preview.

## Vulkan Pipeline Variants

Introduce a `PipelineKey` composed of:

```text
(MaterialAlphaMode, doubleSided)
```

The renderer owns six static pipeline variants:

| Alpha mode | Blending | Depth test | Depth write | Fragment discard |
| --- | --- | --- | --- | --- |
| Opaque | Disabled | Enabled | Enabled | No |
| Mask | Disabled | Enabled | Enabled | Below cutoff |
| Blend | Source alpha | Enabled | Disabled | No |

For each row, `doubleSided=false` uses back-face culling and
`doubleSided=true` uses `VK_CULL_MODE_NONE`.

`VulkanPipeline` receives a focused configuration rather than independent
booleans. Fragment shader Mask behavior uses a specialization constant, so
Opaque and Blend fragments do not pay for a runtime alpha-mode branch.
`alphaCutoff` continues to occupy `MaterialPushConstants::emissiveFactor.a`;
the RGB values remain emissive color. This preserves the existing 128-byte
`ObjectPushConstants`, which already reaches Vulkan's minimum guaranteed push
constant limit.

A temporary `PipelineSet` must create all six variants successfully before it
replaces the live set. If any creation fails, temporary resources are
destroyed and the existing live pipeline resources remain intact.

This phase retains the existing descriptor-set-layout ownership and pipeline
rebuild triggers. Decoupling those lifetimes belongs to the viewport-stall
phase.

## Render Queue Ordering

Every `VulkanDrawItem` carries its `PipelineKey` and transparent sort depth.
Queue construction produces two ordered regions:

1. Opaque and Mask items.
2. Blend items.

Opaque and Mask items may be grouped by `PipelineKey` to reduce binds while
preserving insertion order within a key. They always complete before any
Blend draw.

For Blend items, transform the local submesh bounds center by the model and
view matrices. Define positive view depth as:

```text
viewDepth = -(view * model * localCenter).z
```

Use a stable descending sort so farther submeshes render first. Transparent
items remain in one global depth order even when their culling modes differ;
pipeline-bind reduction must not override blend correctness.

This is intentionally submesh-level sorting. Intersecting transparent
geometry and self-overlapping triangles remain known limitations requiring
mesh splitting or a future order-independent-transparency design.

## Failure Handling

- Invalid sidecar modes warn and fall back to Opaque.
- Missing alpha fields use the documented compatibility defaults.
- Cutoff and opacity are clamped rather than allowed to create undefined
  coverage behavior.
- Missing bounds data uses the local origin and does not abort a frame.
- Pipeline-set replacement is all-or-nothing.
- A material update continues to use the existing in-flight-frame wait before
  old descriptor and pipeline resources are destroyed.

## Automated Verification

Add a focused `miniengine.material_alpha` test target. Tests cover:

1. glTF Opaque, Mask, Blend, missing-mode, cutoff, and base-alpha mapping.
2. Canonical sidecar serialization and old-sidecar defaults.
3. Unknown sidecar mode fallback.
4. Imported alpha is multiplied exactly once.
5. Editor PBR values propagate to cached model data and CPU render submeshes.
6. All six pipeline keys map to the required blending, depth-write, discard,
   and culling state.
7. Opaque and Mask items precede every Blend item.
8. Blend items sort stably from far to near using transformed submesh centers.
9. CPU preview coverage matches Opaque, Mask, and Blend semantics.

Final automated verification consists of:

1. Configure and build the complete `vs2026-x64-debug` preset.
2. Run all CTest targets with failure output.
3. Run `miniengine_app.exe --frames 60`.
4. Run `git diff --check` and inspect the final changed-file scope.

Builds, CTest, and the smoke run do not establish visual correctness.

## Manual GUI Acceptance

The user validates at least these cases:

1. An Opaque material with an alpha-bearing texture remains fully opaque.
2. Changing Mask cutoff immediately changes cutout edges, and cutouts retain
   correct depth occlusion.
3. Two overlapping Blend submeshes composite back-to-front as the camera
   changes position.
4. Alpha Mode and Alpha Cutoff persist after saving and restarting.
5. Single- and double-sided materials select the correct variant in all three
   alpha modes.
6. Sponza Opaque dirt/decal materials no longer enter the blend path merely
   because their textures contain alpha.

The feature is not visually accepted until the user confirms these checks.

## Out of Scope

- Per-triangle transparent sorting.
- Order-independent transparency.
- Dynamic Vulkan blend, depth, or cull state extensions.
- Descriptor indexing or bindless materials.
- Descriptor-layout and viewport-lifetime refactoring.
- Frustum culling and draw batching beyond pipeline-key grouping.
- Memory suballocation.
- Shadows, IBL, MSAA, TAA, FXAA, or post-processing.
