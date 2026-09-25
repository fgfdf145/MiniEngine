# Transmission and Volume (Phase 4a of the Complete BRDF Program)

## Goal

`KHR_materials_transmission` (light through a thin or refracting surface: glass, water, plastic) and
`KHR_materials_volume` (refraction through a closed mesh and absorption along the way, Beer-Lambert),
accepted against the Khronos Sample Viewer (`CompareTransmission`, `TransmissionTest`,
`TransmissionRoughnessTest`, `TransmissionThinwallTestGrid`, `CompareVolume`, `AttenuationTest`,
`DragonAttenuation`, `CompareIor`, `IORTestGrid`). Dispersion (4b) and diffuse transmission (4c)
build on the same refraction pass. See `2026-09-25-complete-brdf-program.md`.

## What the Specifications and the Viewer Do

- Transmission replaces the dielectric base's diffuse lobe by a microfacet BTDF tinted by the base
  colour: `base = mix(diffuse, specular_btdf * baseColor, transmission)`, under the same Fresnel
  mix as the diffuse. Metals transmit nothing. `alphaMode` stays coverage; transmissive materials are
  normally Opaque.
- Without volume (thickness 0) the surface is thin: no macroscopic refraction, roughness blurs what is
  behind. With volume, the view ray refracts by the IOR (`KHR_materials_ior`, 1.5 by default) and
  travels `thicknessFactor * thicknessTexture.g` (mesh units, scaled by the node) before leaving;
  transmittance is `attenuationColor ^ (distance / attenuationDistance)`.
- The viewer (`GltfSVApp.js`, 2026-09-26) renders opaque objects and the background into a 1024x1024
  HDR texture with mipmaps, then draws transmissive primitives back to front sampling it at the
  refracted exit point projected to the screen, at `lod = log2(1024) * roughness *
  clamp(2 * ior - 2, 0, 1)`; the result times the attenuation and the base colour replaces the image
  based diffuse, `mix(diffuse, refraction, transmission)`. Punctual lights add
  `baseColor * D * Vis` about the light mirrored through the surface, attenuated along the same ray.
  Transmissive surfaces do not see each other (the specification's minimum expectation).

## Decisions

1. **Transmissive surfaces leave the G-buffer.** A material with transmission > 0 gets
   `kShadingFlagTransmission` (128) and `kShadingFlagForward`. Unlike the other forward-shaded
   materials it is not drawn by the geometry pass: its pixels must show what is behind it in the
   copied scene. Draw order becomes deferred, forward-shaded, **transmissive**, Blend
   (`BuildMaterialDrawOrder`, `transmissiveDrawItemBegin`). They depth-test and depth-write, write no
   motion vectors (TAA reprojects them with the background's, as Blend surfaces), get no AO or SSR, and
   cast no shadows (glass casting a solid shadow is wrong more often than none, as for Blend).
2. **The scene copy.** A new pass, `VulkanTransmissionCopyPass`, runs after the forward pass's sky
   when the frame has transmissive draws: it blits the HDR target (pre-exposed, before any
   transmissive or Blend draw) into a fixed 1024x1024 RGBA16F image and builds its full mip chain,
   as the viewer does; a fixed size keeps the descriptor stable across resizes and the LOD rule equal
   to the viewer's. The forward pass is split: `Forward` (Opaque/Mask it owns, forward-shaded, sky)
   and `ForwardTranslucent` (transmissive back to front, then Blend back to front). Both scene pass
   orders gain the copy and the second pass; with no transmissive draws the copy is skipped and the
   image is unchanged.
3. **Sampling** (`transmission_common.glsl`, compiled into a C++ test like the other shared files):
   the refracted exit point `position + normalize(refract(-V, N, 1 / ior)) * thickness * nodeScale`
   (thin: `position`), projected with the frame's unjittered view-projection, sampled at the viewer's
   LOD; `ApplyVolumeAttenuation` and the base colour after. The copy is bound in set 0 (binding 18,
   linear, clamp to edge, all mips).
4. **Shading** rides in `SpecularParams`, as iridescence does, so no shading signature changes:
   `transmissionFactor`, the sampled `transmittedRadiance` (IBL, already attenuated and tinted), and
   for punctual lights the attenuation-and-tint and the IOR-scaled roughness. The image-based diffuse
   becomes `mix(diffuse, (1 - metallic) * (1 - specularAlbedo) * transmittedRadiance, transmission)`;
   each direct light's diffuse becomes `mix(diffuse, kD * baseColor * attenuation * D * Vis(mirrored L)
   * radiance, transmission)`. AO does not darken the transmitted light. The specular lobe is
   unchanged.
5. **Data**: `GpuMaterialData` gains `transmissionFactors` (transmission, thickness, attenuation
   distance, dispersion reserved for 4b) and `attenuationColor`; the material set gains bindings 23
   (transmission, R) and 24 (thickness, G). Import, sidecar (`transmission_factor`, `thickness_factor`,
   `attenuation_distance`, `attenuation_color`, the two texture paths and their slots' transforms and
   samplers) and the material panel follow the other extensions. An infinite attenuation distance is
   stored as 0 (no absorption), as the viewer treats it.

## Risks

- The device reports 16 samplers per stage and the pipelines already bind over 30 combined image
  samplers; MoltenVK's argument buffers allow it. Three more (two material slots, the copy) follow the
  same path; if pipeline creation ever fails on a device, device selection must learn the real limit.

## Automated Verification

- `transmission_common.glsl` in C++: a thin surface's exit is the position; a volume's exit moves
  along the refracted ray by thickness times node scale; at normal incidence it goes straight in;
  the LOD rule (0 for roughness 0 or IOR 1, `log2(1024)` for roughness 1 and IOR >= 1.5);
  attenuation (colour ^ (d / D); distance 0 and attenuation distance 0 leave radiance unchanged).
- Import: factors, both textures, defaults, a volume without transmission; sidecar round trip;
  legacy sidecar keeps the defaults.
- `BuildMaterialDrawOrder`: transmissive items after the forward-shaded ones and before Blend, back to
  front among themselves; the scene pass orders include the copy and the translucent pass.

## Manual Acceptance (by image)

1. The transmission and volume scenes against the Sample Viewer: `CompareTransmission`,
   `TransmissionRoughnessTest` (blur grows with roughness), `TransmissionThinwallTestGrid`,
   `CompareVolume` and `AttenuationTest` (colour deepens with thickness), `DragonAttenuation`,
   `CompareIor` and `IORTestGrid` (refraction grows with IOR). Crops side by side, not only the mean.
2. The other comparison scenes and Sponza unchanged (no transmissive material: the copy never runs).
