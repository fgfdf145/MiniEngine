# G-Buffer Layers, IOR and Specular (Phase 2 of the Complete BRDF Program)

## Goal

Let every common material extension coexist on one deferred pixel, and add the two the base still
lacks (see `2026-09-25-complete-brdf-program.md`):

- `KHR_materials_ior` and `KHR_materials_specular`: the dielectric F0 from the index of refraction,
  tinted and scaled by the specular colour and strength, and F90 scaled with it.
- `clearcoatNormalTexture`: the coat's own normal map (orange peel over a smooth base, or a smooth
  coat over a bumpy one).
- Clearcoat and sheen on one pixel, and anisotropy beside either.

Materials that use none of them render as before. The user accepts by the rendered image.

## Current State

- Six colour attachments: GB0 albedo (RGBA8 sRGB, a = 1), GB1 normals (RGBA16F), GB2 surface
  (metallic, roughness, AO, shading model id), GB3 emissive, velocity (RG16F), GB5 custom (RGBA8:
  coat in `.rg` and anisotropy in `.ba`, or sheen in all four). Coat and sheen exclude each other.
- F0 is `mix(0.04, albedo, metallic)` wherever the shader needs it, F90 is 1.
- MoltenVK on Apple GPUs offers 8 colour attachments, the most the layout can use.

## Decisions

1. **Eight colour attachments, fixed meanings.**

   | Target | Format | Channels |
   | --- | --- | --- |
   | GB0 albedo | RGBA8 sRGB | rgb albedo; a reserved (subsurface, phase 5) |
   | GB1 normals | RGBA16F | shading and geometric normal, octahedral (unchanged) |
   | GB2 surface | RGBA8 | metallic, roughness, AO, shading flags |
   | GB3 emissive | B10G11R11 | unchanged |
   | velocity | **RGBA16F** | rg motion; **ba the coat normal**, octahedral |
   | GB5 specular | RGBA8 | rgb `sqrt(dielectric F0)`, a dielectric F90 |
   | **GB6 coat** | RGBA8 | coat factor, coat roughness, anisotropy angle, anisotropy strength |
   | **GB7 sheen** | RGBA8 | sheen colour, sheen roughness |

   The square root puts the common F0 values (0.02-0.08) on 36-72 of 255 instead of 5-20;
   `sqrt(0.04) = 0.2` is exactly 51 / 255. The engine fails at start-up with a clear message on a
   device that offers fewer than 8 colour attachments.
2. **The shading model id becomes flags** in GB2.a: 1 clearcoat, 2 sheen, 4 anisotropy, 8 custom
   specular (GB5), 16 coat normal (velocity.ba). The lighting pass reads only the targets the flags
   name, so a plain pixel (0) reads nothing new and shades exactly as before. `ShadingModel` becomes
   these bits; coat and sheen may both be set.
3. **Dielectric F0 and F90** (Khronos): `F0 = min(((ior - 1) / (ior + 1))^2 * specularColor, 1) *
   specular`, `F90 = specular`, with `specular` and `specularColor` the factors times their maps
   (`specularTexture.a`, `specularColorTexture.rgb`, sRGB). `ior = 0` stands for an infinite index
   (F0 = 1). Metals keep `F0 = albedo`, `F90 = 1`; the surface's values are `mix(dielectric, metal,
   metallic)`. Schlick's Fresnel gains F90, the split sum becomes `F0 A + F90 B`, the LTC weight
   `F0 norm + (F90 - F0) fresnel`, and the diffuse weight `1 - F` uses the new F. The coat keeps
   IOR 1.5 as glTF defines it.
4. **The coat's normal** is the geometric tangent frame applied to `clearcoatNormalTexture`, scaled
   by its `scale`. Without the map it is the geometric normal, as now, and the coat-normal flag is
   not set.
5. **Coat and sheen together**: the sheen sits on the base and the coat on top of both, as the
   Khronos sample viewer layers them: `base' = base * (1 - sheen albedo) + sheen`, then
   `base'' = base' * (1 - coat F) + coat`.
6. **Material data**: `GpuMaterialData` gains a ninth vec4, `specularFactors` (rgb = the IOR's F0
   times the specular colour factor, a = specular factor), 144 bytes; `clearcoatFactors.z` carries
   the coat normal scale. The material set gains bindings 18 specular (A), 19 specular colour (RGB,
   sRGB), 20 coat normal; absent maps bind white, white and the flat normal.
7. **Import, sidecar, editor**: `ior` (default 1.5), `specularFactor` (1), `specularColorFactor`
   (1, 1, 1) and `clearcoatNormalScale` (1) in `MaterialPbrSurfaceSettings`, sidecar keys `ior`,
   `specular_factor`, `specular_color_factor`, `clearcoat_normal_scale`; the three texture paths
   `specular_texture_path`, `specular_color_texture_path`, `clearcoat_normal_texture_path`. The
   material panel gets IOR, Specular and Specular Color, and lists the maps.

## Automated Verification

- The dielectric F0 formula: 1.5 gives 0.04, 1.0 gives 0, 0 gives 1, the colour tints, the
  specular factor scales F0 and F90 alike, and F0 is clamped to 1 before the factor.
- The square-root encoding round-trips 0.04 exactly and every F0 within half an 8-bit step of
  its square root.
- Import of both extensions (factors, defaults, clamps, textures) and of the coat normal texture
  with its scale; sidecar round trip; legacy sidecars read the defaults.
- `static_assert`s: `GpuMaterialData` 144 bytes.
- Pass order and target formats: the geometry pass writes eight colour attachments.

## Manual Acceptance (by image)

1. Spheres of IOR 1.33, 1.5 and 2.4: reflections grow stronger; a tinted specular colour tints the
   reflection of a dielectric; specular 0 removes it.
2. A coat with a bumpy normal map over a smooth base: the coat's reflections wobble, the base's do
   not; and the reverse.
3. A sphere with both coat and sheen shows both; deferred and forward agree.
4. Sponza: within the run-to-run noise of the previous build.

## Out of Scope

- Transmission and volume (phase 4) also read the IOR; this phase only imports and stores it.
