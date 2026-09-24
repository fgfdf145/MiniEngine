# Clearcoat

## Goal

Render a clear lacquer layer over the metallic-roughness base, as `KHR_materials_clearcoat` defines
it: car paint, varnished wood, coated plastic. The coat is a second, dielectric GGX lobe (F0 = 0.04)
with its own roughness, on top of a base that it dims by the light its Fresnel reflects.

Surfaces without a coat must render exactly as before.

## Current State

- `GpuMaterialData` (5 x vec4, set 0 binding 12, one per draw) carries the base material and a
  shading model id; `ShadingModel::DefaultLit = 0` is the only model. `gbuffer.frag` writes the id
  to GB2.a; nothing reads it yet.
- `ShadeSurface` (`pbr_common.glsl`) shades the base for both orders: ambient (uniform or sky SH plus
  prefiltered specular), then directional lights and the pixel's cluster of local lights through
  `EvaluateSceneLight`. The caller adds emissive afterwards.
- GB1 holds the shading normal in `.rg` and the face-flipped geometric normal in `.ba`.
- Material factors live in `MaterialPbrSurfaceSettings` (mirrored by the flat fields of
  `ModelMaterialData`), saved under `pbr:` in `.material.yaml`, edited by `DrawMaterialPbrControls`,
  and copied into the per-draw material by `scene_renderables.cpp`.
- The G-buffer is five colour targets (GB0-GB3, velocity) plus depth; set 2 samples GB0-GB3, depth,
  velocity and AO.

## Decisions

1. **Factors, not textures.** `clearcoatFactor` and `clearcoatRoughnessFactor` (both [0, 1]) are
   imported, stored, edited and shaded. `clearcoatTexture`, `clearcoatRoughnessTexture` and
   `clearcoatNormalTexture` are not: they would need three more material sampler slots. Import logs
   a warning when a material has them, so a model that relies on them is not silently wrong.
2. **The coat uses the geometric normal.** The extension leaves the coat unbumped unless it has its
   own normal texture, and GB1.ba already holds the geometric normal, so neither order needs a new
   normal.
3. **The shading model says when there is a coat.** `ShadingModel::Clearcoat = 1` when the
   material's clearcoat factor is above zero. Only those pixels read the coat parameters and run the
   coat's lobe; every other pixel runs the code it runs today.
4. **A custom data target, GB5.** `R8G8B8A8_UNORM`, written by the geometry pass and read through
   set 2 binding 7. Its channels mean what the pixel's shading model says: for Clearcoat, `.r` is the
   factor and `.g` the roughness; `.ba` are zero. Sheen is expected to use all four. Default Lit
   pixels write zeros and the lighting pass does not read it for them.
