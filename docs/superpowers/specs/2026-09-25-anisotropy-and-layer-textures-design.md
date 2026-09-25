# Anisotropy and Layer Textures

## Goal

Two material extensions in one step:

1. **Anisotropic specular** (`KHR_materials_anisotropy`): brushed metal, the brushed layer under
   car paint, hair-like highlights stretched along a direction on the surface.
2. **Layer textures**: `clearcoatTexture`, `clearcoatRoughnessTexture`, `sheenColorTexture` and
   `sheenRoughnessTexture`, which import only warns about today, and `anisotropyTexture`.

Materials that use none of them must render as before. The user accepts this by the rendered image.

## Current State

- `GpuMaterialData` is 7 x vec4: base, emissive, surface, node graph, shading model, clearcoat,
  sheen. The material set (set 1) has 13 samplers: six primary maps, six blend-graph layer-B maps
  and the blend mask.
- GB5 (`R8G8B8A8_UNORM`) holds per-shading-model data: clearcoat in `.rg`, sheen in `.rgba`. The
  shading model id sits in GB2.a (`ShadingModel`, 0 lit, 1 clearcoat, 2 sheen). A coat wins over a
  sheen; import warns.
- Direct lights use GGX `D` with Smith-Schlick `G` (`k = (r + 1)^2 / 8`); the environment uses the
  split sum with the DFG table and the prefiltered sky along `reflect(-V, N)`; area lights use a
  representative point.
- The tangent frame exists only in the geometry and forward fragment shaders (`fragWorldTangent`).

## Decisions

### Anisotropy

1. **Parameters as glTF defines them.** `anisotropyStrength` [0, 1], `anisotropyRotation`
   (radians, counter-clockwise from the tangent), `anisotropyTexture` (RG = direction in tangent
   space, remapped from [0, 1] to [-1, 1]; B = strength multiplier). Direction
   `d = rotate(textureRG, rotation)`, world tangent `T_a = normalize(TBN * vec3(d, 0))`, re-orthogonalised
   against the shading normal, strength `s = anisotropyStrength * textureB`.
2. **The lobe is the Khronos sample viewer's.** `alpha = roughness^2`, `alpha_t = mix(alpha, 1, s^2)`,
   `alpha_b = alpha`; anisotropic GGX `D` and the height-correlated anisotropic Smith visibility
   (Heitz 2014) for direct lights. With `s = 0` the pixel keeps the isotropic path exactly, so
   isotropic materials are bit for bit unchanged; the visibility term differs slightly between the
   two paths, which only shows as a tiny step between strength 0 and the smallest non-zero strength.
3. **Environment: the bent normal.** The prefiltered sky is looked up along
   `reflect(-V, bentNormal)`, where the bent normal leans toward the anisotropic normal
   (`cross(cross(B_a, V), B_a)`) by `1 - (1 - s (1 - roughness))^4`, as in the sample viewer and
   Filament. The DFG term, specular occlusion and the energy compensation keep the isotropic
   roughness. SSR traces isotropically: its rays do not stretch.
4. **Area lights** evaluate the anisotropic lobe toward their representative point. That is the same
   approximation the isotropic path already makes, stretched.
5. **Clearcoat, sheen and emissive are unaffected**; the coat stays isotropic on top of an
   anisotropic base (the car paint case).

### G-buffer

6. **The shading model id gets an anisotropy bit.** Bits 0-1 stay the layer (0 none, 1 clearcoat,
   2 sheen), bit 2 (`SHADING_MODEL_ANISOTROPY_BIT = 4`) marks an anisotropic base. `ShadingModel`
   stays the layer enum; the bit is added where the id is built.
7. **GB5.ba carries the anisotropy** for the lit and clearcoat layers: `.b` = the direction's angle
   in [0, pi) (the lobe is symmetric under T -> -T) divided by pi, `.a` = strength. The angle is
   measured in an orthonormal frame built from the shading normal alone (Duff et al. 2017), so the
   lighting pass rebuilds the same frame from the decoded normal. To make both sides agree to the
   bit, the geometry pass builds its frame from the normal round-tripped through the same half
   float octahedral encoding GB1 stores. 8 bits of angle are 0.7 degrees.
8. **Sheen keeps GB5 to itself.** A sheen material with anisotropy renders isotropic; import and
   the editor say so, as they do for coat plus sheen.
9. **The forward path** builds the anisotropic parameters directly from the material and the
   tangent frame, with no encoding.

### Layer textures

10. **Five material set bindings appended**, 13 to 17: clearcoat (R), clearcoat roughness (G),
    sheen colour (RGB, sRGB), sheen roughness (A), anisotropy (RGB, linear). Each multiplies its
    factor, as glTF defines. Absent, they bind white textures, except anisotropy, which binds
    (1, 0.5, 1): direction +X, full strength, so the factors alone apply. Only the primary layer
    has them; the blend graph's layer B does not.
11. **Texture usages**: sheen colour is `Color` (sRGB); the other four are `Data`.
12. **`clearcoatNormalTexture` stays unsupported** (GB5 has no room for a second normal); import
    keeps warning about it and the coat keeps the geometric normal.
13. **Paths are stored like the other maps**: `ModelMaterialData` and `ModelImportedMaterialInfo`
    gain `clearcoatTexturePath`, `clearcoatRoughnessTexturePath`, `sheenColorTexturePath`,
    `sheenRoughnessTexturePath`, `anisotropyTexturePath`; sidecar keys `clearcoat_texture_path`,
    `clearcoat_roughness_texture_path`, `sheen_color_texture_path`, `sheen_roughness_texture_path`,
    `anisotropy_texture_path`. The material panel lists them with the other texture rows.
14. **Factors**: `MaterialPbrSurfaceSettings` gains `anisotropyStrength` and `anisotropyRotation`
    (sidecar `anisotropy_strength`, `anisotropy_rotation`, editor sliders; rotation in degrees in the
    UI, radians stored as glTF has it). `GpuMaterialData` gains an eighth vec4, `anisotropyFactors`
    (x strength, y cos rotation, z sin rotation), 128 bytes.
15. **A texture can switch a layer on**: a material whose coat factor is above zero is coated, as
    now; a coat texture that is black everywhere is simply a coat of zero there. The shading model
    is still chosen per material from the factors.

## Automated Verification

- Shared GLSL (`anisotropy_common.glsl`, compiled into a C++ test): the anisotropic `D` integrates
  `D (N.H)` to 1 over the hemisphere for several `alpha_t`, `alpha_b`; it equals the isotropic GGX
  when `alpha_t = alpha_b`; the frame is orthonormal for random normals; the angle encoding
  round-trips within half a step, including across the frame's seam; the direction built from a
  tangent-space vector and rotation matches the glTF definition.
- Import: anisotropy factors read, clamped and defaulted; each layer texture resolves to a path;
  `clearcoatNormalTexture` still warns; sidecar round trip and a legacy sidecar without the keys.
- `static_assert`s: `GpuMaterialData` 128 bytes, `anisotropyFactors` at 112.

## Manual Acceptance (by image)

1. A throwaway glTF with spheres of rising anisotropy strength under a point light and the sky:
   highlights stretch across the tangent direction as strength rises; strength 0 matches the
   isotropic sphere.
2. A sphere with a striped clearcoat texture shows coated and uncoated stripes; a sheen colour
   texture tints the sheen.
3. Sponza (no extension materials): within the run-to-run noise of the previous build.

## Out of Scope

- `clearcoatNormalTexture`, anisotropic SSR, anisotropic sheen, textures on layer B, KHR_texture_transform
  on the new maps, texCoord sets other than 0 (as for the existing maps).
