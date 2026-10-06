# Assetto Corsa Light Scale: ksDiffuse, ksAmbient, Chrome and Emissive

## Goal

The kn5 import ignored `ksDiffuse` and `ksAmbient` on every material. Kunos use them to set how
bright a surface is: the same cockpit bake (`INT_cockpit_OCC`) is leather at 0.15/0.2, rear trim
at 0.79/0.88, and rubber at 0.05/0.04. The headlight chrome is `ksDiffuse` 0.01 and nothing but
reflection. Faults that followed:

- interior fabric, leather and carpet read pale and flat, all at one brightness;
- the rims read light silver; the game draws them dark gunmetal (ksDiffuse 0.38 on a dark detail);
- the chrome reflectors in the headlights rendered as their dark diffuse texture;
- a stand-in x2 on every `txDetail` (paint colour and tiled detail) set the level of those
  materials only. It is not in Kunos' shaders; plain materials (tyres, lamps, trim) had no gain at
  all, so they sat a stop below the paint whatever their ksDiffuse;
- the MFD screen's `ksEmissive` (a colour) was lost.

Split with the car paint correctness work (docs/design/2026-10-06-car-paint-correctness-design.md):
that change owns the specular, clear coat, roughness, txMaps and flakes. This one owns the diffuse
level, metal-like reflectors and emissive.

## What AC computes

Read from Kunos' compiled shaders (`assettocorsa/system/shaders/win/*_ps.fxo`, plain DXBC,
disassembled with d3dcompiler_47's `D3DDisassemble`). All `ksPerPixel*` variants share:

```
lit = tex * (ksLightColor * ksDiffuse * sat(N.L) * shadow
           + ksAmbientColor * ksAmbient * sat((N.y * 0.5 + 0.5) * 0.5 + 0.5)
           + ksEmissive)                      // ksEmissive is a float3
    + ksLightColor * shadow * (specular lobes)
```

A MultiMap's detail multiplies the diffuse, `tex = lerp(diffuse, diffuse * detail, 1 - diffuse.a)`,
with no doubling. The reflection is `F = min(fresnelC + (1 - N.V)^fresnelEXP, fresnelMaxLevel)`
times `cube * txMaps.B`, blended with `lerp(lit, env, F)` for `isAdditive` 0 and 2 and added for 1.

Vanilla AC does all of this in gamma space (CSP's linear colour space notes; `ksPostToneMap` is a
plain exposure divide). Every amount above acts on sRGB-encoded values.

AC's clear-weather noon light (`content/weather/3_clear/colorCurves.ini`, HIGH): sun
(170, 160, 140) x 20 (luminance 12.6), ambient (105, 105, 105) x 11 (4.5).

## The diffuse gain

One albedo has to stand for both terms. Weighted by the light each term typically gets (sun times a
mean N.L of 0.5, ambient times a mean hemisphere factor of 0.75), the diffuse response is
`0.65 ksDiffuse + 0.35 ksAmbient`. As a gain in gamma space, anchored at a neutral surface:

```
gain = 2 x (0.65 ksDiffuse + 0.35 ksAmbient) / 0.5          (Kn5Importer::DiffuseGain)
```

### The anchor: 2 at ksDiffuse = ksAmbient = 0.5

AC's ratios between materials do not fix the overall level, only how AC's light units map to ours.
Two anchors were rendered against the game's own skin preview (`skins/00_bayside_blue/preview.jpg`):

| anchor | R34 paint gain | lit hood (sRGB) | look |
|---|---|---|---|
| AC preview (studio light) | - | 0.01, 0.10, 0.35 | vivid mid blue, dark gunmetal rims |
| A: 1 at 0.5 / 0.5 | 0.97 | 0.04, 0.13, 0.28 | navy paint, cockpit near black even with auto exposure |
| B: 2 at 0.5 / 0.5 | 1.93 | 0.03, 0.19, 0.49 | paint as before, closest to the preview overall |

The user chose B (2026-10-06). It is the level road-car paint was always imported at (the old x2 on
the detail colour), now applied to every material through its own ksDiffuse/ksAmbient. Cost, as
before: the brightest liveries (white, red, yellow) reach 1 and clip; A would not clip them, but the
previews show every livery at a near-constant fraction of its detail colour, which no single
outdoor anchor reproduces for all of them.