5. **`GpuMaterialData` gains a sixth vec4**, `clearcoatFactors` (x factor, y roughness), 96 bytes.
6. **The model follows the Khronos glTF sample viewer**, the renderer the extension's sample assets
   are checked against:
   - per light, the coat adds `radiance * NdotL_c * D(alpha_c) * G(alpha_c) * F(0.04, VdotH) / (4 NdotV_c NdotL_c)`
     with `alpha_c = roughness_c^2`, `roughness_c` floored at 0.04 like the base;
   - the environment adds the prefiltered sky along `reflect(-V, N_c)` at `roughness_c`, weighted by
     `0.04 A + B` from the DFG table (or Karis' fit under the uniform ambient), times the occlusion;
   - the base, emissive included, is scaled by `1 - clearcoat * F_Schlick(0.04, NdotV_c)`, and the
     coat terms are added times `clearcoat`.
   The shadow caster's shadow multiplies its coat term as it does its base term. Area lights use the
   same representative-point approximation for the coat as for the base, with the coat's normal and
   roughness. The coat gets no multiple-scattering compensation: its lobe is usually smooth, where
   the loss is negligible.
7. **Emissive moves into `ShadeSurface`**, because the coat dims it. For coatless pixels the sum is
   `(ambient + direct) + emissive` in the same order as today.

## Data Flow

```
glTF KHR_materials_clearcoat ─► ModelMaterialData / MaterialPbrSurfaceSettings
     .material.yaml pbr: clearcoat_factor, clearcoat_roughness_factor ◄─► editor sliders
                                   │ scene_renderables
                                   ▼
GpuMaterialData.clearcoatFactors, shadingModel = Clearcoat when factor > 0
        │                                    │
   gbuffer.frag                          triangle.frag (forward)
   GB2.a = id, GB5.rg = factor, roughness    │
        │                                    │
   deferred_lighting.frag: id == Clearcoat → read GB5
        └──────────────► ShadeSurface(..., CoatParams, emissive)
```

### Shaders

- `pbr_common.glsl`: a `CoatParams { float factor; float roughness; vec3 normal; }` argument to
  `ShadeSurface`, which now also takes emissive. `EvaluateSceneLight` returns the base term as today
  and, when the coat is on, a coat term through an out parameter; the light's `L` and radiance are
  computed once for both.
- `gbuffer_common.glsl`: `SHADING_MODEL_CLEARCOAT = 1u`.
- `gbuffer_inputs.glsl`: binding 7, `gbufferCustom`.
- `material_common.glsl`: `vec4 clearcoatFactors`.

### C++

- `material.h`: `ShadingModel::Clearcoat`, `clearcoatFactors[4]`, the size and offset asserts.
- `MaterialPbrSurfaceSettings` and `ModelMaterialData`: `clearcoatFactor = 0`,
  `clearcoatRoughnessFactor = 0`, copied everywhere the other factors are (the two conversions in
  `model_loader.cpp`, `ApplyImportedMaterialInfo`, the sidecar and graph-node serialisers, the
  preview hash, the processor panel's resolved-material line).
- `DrawMaterialPbrControls`: "Clearcoat" and "Clearcoat Roughness" sliders, [0, 1].
- Render targets: `RenderTargetId::GBufferCustom`, described like the other G-buffer targets,
  appended to `VulkanGeometryPass::kAttachments` (colour location 5, cleared to zero) and to
  `VulkanGBufferDescriptors::kInputs` (binding 7), and read by the tone mapping pass like the others.
- `GBufferDebugView::Custom = 9`, "G-buffer: custom data (clearcoat)" in Graphics Debug, showing
  GB5's rgb.

## Error Handling

- Factors outside [0, 1] from a file are clamped at import and at load of the sidecar.
- Coat textures: a warning naming the material, then ignored.

## Automated Verification

- Import: a generated glTF with clearcoat 0.8 / roughness 0.2 gives those factors; out-of-range
  values clamp; no extension gives 0 / 0; a material with `clearcoatTexture` still loads its factors.
- Sidecar: serialise then load keeps both factors; a sidecar written before this change loads with
  0 / 0.
- `static_assert`s pin `GpuMaterialData` at 96 bytes and `clearcoatFactors` at offset 80.

## Manual Acceptance

1. **Coatless scenes do not change.** Sponza with five lights, deferred and forward-only, within the
   run-to-run noise of the previous build.
2. **The coat is visible and plausible.** A row of rough red spheres, coat 1 and coat roughness 0.05,
   shows a sharp sky reflection and sun highlight over the blurry base, strongest at grazing angles;
   coat 0 matches the uncoated row.
3. **Both orders agree.** The coated spheres differ between deferred and forward-only by no more
   than the uncoated ones do (VBAO, which only the deferred order has, is the known difference).

## Amendments During Implementation

- **The acceptance spheres use coat roughness 0.2, not 0.05.** At 0.05 the sun's coat highlight is
  narrower than a pixel on 45-pixel spheres, so it is missed by sampling; the coat still shows as a
  faint sky reflection (green and blue +3 on the sphere average). At 0.2 every coated sphere keeps a
  sharp highlight whatever its base roughness, while the uncoated row's highlight spreads and fades.
- **Both orders agree once VBAO is off.** With VBAO on, deferred and forward-only differ a little
  more on coated spheres (mean 2.1 against 1.6 on uncoated ones, max 17 against 12), because VBAO,
  which only the deferred order runs, also darkens the coat's ambient. With it off in both, every row
  agrees within two 8-bit steps (mean 0.3-0.5).
- **`triangle.frag` includes `gbuffer_common.glsl`** for the `SHADING_MODEL_*` constants.
- **Coatless scenes:** Sponza with five lights differs from the previous build in 8 pixels deferred
  and 0.85% forward-only, by one 8-bit step, within two runs of one build.

## Out of Scope

- Clearcoat textures, including the coat's own normal map.
- Coat tint or absorption (not in the extension), coat IOR other than 1.5.
- Clearcoat in the editor's software material preview.
- Multiple-scattering compensation for the coat lobe.
- Graph-node inputs for the coat factors (they are edited on the material's PBR settings).
