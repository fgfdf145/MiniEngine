# Specular Anti-Aliasing

## Goal

A highlight narrower than the pixel's footprint is sampled or missed at random: TAA's eight
jitter positions miss the sun's reflection on small clearcoat spheres (about 0.1 pixel in radius)
entirely, and normal-mapped glossy surfaces shimmer as the camera moves. The fix is to shade what the
pixel actually covers: widen the specular lobe by how much the normal varies across the pixel, so
the highlight's energy spreads over the pixels that should share it.

With the switch off the renderer must produce the image it produces today.

## Decisions

1. **Geometric specular anti-aliasing (Tokuyoshi & Kaplanyan 2019), as Filament ships it.** From the
   screen-space derivatives of the normal, `variance = sigma^2 * (|dN/dx|^2 + |dN/dy|^2)` with
   `sigma^2 = 0.15`; the kernel `min(2 * variance, 0.2)` is added to `alpha^2` (alpha = perceptual
   roughness squared), clamped to 1, and turned back into perceptual roughness.
2. **Where.** In both material fragment shaders, before the roughness is written to GB2 or used:
   the base roughness with the shading normal (so normal-map detail counts), the clearcoat
   roughness with the geometric normal (the coat's own normal). Sheen is left alone: its lobe is
   already broad. Derivatives come from `dFdx`/`dFdy` in the fragment shader, which the deferred
   path has and the lighting pass would not.
3. **A pure C++ twin**, `FilterRoughnessForSpecularAA(perceptualRoughness, normalDerivativeLengthSquared)`
   in `engine/renderer/specular_aa.{h,cpp}`, carries the tests; the shaders run the same formula
   from `specular_aa.glsl`.
4. **A switch in Graphics Debug**, "Specular anti-aliasing", on by default, carried to the shaders in
   a vec4 appended to the camera block (`specularAntiAliasing`: x enabled, y variance, z threshold).
   Off, the shaders skip the filter entirely.

## Automated Verification

- A flat surface (zero derivative) keeps its roughness exactly.
- The filtered roughness never decreases, grows with the derivative, and caps where the threshold
  does (`alpha^2 + 0.2`).
- A mirror (roughness 0) on a sphere 45 pixels in radius (derivative per pixel 1/45) comes out
  near the value the shader will give the acceptance spheres.

## Manual Acceptance

1. Off: Sponza (five lights), both orders, within run-to-run noise of the previous build.
2. On: the coat-roughness-0.05 spheres show the sun's coat highlight, with TAA on and off; the
   uncoated row changes little.
3. On: Sponza still (TAA on) shows no visible loss of highlight sharpness on flat surfaces.

## Amendments During Implementation

- **The coat's filter starts from its floor.** The coat roughness is filtered from
  `clamp(roughness, 0.04, 1)`, the value the lighting would give it, and stored filtered in GB5;
  with the switch off GB5 holds the material's value as before.
- **Results.** With TAA on, two runs of one build already differ in 1.6% of pixels (max 11, the
  jitter meeting wall-clock exposure adaptation). Filter off stays within that: 1.43% deferred
  (max 13) and 0.43% forward-only (max 1). Filter on: 30% of Sponza's pixels change, slightly (mean
  0.14, max 23), on its normal-mapped stone. On the coat-roughness-0.05 spheres the coat's sun
  highlight appears on every sphere, with TAA on and off: the coated row's peaks go from 248-576 to
  454-627 while the uncoated row stays within a few steps.
- **Stage Manager.** Every capture was checked for its size (667 x 541) and for resizes after
  start-up; none happened, so the window's stage did not reach the images.

## Out of Scope

- Filtering normal maps into roughness at mip generation (Toksvig or LEAN maps).
- Specular AA for sheen or area lights beyond the roughness change.