Why A is so dark inside: AC's ambient term is never shadowed (its occlusion is baked into
`INT_cockpit_OCC`), while this renderer occludes the cabin physically on top of that bake. Interior
materials are mostly ambient-lit, so their low ksDiffuse reads far darker here. Not fixed: that is a
lighting question (double occlusion), not a material one.

### Where it goes

| path | how the gain is applied |
|---|---|
| flat paint detail | the colour (undoubled) times gain, into `BakePaint` / the factor |
| tiled detail (cloth, leather, carpet) | `MINIENGINE_materials_detail_layers.intensity` = gain (was 2); the neutral layer is white |
| ksMultilayer | not changed |
| any other textured material, gain <= 1 | `baseColorFactor` = gain^2.2 |
| any other textured material, gain > 1 | the diffuse baked times gain (`<diffuse>_lit.png`), clamped per texel |

R34 examples: paint 1.93, rims 1.58, tyres 1.18, leather 0.67, fabric 0.68, rubber 0.19,
`INT_cockpit_LR` 3.29, under-chassis 0.

## Metal-like reflectors

AC's lerp makes a surface with a near-black diffuse and a large `F` a mirror-like metal. Linear
reflectance of that is `F^2.2` (out = F x env in gamma space). Per material, the cosine-weighted mean
of AC's `F` against the diffuse it keeps:

```
F_mean = integral of 2 F(mu) mu dmu,   metal when F_mean > gain   (Kn5Importer::MeanReflection)
```

On the R34 that is `EXT_Chrome_Light` (F_mean 0.68, gain 0.17) and `mirror` (0.48, 0.20); every
other material's diffuse dominates (glass and windows blend and are skipped). Such a material
becomes `metallicFactor` 1 with a grey base colour of `F_mean^2.2` and no base colour map, since AC's
reflection is the untinted cube map. Its roughness follows AC's cube-map level,
`6 x sat(1 - EXP / 8)` on isAdditive 2 and `6 x sat(1 - EXP / 255)` otherwise: level 0 (both R34
reflectors) is the sharp map, roughness 0.04.

The R34's exterior `mirror` comes out a dim metal (0.20): that is what its numbers give; in the game
the cockpit mirrors show a rendered view instead, which this engine does not do.

## Emissive

`ksEmissive` is a float3 in the kn5's `valueC`. The reader dropped it, so R34 `INT_MFD`
(2.6, 2.6, 2.6, scalar 0) imported dark. AC adds `tex x ksEmissive` in the same units as the lit
term. A neutral surface (ksDiffuse = ksAmbient = 0.5) under AC's noon light is
0.5 x 12.6 x 0.5 + 0.5 x 4.5 x 0.75 = 4.84 of those units and is drawn at gain 2; a white surface
under our daylight (120 klx sun, mean N.L 0.5, about 20 klx of sky) is about 25 500 cd/m^2. So:

```
emissive (cd/m^2) = diffuse x (ksEmissive x 2 / 4.84)^2.2 x 25 500
```

The R34's MFD (2.6) is about 29 900 cd/m^2: as bright as a sunlit white panel, which is how AC
draws it in daylight.

written as `emissiveTexture` = the diffuse, `emissiveFactor` = the normalised colour,
`KHR_materials_emissive_strength` = the peak.

## Scope

- Only cars get the gain and the metal reflectors (a lone kn5 beside `data.acd` or `data/car.ini`).
  Tracks keep the old x2 on details and no gain: Spa's materials sit at gain 0.4 to 0.7 by the same
  formula, and darkening every track was not asked for nor checked. Emissive applies to both.
- Not done: AC's lights (data/lights.ini) as emissive; the cabin's double occlusion (above).

## Verification

`tests/kn5_import_tests.cpp`: `RulesMatchTheConverter` (DiffuseGain, MeanReflection, undoubled
FlatDetailColor), `CarMaterialsTakeAcsLightScale` (factor, bake, metal, emissive from valueC, tiled
detail intensity and white neutral layer, paint). R34 renders: before / A / B against the skin
preview, front, headlight, cockpit with auto exposure.
