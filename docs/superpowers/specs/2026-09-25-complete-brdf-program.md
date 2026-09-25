# Complete BRDF Program

## Goal

The full glTF PBR material model (every `KHR_materials_*` extension the Khronos sample viewer
renders) plus the accuracy fixes the analytic lighting still lacks, aimed at the look of GT7's
materials. Agreed with the user on 2026-09-25. Each phase gets its own spec, tests and image
acceptance, as every feature so far.

## What GT7 Says About Its Materials

From `s2025_PBS_Physically_Based_Tone_Mapping_GT7.pdf` (pages 21-25) and
`SIGGRAPH2023_RenderingTechBehindGT7.pdf` (pages 60-61):

- The BRDF is a slight extension of the Disney Principled BRDF (Burley 2012), described in Hirai and
  Takano 2022 (CEDEC, Japanese; not available here).
- Car paints are measured (Mini-Diff V2 for dense RGB BRDFs, X-Rite MA-T12 for spectral colour) and
  fitted to the shader's parameters with Mitsuba 3 as a differentiable renderer, reconstructed in
  Rec. 2020. Over 5000 measured paint colours, many outside Rec. 709.
- A forward renderer, chosen for complex materials: car metallic flakes, special road and grass
  shaders.

## Architecture

- **Common materials stay deferred** on a wider G-buffer: the base, `ior`, `specular`, clearcoat
  (with its normal map), sheen, anisotropy, and every combination of them on one pixel.
- **Rare, expensive materials go forward**, as Blend does: iridescence, transmission, volume,
  dispersion, diffuse transmission, car paint flakes. The forward path already shades through the
  same `ShadeSurface`; these materials get a flag that routes their draws there. They get no
  screen-space reflections, as Blend surfaces do not.

## Phases

1. **Accuracy of the analytic lighting.** Height-correlated Smith for direct lights (matching the
   DFG table and the anisotropic lobe); Burley diffuse, Disney's diffuse; horizon specular
   occlusion for normal-mapped reflections; the sun as a disk and point and spot lights with a
   source radius (representative point with energy normalisation); area lights through linearly
   transformed cosines (Heitz 2016) with a table fitted here.
2. **G-buffer layout and the common extensions.** `KHR_materials_ior`, `KHR_materials_specular`
   (F0 strength and colour, dielectric only), clearcoat normal texture, clearcoat and sheen on one
   pixel, all in a redesigned G-buffer.
3. **The forward material path and iridescence.** Routing by material flag;
   `KHR_materials_iridescence` (thin-film interference, Belcour and Barla 2017).
3b. **Material variants.** `KHR_materials_variants` (added 2026-09-26): a model carries several
   material bindings per primitive and one is chosen by name at load or in the editor (a car's
   colour options). Import and selection only; no shader change.
4. **Transmission.** `KHR_materials_transmission` (rough refraction from a mip chain of the opaque
   scene colour), `KHR_materials_volume` (Beer-Lambert through thickness), `KHR_materials_dispersion`,
   `KHR_materials_diffuse_transmission`.
5. **Subsurface scattering** (screen-space diffusion; glTF has no ratified
   extension, the draft `KHR_materials_subsurface` is followed where it helps).

## Deferred

- **Car paint flakes** (GT7's paint; not a glTF extension, an engine material parameter set in the
  editor and the sidecar). Deferred by the user on 2026-09-26 until the Khronos material model is
  complete: the standard extensions come first.

## What Cannot Be Done Here

- **The measured paints.** GT7's paint parameters come from measuring physical samples with
  dedicated instruments and fitting them; neither the samples, the instruments nor the fitted data
  are available. The paint model can be built; its parameters will be authored, not measured.
- **Hirai and Takano's extension of the Disney BRDF.** Not in the material at hand; the Disney
  model itself is followed.
- **Wide-gamut rendering (Rec. 2020 working space).** Doable, but it is a colour pipeline change,
  not a BRDF one (textures, lights, sky and tone mapping all change space). Out of this program;
  worth its own design. HDR10 output already exists.
- **Ray-traced reflections**, which GT7 uses in photo mode, need ray queries, which MoltenVK on this
  Mac does not provide. SSR stays the substitute.
- **GT7's road and grass shaders** are not described in the material at hand.
