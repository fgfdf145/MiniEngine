# Tiled Detail Maps on ksPerPixelMultiMap (Car Interiors)

## Goal

The Skyline R34's cockpit imported without its materials: carbon, leather, cloth, carpet and the
headliner all read as one grey AO bake, and the steering-wheel rim, the top of the dashboard and
the door leather were missing altogether.

## What the kn5 says

Kunos paint an interior as one shared AO/colour bake (`INT_cockpit_OCC`, 1024² A8L8) and give each
surface its look with a tiling detail map:

| material | shader | txDetail | detailUVMultiplier | txNormalDetail | detailNormalBlend |
|---|---|---|---|---|---|
| Leather | ksPerPixelMultiMap_AT_NMDetail (alpha-tested) | leather | 37 | leather_nm | 0.7 |
| Headliner | ksPerPixelMultiMap_AT_NMDetail (alpha-tested) | roof | 30 | roof_NRM | 3 |
| Fabric / Fabric_P2 | ksPerPixelMultiMap_NMDetail | cloth | 100 / 20 | cloth_NRM | 1 / 0.6 |
| Fabric_P1 (seat centre) | ksPerPixelMultiMap_NMDetail | cloth_perforated | 650 | cloth_perforated_nrm | 3 |
| Carpet, INT_cockpit_LR | ksPerPixelMultiMap_NMDetail | carpet | 38 / 30 | Carpet_NM | 1 |
| EXT_carbon | ksPerPixelMultiMap_NMDetail | MAT_Carbon | 400 | MAT_Carbon_NM | 0.3 |
| Plastico, Metal_Brushed | ksPerPixelMultiMap | INT_Plastic, brushed_metal | 70, 50 | - | - |

The import used `txDetail` only when it is one flat colour (Kunos road-car paint, see
`FlatDetailTint`). A patterned detail and `txNormalDetail` were dropped.

Second fault: on MultiMap shaders the diffuse alpha is the detail mask, not coverage. The cockpit
bake's alpha is 0 on every texel (detail everywhere). Leather and Headliner are flagged alpha-tested,
the import made them `MASK` at 0.5, and every fragment was discarded.

## AC's combination

`ksPerPixelMultiMap`: the detail, sampled at `uv * detailUVMultiplier`, multiplies the diffuse where
the diffuse's alpha is 0 and leaves it alone where it is 1. A detail map is neutral at mid-grey, so
the product is doubled (the same doubling `FlatDetailTint` applies); AC works in gamma space.

```
diffuse.rgb *= lerp(2 * detail.rgb, 1, diffuse.a)
```

The `_NMDetail` variants add `txNormalDetail`, tiled the same way, weighted by `detailNormalBlend`.

## Decisions

1. **Reuse `MINIENGINE_materials_detail_layers`** (2026-09-28 design), no renderer change, `texCoord`
   mapping, `intensity` 2:
   - layer R: `txDetail`, scale `detailUVMultiplier`;
   - layer G: a one-texel mid-grey (128) map, only when some diffuse texel's alpha is above 0;
   - a baked mask: R = 1 - diffuse alpha, G = diffuse alpha, B = A = 0. One texel when the alpha is
     uniform (the Skyline's cockpit bake), full size otherwise (`Skin_00`, `INT_LR`).
   `2 * (detail * (1 - a) + 0.502 * a)` is AC's lerp to within 0.4 %.
2. **Detail normal** as the material's normal texture, `KHR_texture_transform` scale
   `detailUVMultiplier`, glTF `scale` = `detailNormalBlend`, when the material's own `txNormal` is
   flat (`flat_nm`) or absent and the blend is above 0. A real base normal map (INT_cockpit_LR's) is
   kept; the engine has one normal slot.
3. **An alpha test that discards every texel is dropped.** An alpha-tested material is `MASK` only if
   its diffuse's highest alpha reaches the cutoff. Real cutouts (grilles, decals, seams) keep it.
4. **When it is decided.** The first import pass reads only the kn5 tables (no texels), so it marks
   every `useDetail` material's `txDetail`/`txNormalDetail` as used; the texture pass, which has the
   texels, skips a detail that is a flat colour and is not sampled as anything else. Alpha ranges are
   never cached from a table-only read.

## Out of scope

- `ksDiffuse` / `ksAmbient`: AC's leather has `ksDiffuse` 0.15 and renders darker than the bake; the
  import ignores these for every material (unchanged).
- The detail's alpha (spec/gloss mask on MAT_Carbon) and the diffuse alpha masking the detail normal.
- `normalUVMultiplier` (AT_NMDetail; it tiles the base `txNormal`, which is flat here).
- Ray-traced paths see the base map only, as for multilayer.

## Verification

- `ImportsMultiMapDetailAsDetailLayers` (tests/kn5_import_tests.cpp): alpha-0 leather is opaque with
  one layer at 37, intensity 2, a one-texel mask (255, 0, 0, 0), detail normal scale 0.7 tiled 37;
  a half-alpha diffuse gives the neutral layer and a full-size mask; a real cutout keeps `MASK`; a
  flat detail stays a tint.
- R34 re-import: 76 images (was 58); 11 materials take the extension; MASK only on Caliper,
  INT_Grid_Round, INT_DEcals, INT_Seams, INT_MFD, INT_Grid. Cockpit renders before/after: the
  wheel rim, dashboard top and door leather are back; seat centres show the perforated cloth,
  the floor the carpet, the door cards the cloth.
