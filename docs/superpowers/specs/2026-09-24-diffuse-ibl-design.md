# Diffuse IBL Design

## Goal

Light surfaces with the sky. When the scene's environment is the atmosphere or an HDRI, the
ambient term becomes the sky's irradiance for the surface normal, as L2 spherical harmonics,
instead of a constant luminance, still attenuated by material occlusion and VBAO. Phase 3 of the
IBL roadmap in `docs/superpowers/specs/2026-09-23-float-textures-design.md`; phase 4 replaces the
interim specular term.

## Current State

- `ShadeSurface` (`shaders/vulkan/pbr_common.glsl`) computes the ambient term with
  `EvaluateUniformAmbient(N, V, albedo, metallic, roughness, ubo.ambientLuminance.rgb) * ao`: a
  uniform environment, diffuse and specular lobes from Karis' environment BRDF fit.
- `ambientLuminance` is the sum of the scene's Ambient lights, or a fallback when there are none
  (`SceneLightSelection::usesFallbackAmbient`).
- Phase 2 gives every frame a sky: `EnvironmentMode` None / Atmosphere / Hdri, the sky-view and
  transmittance LUTs (set 0 bindings 3 and 4), the equirectangular HDRI (binding 6) with rotation and
  intensity in the environment UBO block, and `SampleSky` / `SampleEnvironmentMap` in
  `atmosphere_sampling.glsl`. The directional sun light is a separate light.
- `VulkanAtmosphere` records its compute work before the scene passes, with its own barriers.

## Locked Decisions

1. **Representation:** the sky's radiance projected onto the nine real SH basis functions up to
   band 2, RGB. Irradiance for a normal n is `E(n) = sum_l A_l sum_m L_lm Y_lm(n)` with
   A = (pi, 2pi/3, pi/4) (Ramamoorthi and Hanrahan 2001).
2. **Basis convention (Y up):** with a unit direction (x, y, z),
   `Y00 = 0.282095`; `Y1-1 = 0.488603 z`, `Y10 = 0.488603 y`, `Y11 = 0.488603 x`;
   `Y2-2 = 1.092548 x z`, `Y2-1 = 1.092548 z y`, `Y20 = 0.315392 (3 y^2 - 1)`,
   `Y21 = 1.092548 x y`, `Y22 = 0.546274 (x^2 - z^2)`. Index order: 0 Y00, 1 Y1-1, 2 Y10, 3 Y11,
   4 Y2-2, 5 Y2-1, 6 Y20, 7 Y21, 8 Y22. The same constants in C++ and GLSL.
3. **Atmosphere:** a compute shader projects the sky each frame, after the sky-view LUT, over a
   128 x 64 grid of directions uniform in (azimuth, cos zenith) (equal solid angle, 4 pi / 8192 each):
   one workgroup of 64 invocations, 128 directions each, a shared-memory reduction, nine vec4 into a
   device-lifetime storage buffer. The radiance sampled is `SampleSky` without the sun disk (the sun
   is a light already) above the horizon; below it, the sky-view in-scattering plus the ground
   lit by the transmitted sun, `albedo / pi * E_sun * T(ground, sun) * max(cos sun zenith, 0)`.
4. **HDRI:** projected on the CPU on the worker thread that decodes it, over every texel with its
   solid angle `(2 pi / W) (pi / H) sin(theta)`, in the map's own frame at intensity 1, with the
   direction for texel (u, v) following `SampleEnvironmentMap`'s mapping. Each frame the CPU
   rotates the coefficients about +Y by the scene's rotation (bands 1 and 2 rotate their (m, -m)
   pairs by m times the angle), scales them by the intensity, and uploads them in the environment
   UBO block.
5. **Where shaders read it:** set 0 binding 7, a storage buffer of nine vec4 (the atmosphere's),
   and nine vec4 appended to the environment UBO block (the HDRI's). `SkyShCoefficient(i)` picks
   by mode. Set 0 bindings 3 to 6 gain the compute stage so the projection shader can sample the
   sky.
6. **Shading:** in None mode nothing changes. In Atmosphere and Hdri modes the ambient term is
   `(diffuseAlbedo * E(N) + specularAlbedo * E(R)) / pi * ao`, with the lobes' albedos as in
   `EvaluateUniformAmbient` and R the reflection vector: the specular lobe sees the sky's cosine-blurred
   radiance until phase 4 prefilters it. The scene's Ambient lights still add their uniform
   luminance on top; the fallback ambient does not (`ambientLuminance.w` carries
   `usesFallbackAmbient`).
7. **Synchronisation:** the SH buffer is written by `VulkanAtmosphere::Record` (its fifth dispatch)
   and follows the LUTs' barriers; zero-filled on first use.

## Components

- `engine/renderer/spherical_harmonics.{h,cpp}` (engine_render_core, pure): `ShCoefficients`
  (9 x vec3), `EvaluateShBasis(dir)`, `ProjectEquirectangular(const FloatTextureData&)`,
  `RotateShAboutY(const ShCoefficients&, float radians)`, `EvaluateShIrradiance(const ShCoefficients&, dir)`.
- `atmosphere.{h,cpp}`: `EnvironmentUniformData` gains `hdriIrradianceSh[9]`;
  `BuildEnvironmentUniformData` takes the HDRI's unrotated SH.
- `shaders/vulkan/spherical_harmonics.glsl`, `atmosphere_irradiance.comp`; `pbr_common.glsl` ambient.
- `VulkanAtmosphere`: the SH buffer and the irradiance pipeline; `VulkanUniformBuffer`: binding 7.
- Renderer: HDRI SH computed with the decode; per-frame upload.

## Verification

- `miniengine.spherical_harmonics`: a constant map gives `E = pi L` in every direction; a map
  bright only in the upper hemisphere gives more irradiance up than down; rotating the SH equals
  projecting the rotated map; the basis constants match the closed-form ones; irradiance of a
  single-direction lobe peaks toward it.
- Existing suite, zero validation messages.
- Captures: cubes under the test HDRI take the colour of the sky they face (front white, sides
  green or blue, top yellow); cubes under the noon sky get bluish sky light on faces the sun does
  not reach; None mode is byte-identical to the phase 2 capture.
- GPU cost of the projection dispatch.

## Out of Scope

- Specular prefiltering, BRDF LUT and reflections (phase 4).
- Local light probes, sky occlusion beyond VBAO, and the sun's contribution to the SH.
