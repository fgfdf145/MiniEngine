# Detail Layers (Assetto Corsa Multilayer Surfaces)

## Goal

Draw Assetto Corsa track surfaces as the game does. Tarmac, grass, sand and kerbs use the
`ksMultilayer` shaders: a low-frequency base map, a mask whose four channels pick between four
tiling detail maps, and a detail normal map. The kn5 import keeps only the base map, so a track
reads as flat colour washes. Spa has 29 such materials, most of its ground.

## The reference

Custom Shaders Patch's recreations of the Kunos shaders (acc-shaders, `recreated/ksMultilayer_ps.fx`,
`ksMultilayer_fresnel_nm_ps.fx`, `ksMultilayer_objsp_ps.fx`):

```
diffuse  = txDiffuse(uv)
mask     = txMask(uv)
detail_c = txDetail_c(P * mult_c)                       for c in R, G, B, A
diffuse *= (detail_R * mask.r + detail_G * mask.g + detail_B * mask.b + detail_A * mask.a) * magicMult
```

`P` is the world position's `xz` for `ksMultilayer` and `ksMultilayer_fresnel_nm`, and the mesh
UV for `ksMultilayer_objsp`. `_fresnel_nm` adds `txDetailNM` sampled at `uv * detailNMMult`, an
ordinary tangent-space normal map. The combined alpha scales the specular. AC lights in gamma
space, so the products are of sRGB-encoded values.

## Decisions

1. **A glTF extension, `MINIENGINE_materials_detail_layers`**, on the material:
   ```json
   { "maskTexture": { "index": 3 },
     "layers": [ { "texture": { "index": 4 }, "scale": [ -0.8, -0.8 ] }, ... ],
     "mapping": "positionXZ",
     "intensity": 1.0 }
   ```
   Up to four layers, in mask channel order R, G, B, A. `mapping` is `"texCoord"` (the layer
   samples `TEXCOORD_0 * scale`) or `"positionXZ"` (the vertex position's `x` and `z` in the
   model's space, times `scale`). A layer without a texture samples white. The mask samples
   `TEXCOORD_0`, untransformed. Unknown to other viewers, which then show the base map, as now.
2. **Combined in gamma space.** The mask and the layers load as data (UNORM, every channel), are
   combined as AC does, times `intensity`, and the result, decoded from sRGB, multiplies the base
   colour. The base map is sampled sRGB as always; since `lin(a * b) = lin(a) * lin(b)` under a
   power curve, this equals AC's product decoded once.
3. **Engine data.** `MaterialDetailLayers` (in `material_graph.h`, next to the blend graph) on
   `ModelMaterialData` and `ModelImportedMaterialInfo`; the sidecar stores it under
   `detail_layers`. It is not part of the shader graph, so compiling a graph leaves it alone, as it
   leaves the KHR layer maps.
4. **GPU.** Set 1 gains bindings 27 (mask) and 28 to 31 (layers R to A), outside the per-slot
   transform and sampler arrays (default sampler: repeat, linear, mipmapped).
   `GpuMaterialData` grows to 18 vec4: `detailLayerScales[8]` (two vec4, a vec2 per layer) and
   `detailLayerParams` (x intensity). `shadingModel.z` is 0 for none, 1 for `texCoord`, 2 for
   `positionXZ`; with 0 no detail map is sampled. `triangle.vert` forwards the model-space
   position at location 10. `gbuffer.frag` and `triangle.frag` apply the layers to the base colour
   before the alpha test.
5. **kn5 import.** A material whose shader starts with `ksMultilayer` and has a `txMask` gets the
   extension: its four `txDetail` maps with `mult` as the scale, `magicMult` (default 1) as the
   intensity. Position mapping for `ksMultilayer` and `ksMultilayer_fresnel_nm*`, texCoord for
   `ksMultilayer_objsp*`. The model's space is AC's world turned half about Y, so for position
   mapping the scale is `-mult` on both axes, which samples exactly where AC does. `txDetailNM`
   with `detailNMMult > 0` becomes the normal texture with a `KHR_texture_transform` scale of
   `detailNMMult` (with 0, AC samples one texel: no useful relief, so none is bound).

## Out of scope

- The combined alpha as a specular mask, and `_fresnel_nm`'s view-dependent tarmac sheen: the
  import's roughness and `KHR_materials_specular` stay as they are.
- `ksMultilayer_fresnel_nm4`'s four normal maps and CSP's tiling fix.
- Ray-traced effects (DDGI, the reference path tracer) see the base map only.

## Automated Verification

- glTF: the extension read with both mappings, a missing layer, a missing mask (ignored); the
  sidecar round trip; a sidecar without the block.
- kn5: a `ksMultilayer_fresnel_nm` material imports with position mapping and negated scales,
  the detail normal with its transform; an `_objsp` one with texCoord mapping; a plain
  `ksPerPixel` material has no extension.
- `static_assert`s on `GpuMaterialData`'s size and the new members' offsets.

## Manual Acceptance (by image)

1. Spa from the grid: the tarmac shows its grain, the grass its blades, the kerbs their stripes,
   instead of flat washes.
2. Sponza and the test scenes: unchanged.

## Amendments During Implementation

- **The kn5 import writes the maps it references.** It used to write only the textures a
  material's `txDiffuse` and `txNormal` name; a multilayer material's mask, details and
  `txDetailNM` are now collected too, and the mask keeps its alpha (the fourth layer's weight).
- **Measured on Spa**: all 29 multilayer materials take the extension (52 more images). None sets
  `detailNMMult` above 0, so no detail normal is bound there; the tarmac shows its grain, the grass
  verges their blades.
- **Multilayer roughness has a floor (2026-09-29).** With the grain in place the tarmac still read as
  wet plastic with a sun glare. The import turned `ksSpecularEXP` 15 times `tarmacSpecularMultiplier`
  2.5 (the sheen's intensity, not a lobe width) into roughness 0.225, one value for the whole
  material, with no detail normal to break the reflection up. Now the multiplier no longer scales the
  exponent, and a `ksMultilayer*` material is imported at roughness 0.7 at the least (`sand`, `carpet`
  and the kerbs land on it; the tarmac's own 0.32 is raised to it). `fresnelMaxLevel` still becomes
  `KHR_materials_specular`. Measured looking down the start straight from behind a car at
  `240,20.7,610` (yaw 52.6, pitch -8.8), the sun glare at the frame's lower right: tone-mapped
  luminance 0.88 before, 0.28 after (0.09 with no specular at all). Still out of scope, and the next step for a road with a
  believable sheen: the combined alpha as a per-pixel specular mask, and a detail normal.
