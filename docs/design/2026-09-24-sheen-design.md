# Sheen

## Goal

Render cloth-like retroreflective sheen (`KHR_materials_sheen`): velvet, satin, microfibre. Sheen is
a soft lobe from fibres standing off the surface, brightest at grazing angles, tinted by its own
colour, layered over the base.

Surfaces without sheen must render as before.

## Current State

- `GpuMaterialData` (6 x vec4) carries the base, the shading model and the clearcoat factors.
  `ShadingModel::DefaultLit = 0` and `Clearcoat = 1` exist; GB5 (`R8G8B8A8_UNORM`, set 2 binding 7)
  holds per-shading-model data, and clearcoat uses `.rg` of it.
- `ShadeSurface` takes emissive and a `CoatParams`; with no coat it sums `ambient + direct + emissive`.
- The DFG table (`BuildEnvironmentBrdfLut`, 64 x 64, set 0 binding 9) stores GGX `(A, B)` in `.rg`;
  `.b` is written as 0 and unused.
- Clearcoat's factors run through import, `.material.yaml`, graph nodes, the PBR editor controls, the
  preview hash and the processor panel; sheen follows the same path.

## Decisions

1. **Factors, not textures.** `sheenColorFactor` (linear RGB, [0, 1]) and `sheenRoughnessFactor`
   ([0, 1]) are imported, stored, edited and shaded. `sheenColorTexture` and `sheenRoughnessTexture`
   are not; import warns when a material has them.
2. **`ShadingModel::Sheen = 2`** when the largest sheen colour component is above zero. GB5 holds the
   sheen colour in `.rgb` and the sheen roughness in `.a`.
3. **One layer per pixel.** GB5 has room for either the coat or the sheen, not both. A material with
   both uses Clearcoat and loses its sheen, in both orders, so they agree. Import warns; the editor
   shows a note under the controls when both are set.
4. **Filament's sheen model.** Charlie distribution (Estevez & Kulla 2017) with Neubelt's visibility,
   `f_sheen = sheenColor * D_Charlie(alpha_s, NdotH) * V_Neubelt(NdotV, NdotL)`,
   `alpha_s = roughness_s^2`, `roughness_s` floored at 0.04. The base's diffuse and specular, direct
   and ambient, are scaled by `1 - max(sheenColor) * E_sheen(NdotV, roughness_s)`, the sheen lobe's
   directional albedo; emissive is not. The sheen lobe uses the shading normal (it sits on the base).
5. **The sheen's directional albedo goes in the DFG table's blue channel.** `IntegrateSheenAlbedo`
   integrates `D_Charlie * V_Neubelt * NdotL` over the hemisphere on the CPU at start-up, like the
   GGX terms, and `SampleEnvironmentBrdf` returns all three channels.
6. **Sheen ambient follows Filament**: the GGX-prefiltered sky along the reflection vector at the
   sheen roughness, times `sheenColor * E_sheen`, times AO; under the uniform ambient, the ambient
   luminance in place of the sky. The Khronos sample viewer uses a Charlie-prefiltered cube instead;
   a second prefiltered cube is not worth it for a lobe this broad.
7. **Area lights** evaluate the sheen lobe toward the rectangle's centre, times the irradiance its
   form factor gives: the lobe is broad, so a representative point would change little.
8. **`GpuMaterialData` gains a seventh vec4**, `sheenFactors` (rgb colour, a roughness), 112 bytes.

## Shaders

- `pbr_common.glsl`: `SheenParams { vec3 color; float roughness; }` and `NoSheen()`;
  `D_Charlie`, `V_Neubelt`, `EvaluateSheen(N, V, L, sheen, radiance)`. `EvaluateSceneLight` returns the
  sheen term through a second out parameter; `ShadeSurface` takes a `SheenParams` after the coat and,
  when the sheen is on, sums `(ambient + direct) * scaling + sheenDirect + sheenAmbient + emissive`.
- `gbuffer_common.glsl`: `SHADING_MODEL_SHEEN = 2u`; `gbuffer.frag` writes GB5 for it;
  `deferred_lighting.frag` and `triangle.frag` build `SheenParams` from GB5 or the material.
- `material_common.glsl`: `vec4 sheenFactors`.

## Automated Verification

- `IntegrateSheenAlbedo`: finite and in [0, 1] over the table; within 0.01 of a brute-force
  integration over outgoing directions on a fine grid (an independent quadrature); 512 samples
  within 0.01 of 4096; the table's blue channel holds it at texel centres.
- Import: colour and roughness read and clamped; absent extension gives no sheen; textures warn and
  keep the factors; a material with both coat and sheen still loads both factors.
- Sidecar round trip and a legacy sidecar without the keys.
- `static_assert`s: `GpuMaterialData` 112 bytes, `sheenFactors` at 96.

## Manual Acceptance

1. Sheen-less scenes unchanged (Sponza, both orders, within noise).
2. A dark red base with white sheen brightens toward the silhouettes, more so at higher sheen
   roughness; the no-sheen row does not.
3. With VBAO off, deferred and forward-only agree on sheen spheres as on plain ones.

## Amendments During Implementation

- **The sheen albedo exceeds 1 and is clamped in the table.** Charlie with Neubelt's visibility is
  not energy conserving: smooth sheen at grazing angles integrates to 1.74 (roughness 0.1, N.V 0.05),
  which a brute-force quadrature over outgoing directions and a separate Python integration both
  confirm. `IntegrateSheenAlbedo` returns the true integral (its test allows up to 2);
  `BuildEnvironmentBrdfLut` stores it clamped to 1, so the base scaling never goes negative and the
  sheen ambient never reflects more than arrives.
- **Acceptance** (Sponza with five lights, 3500 frames): deferred differs from the previous build in
  0.59% of pixels by one 8-bit step, forward-only in 0.85%; two runs of one build differ in 0.59%.
  On the sphere grid, with VBAO off in both orders, deferred and forward-only agree within two 8-bit
  steps on every row (mean 0.14-0.51); with it on, the sheen rows differ about as much as the plain
  row (mean 1.1-1.2).
- **The GB5 debug view** is renamed "custom data (clearcoat / sheen)"; for sheen it shows the colour.

## Out of Scope

- Sheen textures; sheen and clearcoat on one pixel; a Charlie-prefiltered environment.
- Sheen in the software material preview; graph-node inputs for the sheen factors.
