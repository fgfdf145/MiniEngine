# Dispersion and Diffuse Transmission (Phases 4b and 4c of the Complete BRDF Program)

## Goal

`KHR_materials_dispersion` (the refraction of 4a split by wavelength: coloured fringes in glass and
gems) and `KHR_materials_diffuse_transmission` (light scattered through thin translucent material:
leaves, paper, lamp shades, porcelain), accepted against the Khronos Sample Viewer
(`CompareDispersion`, `DispersionTest`, `DragonDispersion`, `DiffuseTransmissionTest`,
`DiffuseTransmissionTeacup`). Diffuse transmission is a Release Candidate, not ratified, but the viewer
implements it. See `2026-09-25-complete-brdf-program.md` and `2026-09-26-transmission-volume-design.md`.

## What the Specifications and the Viewer Do

Read from the viewer release the captures come from (`GltfSVApp.js`, 2026-09-26).

- **Dispersion** spreads the IOR over the three channels, `halfSpread = (ior - 1) * 0.025 * dispersion`,
  IORs `(ior - halfSpread, ior, ior + halfSpread)` for red, green and blue; the image-based refraction
  is traced once per channel (exit point and copy LOD with that channel's IOR) and each sample gives
  its own channel. The viewer attenuates all three with the blue ray's length (marked TODO in its
  source). Punctual lights' transmission lobe is not dispersed. Dispersion only shows through a volume:
  a thin wall does not refract.
- **Diffuse transmission** (`diffuseTransmissionFactor` times the texture's A,
  `diffuseTransmissionColorFactor` times the colour texture's RGB, sRGB) replaces the diffuse lobe by a
  Lambertian lobe through the surface by the factor:
  - image-based: `mix(irradiance(n) * baseColor, irradiance(-n) * dtColor, factor)`, attenuated with
    `KHR_materials_volume` by the thickness times the node's mean scale;
  - punctual: the front diffuse times `1 - factor`; for a light behind the surface (`n.l < 0`) add
    `factor * E * |n.l| * dtColor / pi`, attenuated alike, and weight the dielectric mix by the Fresnel
    about the light mirrored through the surface;
  - then specular transmission (4a) mixes over the result, then the dielectric Fresnel mix.
- `DiffuseTransmissionTest` lights its panels from behind with its own directional light
  (`KHR_lights_punctual`, 1 lux, travelling +Z, toward the viewer's camera). The viewer draws no shadows.

## Decisions

1. **Data.** `transmissionFactors.w` (reserved in 4a) holds the dispersion. `GpuMaterialData` gains
   `diffuseTransmission` (rgb the colour factor, a the factor): 14 x vec4, 224 bytes. The material set
   gains slots 25 (diffuse transmission, A) and 26 (diffuse transmission colour, RGB, sRGB), with
   their transforms and samplers (`kMaterialTextureSlotCount` 27). Import, sidecar
   (`dispersion`, `diffuse_transmission_factor`, `diffuse_transmission_color`, the two texture paths)
   and the material panel follow the other extensions.
2. **Routing.** A material with diffuse transmission (factor > 0) is forward shaded
   (`kShadingFlagForward`), like iridescence: still in the G-buffer for depth, motion and AO, shaded by
   the forward pass. No new flag bit (GB2.a keeps flags below 256): the forward pass reads the factor.
   It does not need the transmission copy.
3. **Dispersion** (`transmission_common.glsl`, tested in C++): `DispersedIors(ior, dispersion)`;
   `triangle.frag` traces the exit point, projection and LOD per channel when the dispersion is above 0,
   one lookup otherwise (no change for 4a's materials). Each channel is attenuated by its own ray's
   length (the viewer's blue-for-all is its own shortcut; the difference is below a percent of the
   distance).
4. **Diffuse transmission shading** rides in `SpecularParams` as transmission does:
   `diffuseTransmissionFactor` and `diffuseTransmissionColor` (colour times the volume attenuation).
   The image-based diffuse becomes `mix(diffuse, (1 - metallic) * (1 - specularAlbedo) * dtColor *
   irradiance(-N), factor)` before 4a's specular transmission mix; the AO does not darken the
   transmitted part. Each direct light's diffuse becomes `(1 - factor) * diffuse + factor *
   kD(mirrored) * dtColor / pi * |N.L| * radiance` when the light is behind. Area lights add no back
   lobe (as for 4a).
5. **Shadows from behind.** The shadow lookups offset the receiver along the geometric normal, away
   from a light behind the surface, which puts a lit-from-behind leaf in its own shadow. For a surface
   that transmits (either extension) and a light behind its geometric normal, the offset goes the
   other way (`-geoNormal`): the thin wall passes the light, other casters still block it. A thick
   volume is still shadowed by its own far side (accepted: the transmission is a thin-wall model).
6. **The comparison's lights.** `make_scenes.py` turns a model's `KHR_lights_punctual` directional
   lights into scene lights (intensity in lux as glTF's, direction from the node, no shadows since the
   viewer has none). Point and spot lights are not needed by any compared model and are reported.

## Automated Verification

- `DispersedIors`: dispersion 0 gives the IOR three times; the spread is `(ior - 1) * 0.025 *
  dispersion` either side; IOR 1 does not spread.
- Import: dispersion; the diffuse transmission factors, both textures and defaults; the forward flag;
  sidecar round trip; legacy sidecar keeps the defaults.
- The light direction conversion in `make_scenes.py`: glTF's -Z through the node matrix, then the
  engine's Euler angles whose rotation of -Y gives it.

## Manual Acceptance (by image)

1. `DiffuseTransmissionTest` and `DiffuseTransmissionTeacup` against the viewer: panels brighten
   and take the transmission colour with the factor, the textures' stripes and logos show through.
2. `CompareDispersion`, `DispersionTest` and `DragonDispersion`: colour fringes grow with the
   dispersion and the IOR, crops side by side.
3. Every other comparison scene unchanged within the noise (their materials have neither extension).
