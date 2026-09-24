# Exposure-Driven Glare

## Goal

Bloom mixes a fixed 4% of every pixel into its surroundings, the same in a sunlit street and a dark
interior. GT7 (see `docs/references/gt7-rendering-notes.md`, section 3) instead derives the glare
from a camera aperture chosen by the exposure, through Fraunhofer diffraction: bright scenes are shot
stopped down and glare visibly, dark scenes wide open and barely. Replace the fixed mix with that
model, keeping the existing mip chain, so the glare amount, its radial falloff and a slight
dispersion all follow from the exposure and the wavelength instead of a slider.

The user accepts this by the rendered image.

## Model

1. **Aperture from exposure.** At ISO 100, `EV100 = log2(N^2 / t)`. With a fixed shutter
   `t = 1/125 s`, `N = sqrt(2^EV100 / 125)`, clamped to [1.4, 22] (the range of real lenses; GT7
   calibrated ten F-numbers in a similar span). EV 15.6 gives f/20, EV 10 f/2.9, EV 5 and below f/1.4.
2. **Diffraction energy outside a radius.** For a circular aperture the Airy pattern's encircled
   energy satisfies `1 - EE(rho) ~ 2 lambda N / (pi^2 rho)` once past the first rings (rho in the
   focal plane). With a full-frame sensor (24 mm tall) the pixel pitch is `p = 24 mm / viewport
   height`, so the energy that lands more than R pixels away is `K / R` with
   `K = 2 lambda N / (pi^2 p)`. This is independent of focal length, so it needs no field of view.
3. **Bands on the mip chain.** Bloom level k (half resolution at k = 0) has texels 2^(k+1) pixels
   wide; its blur stands for the ring from `R_k = 2^(k+1)` to `R_(k+1)`. Band k's energy is
   `K (1/R_k - 1/R_(k+1))`, and the last level takes everything beyond it, `K / R_last`. The energy
   within 2 pixels stays where it is. Total moved: `K / 2`.
4. **Dispersion.** K per channel with the Rec.709 primaries' dominant wavelengths, R 612 nm, G 549 nm,
   B 465 nm: red glare reaches slightly further than blue, GT7's "some dispersion".
5. **Composite, energy conserving.** `result = scene * (1 - total) + sum_k band_k * blur_k`, per
   channel. The upsample chain computes the weighted sum: level i becomes
   `band_i * down_i + tent(level i+1)`, with the bottom level scaled by its own band on the first
   upsample. A `strength` multiplier (default 1, the physical value) scales every band.

Figures at the capture resolution (541 px tall, p = 44 um), green: f/20 moves 2.5% of each pixel's
energy (the old fixed mix moved 4%), f/2.9 0.36%, f/1.4 0.18%. Against a dark background even 0.2% of
a lamp is visible, which is the night-scene glare GT7 describes.

## Decisions

- `engine/renderer/glare.h/.cpp`, pure and unit tested: `GlareFNumberFromEv100(ev)`,
  `ComputeGlareBands(fNumber, viewportHeight, levelCount, strength) -> std::vector<glm::vec3>`.
- `BloomSettings` becomes `{ enabled, strength = 1.0 }`; the Graphics Debug panel shows
  "Strength" (0-4) and the current f-number read-only. The old `intensity` goes.
- `ScenePassFrameContext` gains `glareFNumber`, from this frame's EV (the same one the pre-exposure
  uses). The bloom pass computes the bands itself, since it owns the level count.
- Push constants: `levelCount` and `intensity` give way to `vec4 destinationWeight` and
  `vec4 sourceWeight` (rgb used) for the upsample, and `vec4 remaining` (1 - total) for the
  composite; the block stays within 64 bytes.
- The Karis average on the first downsample stays.

## Automated Verification

- f-number: EV 15.6 → ~20, clamp at both ends, monotonic.
- Bands: sum equals `K / 2` per channel; each band below the last is half the one before; red > green
  > blue; zero strength gives zero bands; doubling the viewport height at the same angles halves
  every band except that the energy beyond a fixed angle is unchanged.

## Manual Acceptance (by image)

1. Daylight emissive sphere (2e7 cd/m^2, EV 15.6): a halo that falls off with radius, a warm fringe
   outward.
2. Sponza (EV ~5): the sunlit arch no longer spreads a visible glow into the vault; the image is
   otherwise as before.
3. Bloom off is unchanged from the previous build.

## Out of Scope

- HDR-output narrowing of the glare (no HDR output yet), star-shaped aperture blades, lens dirt.
