# BRDF Accuracy (Phase 1a of the Complete BRDF Program)

## Goal

Fix what the analytic lighting gets wrong or leaves out before new material features build on it
(see `2026-09-25-complete-brdf-program.md`). Every lit image changes a little; the user accepts
by the rendered image.

## Current State

- Direct lights use GGX with Smith-Schlick `G` at `k = (roughness + 1)^2 / 8` (UE4's "hotness"
  remap). The DFG table integrates Smith-Schlick at `k = alpha / 2`. The anisotropic lobe uses the
  height-correlated Smith visibility. Three different geometry terms.
- Diffuse is Lambert weighted by `(1 - F(H.V)) (1 - metallic)`.
- A normal-mapped surface can reflect the environment from below its geometric horizon, where the
  surface itself would block it: those reflections read as a glow along the edges of bumps.
- Directional lights are points in the sky, point and spot lights points in space: a smooth surface
  shows a one-pixel highlight however large the sun or lamp is. The sun's angular diameter already
  reaches the shaders (`sunIlluminance.w = cos(sun angular radius)`, 0.545 degrees by default).

## Decisions

1. **One geometry term: height-correlated Smith** (Heitz 2014), `V = 0.5 / (N.L sqrt(N.V^2 (1 - a^2) + a^2)
   + N.V sqrt(N.L^2 (1 - a^2) + a^2))`, `a = roughness^2`, for the base, the coat and area lights,
   and in the DFG table. The table and the direct lights then agree, so the energy compensation is
   the compensation of the lobe actually drawn, and the anisotropic lobe reduces to the isotropic
   one at strength 0 without a step.
2. **Burley diffuse** (Disney, the BRDF GT7 builds on), in Frostbite's energy-normalised form
   (Lagarde and de Rousiers 2014): `f_d = albedo / pi * F(N.L) F(N.V) * energyFactor`, with
   `F(x) = 1 + (f90 - 1)(1 - x)^5`, `f90 = energyBias + 2 L.H^2 roughness`,
   `energyBias = 0.5 roughness`, `energyFactor = mix(1, 1 / 1.51, roughness)`. The `(1 - F)(1 - metallic)`
   weight stays. Area lights evaluate it toward the rectangle's centre. The ambient diffuse stays
   Lambert irradiance (the SH has no view dependence to give it).
3. **Horizon specular occlusion** (the fade Jimenez et al. 2016 and Frostbite use): the environment
   specular is multiplied by `h^2`, `h = saturate(1 + k dot(R, geoNormal))` with `k = 1`, `R` the
   (bent) reflection vector. Screen-space reflections are not faded: they see the real geometry.
4. **The sun is a disk.** Every directional light takes the environment's sun angular radius. The
   specular lobe looks at the point of the disk closest to the reflection vector (Filament's
   `getSunDirection`: `R` itself inside the disk, the disk's edge toward `R` outside), with the
   lobe renormalised by `(alpha / alpha')^2`, `alpha' = saturate(alpha + sin(radius) / 2)`, the
   widening the area lights already use. Diffuse keeps the disk's centre.
5. **Point and spot lights get a source radius**: `LightComponent::sourceRadius` in metres, default
   0 (a point, as today), editor "Source Radius", saved as `source_radius`. Specular uses Karis
   2013's representative point on the sphere closest to the reflection ray, renormalised by
   `(alpha / alpha')^2`, `alpha' = saturate(alpha + radius / (2 distance))`. Diffuse keeps the centre.
   Stored in `GpuLightData::spotAndArea.z` (unused by point and spot lights).
6. **The coat gets the same treatment** (Smith, disk, sphere); sheen keeps its own lobe.
7. **Shared GLSL** (`brdf_common.glsl`, compiled into a C++ test): the correlated visibility, Burley
   diffuse, the horizon fade, the disk and sphere representative directions and the normalisation.

## Automated Verification

- The correlated visibility matches the anisotropic one at equal alphas (existing test) and a
  direct evaluation of Heitz's formula.
- Burley diffuse: the directional albedo `integral f_d N.L` for a white albedo stays within
  [0.9, 1.05] for every roughness and N.V (energy-normalised), and at roughness 0 and N.V = 1 is
  within 0.05 of Lambert's 1.
- DFG table: `A + B <= 1`; within 0.01 of an independent quadrature over outgoing directions using
  the correlated visibility.
- Sun disk: the direction is `R` when `R` lies in the disk, otherwise a unit vector exactly on the
  disk's edge in the plane of `R` and the centre.
- Sphere light: the representative direction lies within the sphere's cone and equals the centre
  direction at radius 0; the normalisation is 1 at radius 0 and in (0, 1] otherwise.
- Horizon fade: 1 when `R . n >= 0`, 0 at `R = -n`, monotonic in between.

## Manual Acceptance (by image)

1. A smooth sphere under the sun shows a sun-sized highlight instead of a pixel.
2. A point light with a source radius gives a larger, dimmer highlight on a smooth sphere; radius 0
   matches the previous build.
3. Sponza: diffuse slightly brighter at grazing angles on rough surfaces (Burley's retroreflection),
   rough metals slightly different; no edge glow on normal-mapped stone.

## Amendments During Implementation

- **A long-standing bug in `DistributionGGX`.** It floored `pi d^2` at `1e-4`; the true value at a
  lobe's peak is `pi alpha^4`, far below that for any roughness under about 0.3, so every smooth
  highlight was cut, by up to five orders of magnitude at the 0.04 roughness floor. Smooth spheres
  under the sun showed a dim grey blur in the previous build as in this one until the floor went.
  The denominator is now only kept positive.
- **The DFG table takes 2048 samples** (was 512): the correlated visibility's estimator is noisier
  under GGX importance sampling, 0.013 off at roughness 0.5, N.V 0.2 with 512. Noise can push
  `A + B` a hair past 1, so the builder scales such texels back to 1.
- **The uniform ambient and the coat's uniform ambient sample the DFG table** instead of Karis'
  analytic fit, which approximates the Smith-Schlick table and would disagree with the lobe the
  direct lights now draw. The test keeps the fit only as a loose same-family bound (0.3).
- **Burley's albedo bound was wrong in the spec.** Smooth Burley loses most of its diffuse at
  grazing angles (albedo 0.22 at N.V 0.05), which is the model, not an error. The test checks it
  never exceeds 1.05, stays positive, equals Lambert head on when smooth, and keeps more at grazing
  when rough.
- **The GGX quadrature reference runs at roughness 0.5 and above**: a regular grid over outgoing
  directions cannot resolve a lobe of roughness 0.2.
- **Acceptance.** Smooth spheres (roughness 0.02 to 0.3, chrome 0.05) under the default sun: the
  previous build shows a grey blur on the smooth black spheres, this one a crisp sun highlight. In
  a dark scene, a 20000 lm lamp with source radius 0.2 m gives a visibly larger disk on the smooth
  spheres than radius 0. The sun's own reflection in a mirror sphere is still sub-pixel at this
  distance (about a millimetre on a 0.8 m sphere), as it physically is. Sponza: the floor picks up
  the lamp's gloss on its smoother patches; no fireflies, no noise.

## Out of Scope

- LTC area lights (phase 1b), tube lights, a sun-disk shape other than a circle.
