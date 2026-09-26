# Material Foundation

## Goal

Make room for material models beyond metallic-roughness (clearcoat and sheen next) without changing
a single pixel today. Two things stand in the way:

1. **The push constant is full.** `ObjectPushConstants` is the model matrix plus
   `MaterialPushConstants`, exactly the 128 bytes Vulkan guarantees. Not one more float fits.
2. **The G-buffer cannot tell surfaces apart.** Every pixel is shaded as metallic-roughness; there is
   no per-pixel record of which shading model a surface uses.

This design moves the material parameters into a storage buffer, shrinks the push constant to the
model matrix, and reserves GB2.a for a shading model id. It also starts reading `KHR_materials_*`
extensions at import, with the one that has a consumer today.

## Current State

- `MaterialPushConstants` (4 x vec4: base colour, emissive + alpha cutoff, surface factors, node
  graph factors) lives in each `RenderSubmesh`, filled from the model's `ModelMaterialData` by
  `scene_renderables.cpp`. `BuildDrawItems` copies it into every `VulkanDrawItem`'s push constant,
  and `triangle.vert`, `triangle.frag` and `gbuffer.frag` all declare the same block.
- Every draw already carries a per-draw index: `firstInstance` is the submesh index (the motion
  slot), which `triangle.vert` uses as `gl_InstanceIndex` to read last frame's model matrix from set
  0 binding 2.
- `VulkanUniformBuffer` is rebuilt on every content upload, sized by the submesh count, and waited
  for before the old one is destroyed. So anything it holds about the submeshes is immutable for its
  lifetime.
- The shadow pass has its own 112-byte push constant and copies only the alpha-test fields from
  `MaterialPushConstants`.
- GB2 is `R8G8B8A8_UNORM`: metallic, roughness, occlusion, and `a` written as 0.
- The glTF loader (tinygltf) reads core material fields only. `ModelMaterialData::emissiveIntensity`
  exists, is saved in `.material.yaml` and multiplies the emissive colour, but import always sets it
  to 1.

## Decisions

1. **One material record per draw, not a material table.** The parameters already live per
   submesh, and the draw slot already indexes a per-draw buffer, so the material buffer is indexed
   by the same slot. A deduplicated table would need material identity the renderer does not have
   yet and would save kilobytes (405 Sponza submeshes x 80 bytes = 32 KiB).
2. **The buffer is immutable per content.** It is written once, when `VulkanUniformBuffer` is built,
   at set 0 binding 12 (fragment stage), one buffer shared by every frame in flight. Material edits
   already go through a content rebuild, so nothing changes for them.
3. **`MaterialPushConstants` becomes `GpuMaterialData`.** Five vec4s: the four it has, unchanged in
   meaning, plus `uvec4 shadingModel` (x = the id, yzw reserved). `std430`, 80 bytes. The name stops
   claiming to be a push constant.
4. **The push constant is the model matrix alone.** `ObjectPushConstants` shrinks to 64 bytes. The
   fragment shaders get the slot from `triangle.vert` as a flat `uint` (location 7).
5. **The shadow pass is left as it is.** It keeps its own push constant and copies the alpha-test
   fields from `GpuMaterialData`, as it does today.
6. **GB2.a holds the shading model id.** `ShadingModel::DefaultLit = 0` is the only model, so GB2.a
   stays 0 and nothing reads it yet; `gbuffer_common.glsl` gets the encode and decode (`id / 255`).
   Clearcoat is its first consumer.
7. **Extensions are read when something uses them.** This design reads
   `KHR_materials_emissive_strength` into the existing `emissiveIntensity`, which the renderer already
   applies. Clearcoat and sheen are read in the designs that shade them, so no field exists that
   nothing reads.

## Data Flow

