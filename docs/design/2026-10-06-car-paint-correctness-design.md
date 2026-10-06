# Car Paint Correctness (kn5 Import and Coat Shading)

## Goal

An audit of the Skyline R34's paint (`EXT_Carpaint`, `ksPerPixelMultiMap_damage_dirt`) found the
clearcoat shading itself matches the Khronos sample viewer, but the kn5 -> glTF mapping and the
coat's environment term were wrong in six places. This change fixes them; the brightness scale
from `ksDiffuse` / `ksAmbient` and metal-like reflective trim are a parallel session's work.

## References

- Kunos' compiled shaders (`system/shaders/win/ksPerPixelMultiMap*_ps.fxo`, D3DDisassemble), read by
  the parallel session "AC导入车材质细化": the ground truth below.
- Content Manager's showroom port (gro-ove/actools, `AcTools.Render/Shaders/Includes/
  DarkMaterial.*.fx`, `Kn5MaterialDarkMaps.cs`): the same channel meanings, `isAdditive` 2 as
  "car paint".
- Walter et al. 2007: Blinn-Phong exponent n and Beckmann alpha, `alpha = sqrt(2 / (n + 2))`.

What Kunos' `ksPerPixelMultiMap` does (gamma space throughout):

```
diffuse.rgb = lerp(diffuse, diffuse * detail, 1 - diffuse.a)           // no x2
maps.r      = lerp(R, R * detail.a, 1 - diffuse.a)
lit = tex * (light * ksDiffuse * N.L + ambient * ksAmbient * hemi + ksEmissive)
    + light * [ksSpecular * R * pow(N.H, G * ksSpecularEXP + 1)
               + sunSpecular * G * B * pow(N.H, G * sunSpecularEXP + 1)]
F   = min(fresnelC + pow(1 - N.V, fresnelEXP), fresnelMaxLevel);  env = cube(R) * B
isAdditive 1: lit + F * env;   0 and 2: lerp(lit, env, F)
blur: LOD 6 * sat(1 - G * EXP / 255) (0, 1);  6 * sat(1 - G * EXP / 8) (2: mirror-sharp paint)
```

## Faults and fixes

| # | Fault | Fix |
|---|---|---|
| 1 | `SpecularExponentToRoughness` returned alpha, written as glTF perceptual roughness (the shader squares it): every kn5 material far too sharp; the coat repeated it. `ksSpecular` was folded into the exponent. | `roughness = (2 / (n + 2))^(1/4)`, clamped [0.04, 1]. The intensity is no longer a width: it is the specular level (below). The multilayer floor 0.7 stays. |
| 2 | txMaps: R folded into the exponent; G (gloss) and B (reflection) ignored. | Baked per pixel: G -> roughness at `G * EXP + 1`; R or B -> `specularTexture` (A). One RGBA map serves `metallicRoughnessTexture` (G, B = 0) and `specularTexture` (A). |
| 3 | `clearcoatFactor = sunSpecular / 20` (0.5 on the R34): face-on 0.02 against AC's `fresnelC * B` = 0.039; no mask; coats on calipers, rims and brushed metal. | Coat only on car paint (`isAdditive` 2 on a multi-map shader). Weight `fresnelC * B / 0.04` and roughness from `sunSpecularEXP * G + 1`, baked into one map (R weight, G roughness). |
| 4 | Metallic flakes dropped: the paint detail's alpha is flake noise on metallic liveries (std 35 on Bayside Blue, 0 on Active Red). `isAdditive` 2 not interpreted. | Car paint: the detail alpha scales the base's specular (Kunos: `maps.r *= detail.a`) through `specularColorTexture`, tiled by `detailUVMultiplier`; a flat alpha is `specularColorFactor`. `isAdditive` 2 is the coat (lerp, not tinted). |
| 5 | `FlatDetailTint` clamped `2 * detail` to 1 before the diffuse multiply: red, yellow (hue to lemon), white, Silica Breath lost brightness; the diffuse alpha mask was ignored. | `FlatDetailColor` keeps the colour unclamped; when it is above 1 or the diffuse alpha keeps the template anywhere, `<diffuse>_paint.png` = `diffuse * lerp(colour, 1, a)`, clamped only at the end. |
| 6 | The coat's ambient had no SSR and no specular/horizon occlusion (raw AO); SSR traced the base's normal and roughness. Paint's mirror lobe reflected sky where the road should be. | `SsrTracesCoat`: a coated pixel whose coat is at least as smooth as its base traces, resolves and applies the reflection on the coat (normal from the coat normal or geometric normal); the base then sees the occluded environment. `EvaluateCoatAmbient` uses `SpecularAmbientRadiance` (Lagarde AO at the coat roughness, horizon). |

### Specular level (KHR_materials_specular)

- Car paint: `ksSpecular`, mask R (the coat carries the reflection).
- Other surfaces with `fresnelMaxLevel` > 0: `fresnelMaxLevel` (caps the reflection), mask B.
- Surfaces without a reflection: `ksSpecular`, mask R. `ksSpecular` 0 (leaves, grass) reflects
  nothing.

## Decisions

1. **Physical conversion for roughness** (the user's call). AC's paint reflection is mirror-sharp
   (LOD from `G * EXP / 8`), its sun lobe exponent 2000: one GGX lobe cannot be both. The coat takes
   the sun lobe's roughness (0.18 on the R34), so reflections soften compared with AC.
2. **Coat weight is linear.** AC lerps in gamma space, so its face-on 0.039 is about 1 % of linear
   light; read literally that is a quarter of a coat. Lacquer is F0 0.04 at full coverage, and
   `fresnelC * B / 0.04` gives 0.96 on the paint, less in panel gaps (B low): kept.
3. **The detail's x2 stays for now.** Kunos' shader does not double the detail; the doubling
   stood in for AC's light scale. Removing it alone turns Bayside Blue navy; the parallel session
   replaces it with the `ksDiffuse` / `ksAmbient` gain calibrated from AC's skin previews. The
   paint bake takes any gain.
4. **Flakes weigh F0 only** (the colour slot): the 1x txMaps mask and the tiled flake need two
   maps with their own transforms. Real sparkle would need flake normals; out of scope.
5. **Non-paint sun lobes** (calipers' `sunSpecular` 1, rims 1, brushed metal 2) are dropped: they
   had coats of weight 0.05 to 0.1 before, too weak to see.

## Out of scope

- `ksDiffuse` / `ksAmbient`, the x2, metallic for chrome-like lerp materials, `ksEmissive` as a
  float3 (parallel session).
- Fresnel F0 of uncoated reflective surfaces (glass `fresnelC` 0.1 vs F0 0.024).
- The flake noise on patterned details (MAT_Carbon's alpha).

## Verification

- `kn5_import` tests: exponent 100 -> 0.374, 2000 -> 0.178; `FlatDetailColor` unclamped (148 ->
  1.161); fixture paint with the R34's figures: base map G 100 (exponent 81), A = txMaps R; coat map
  R 238 (`0.07 * 136/255 / 0.04`), G 45; specular level 0.6; solid livery alpha 69 ->
  `specularColorFactor` 0.271; metallic livery -> flake map tiled 40; grey template baked to 202
  (174 x 1.161); leaves specular 0; multilayer floor.
- `ssr` tests: `SsrTracesCoat` compiles as C++.
- R34 re-import and before/after captures (see the commit message).
