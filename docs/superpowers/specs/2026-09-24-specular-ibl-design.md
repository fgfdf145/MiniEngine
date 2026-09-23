# Specular IBL Design

## Goal

Reflect the sky. Under the atmosphere or an HDRI, the specular part of the ambient term becomes
the split-sum approximation (Karis 2013): a GGX-prefiltered cubemap of the sky, looked up along
the reflection vector at the surface's roughness, times the environment BRDF from a DFG lookup
table. Phase 4, the last of the IBL roadmap; it replaces phase 3's interim `E(R) / pi` and the
analytic environment BRDF fit in sky modes.

## Current State

- `EvaluateSkyAmbient` (`pbr_common.glsl`): diffuse `E(N) / pi` from SH9; specular `E(R) / pi`
  times Karis' analytic environment BRDF fit; times AO. None mode uses `EvaluateUniformAmbient`.
- `SampleSkyForLighting` (`atmosphere_sampling.glsl`): sky radiance without the sun disk, ground
  below the horizon; `SampleEnvironmentMap` for HDRIs. Set 0 bindings 3 to 6 are visible to
  compute.
- `VulkanAtmosphere` records its compute work (LUTs, SH) before the scene passes with its own
  barriers; images stay in `GENERAL`, shared by the frames in flight.

## Locked Decisions

1. **Capture:** every frame in Atmosphere and Hdri modes, a compute shader writes the sky's
   radiance (`SampleSkyForLighting` or `SampleEnvironmentMap`, no sun disk) into mip 0 of a
   128 x 128 `R16G16B16A16_SFLOAT` cube (the radiance cube), 2 x 2 samples per texel. Blits then
   build its mip chain down to 1 x 1: the source the prefilter samples.
2. **Prefilter:** a second cube, 128 x 128 with 6 mips (128 to 4), mip m for roughness
   `m / 5`. Mip 0 copies the radiance cube's mip 0 (a mirror); every other mip integrates 64 GGX
   importance samples with N = V = R, weighted by N.L, each read from the radiance cube at the mip
   whose texel solid angle matches the sample's (Karis 2013's pdf-based lookup):
   `lod = 0.5 log2((1 / (64 pdf)) / (4 pi / (6 * 128^2)))`, pdf = D / 4.
3. **Cube face convention:** Vulkan's: for face f and s, t in [-1, 1] (t down),
   +X (1, -t, -s), -X (-1, -t, s), +Y (s, 1, t), -Y (s, -1, -t), +Z (s, -t, 1), -Z (-s, -t, -1).
4. **Environment BRDF:** a 64 x 64 DFG table, x = N.V, y = roughness, (A, B) with specular albedo
   `F0 A + B`, integrated on the CPU at startup (512 GGX samples, Schlick-Smith
   visibility with k = alpha / 2, alpha = roughness^2, the remapping Karis uses for IBL and fits) by a pure, tested C++ function, uploaded
   once as RGBA32F. Lookups clamp to texel centres.
5. **Shading:** sky modes use `specularAlbedo = F0 A + B` from the table for both lobes'
   energy split, and specular radiance `textureLod(prefiltered, R, roughness * 5)`. Diffuse stays
   SH. The scene's Ambient lights keep adding their uniform luminance with the same albedos. None
   mode is unchanged.
6. **Resources:** a device-lifetime `VulkanEnvironmentProbe` owns both cubes, their views (cube
   views for sampling, 2D-array views per mip for storage) and two compute pipelines; records after
   `VulkanAtmosphere`, images in `GENERAL`, global memory barriers plus transfer barriers around
   the blits. Set 0 gains binding 8 (the prefiltered cube) and binding 9 (the BRDF table), fragment
   stage. In None mode nothing is recorded after the first frame's clears.

## Amendments During Implementation

- **DFG table oracle:** the planned comparison with Karis' analytic fit (within 0.05) failed across
  the whole grid, not only at grazing angles: the fit under-reflects near-mirrors seen head on by
  about 0.14 (0.86 against 0.99 at roughness 0.25). The integral matches exact values instead (a
  mirror is Schlick's Fresnel, `B = (1 - N.V)^5`), so the test rests on those, energy conservation,
  monotonic head-on albedo and 512-sample convergence; the fit is only a loose (0.2) same-family
  bound. The None path keeps the fit, unchanged.
- **Face orientation** was verified by drawing the HDRI sky from the prefiltered cube's mip 0:
  the four test rotations show the same colours as the direct lookup; mip 5 blends neighbouring
  directions smoothly.
- **Measured cost** (RTX 4070 Laptop, Release): capture plus prefilter about 0.07 ms per frame;
  the whole environment compute is 0.15 ms per frame under the atmosphere, 0.07 ms under an HDRI.

## Verification

- `miniengine.environment_brdf`: A + B = 1 and B = 0 for a mirror seen head on; A and B within
  0.05 of Karis' analytic fit over a grid; A + B never exceeds 1; the table's rows follow roughness.
- Captures: with the sky temporarily drawn from the prefiltered cube's mip 0, the sky matches the
  direct one (face orientation); from mip 5 it is a smooth blur; the test HDRI's colours land in
  the right directions on reflective surfaces. None mode within run-to-run noise.
- Zero validation messages; GPU cost of capture plus prefilter.

## Out of Scope

- Local reflection probes, parallax correction, screen-space reflections.
- Horizon or specular occlusion beyond AO.
- Caching the cubes when the sky has not changed.