```
ModelMaterialData ──(scene_renderables)──► RenderSubmesh::material : GpuMaterialData
                                                          │
                              content upload: VulkanUniformBuffer(materials = one per submesh)
                                                          │ set 0 binding 12, immutable
draw i: push constant = model matrix, firstInstance = i   ▼
triangle.vert ──flat uint drawSlot──► triangle.frag / gbuffer.frag: materials[drawSlot]
                                                          │
                                           gbuffer.frag: GB2.a = shading model id
```

### C++

- `engine/renderer/material.h`: `GpuMaterialData` (the old four arrays plus `uint32_t
  shadingModel[4]`), `enum class ShadingModel : uint32_t { DefaultLit = 0 }`,
  `ObjectPushConstants { glm::mat4 model; }`, with `static_assert`s on the size (80, 64) and the
  offsets of every member.
- `VulkanUniformBuffer`: a `std::span<const GpuMaterialData>` constructor parameter (at least one
  record, like the motion slots), one host-visible storage buffer, binding 12 in
  `VulkanFrameDescriptorSetLayout`, one more storage buffer per frame set in the pool.
- `VulkanRenderer`: passes each submesh's material to the constructor in submesh order, which is the
  draw slot order.
- `VulkanShadowPass` reads `GpuMaterialData` in place of `MaterialPushConstants`.

### Shaders

- `scene_common.glsl` is included by compute shaders too, so the material buffer is declared in a new
  `material_common.glsl`, included by `triangle.frag` and `gbuffer.frag`:
  `layout(set = 0, binding = 12, std430) readonly buffer MaterialBuffer { MaterialData materials[]; }`.
- `triangle.vert` keeps only `mat4 model` in its push constant block and writes
  `layout(location = 7) flat out uint fragDrawSlot = gl_InstanceIndex`.
- `triangle.frag` and `gbuffer.frag` replace `drawData.<field>` with
  `materialData.materials[fragDrawSlot].<field>`, same names.

### Import

`BuildMaterialData` reads `material.extensions["KHR_materials_emissive_strength"]["emissiveStrength"]`
(a non-negative number; anything else is ignored with a warning) into `emissiveIntensity`. Models
imported before keep their `.material.yaml`, so their look does not change until re-imported.

## Error Handling

- A content with no submeshes still binds set 0, so the buffer holds at least one default record,
  as the motion buffer does.
- A draw slot past the buffer would read out of bounds with no robustness feature to catch it;
  `VulkanUniformBuffer` refuses materials and motion slots of different counts at construction.

## Automated Verification

- `static_assert`s pin `GpuMaterialData` at 80 bytes with every member at its std430 offset, and
  `ObjectPushConstants` at 64.
- A loader test imports a generated glTF with `KHR_materials_emissive_strength` of 4 and checks
  `emissiveIntensity == 4`; one without the extension keeps 1; a negative or non-numeric strength
  keeps 1.
- The existing suite passes.

## Manual Acceptance

1. **No pixel changes.** Sponza with the five-light scene, captured at equal frame counts before and
   after, differs no more than two runs of one build do (at most one 8-bit step, under 1% of pixels).
2. **Forward-only too.** The same with the forward-only order, which reads the material in
   `triangle.frag`.

## Amendments During Implementation

- **One parameter instead of a count check.** Rather than refusing materials and motion slots of
  different counts, `VulkanUniformBuffer` takes the per-draw materials in place of its motion slot
  count and sizes both buffers from them, so they cannot disagree.
- **The push constant range is vertex-only.** Only `triangle.vert` still reads it.
- **No pixel change, within noise** (Sponza with five lights, 3500 frames): deferred differs from the
  previous build in 0.76% of pixels by one 8-bit step, forward-only in 0.45%; two runs of one build
  differ in 0.70%.

## Out of Scope

- Clearcoat, sheen and every other `KHR_materials_*` extension beyond emissive strength.
- A deduplicated material table, per-material updates without a content rebuild.
- Moving the model matrix out of the push constant.
- Any change to the shadow pass's push constant.
