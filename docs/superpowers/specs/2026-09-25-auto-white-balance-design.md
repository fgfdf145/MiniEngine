# Auto White Balance

## Goal

The renderer shows every illuminant as it is: a 2000 K sunset or a warm interior light tints the
whole image as film under the wrong light would. GT7 (`docs/references/gt7-rendering-notes.md`,
section 4.4) balances white with the same machinery as its auto exposure: it measures the scene's
illuminant from its light references, blends in a faint virtual D65 light so the estimate is stable
at night, and adapts slowly toward D65 within the limits of a real camera's AWB. The eye and a
camera both adapt only partly, so a sunset stays warm, just not orange.

The user accepts this by the rendered image.

## Model

1. **Illuminant estimate**, the luminance-weighted mean chromaticity of:
   - the sun: the brightest directional light's ground illuminance (after transmittance), its colour;
   - the sky: HDRI mode the SH average radiance times pi (illuminance of a horizontal surface under a
     uniform sky of that radiance), None mode the ambient luminance times pi, its colour;
   - a virtual D65 light of 5% of that total plus 1 lx, which keeps a dark scene near neutral.
   Colours are linear Rec.709, converted to CIE XYZ; the mean is taken on XYZ (luminance-weighted
   chromaticity), then reduced to xy.
2. **Camera limits.** The estimate's correlated colour temperature (McCamy) is clamped to
   [2800 K, 10000 K]; outside it the white point moves to the Planckian locus at the clamped
   temperature (Kang et al. 2002).
3. **Temporal adaptation.** The adapted white point's xy moves exponentially toward the estimate at
   0.5/s (first frame snaps), the same way exposure adapts.
4. **Partial chromatic adaptation.** A Bradford transform from the adapted white to D65, with a degree
   of adaptation D = 0.8 (CIECAM02's D for an average surround is about 0.8-0.9): the cone responses
   scale by `D * (D65 / white) + (1 - D)`. As a 3x3 matrix on linear Rec.709 it runs in the tone
   mapping pass before the operator.

## Decisions

- `engine/renderer/white_balance.h/.cpp`, pure and unit tested: `Rec709ToXyz`, `XyzToXy`,
  `CorrelatedColorTemperature(xy)`, `PlanckianXy(kelvin)`, `LimitWhitePoint(xy)`,
  `EstimateIlluminantXy(references)`, `AdaptWhitePointXy(current, target, dt, rate)`,
  `WhiteBalanceMatrix(whiteXy, degree) -> glm::mat3` (linear Rec.709 to linear Rec.709).
- `Camera` gains `AutoWhiteBalanceSettings { enabled = true, degree = 0.8, rate = 0.5 }` and a
  read-only `adaptedWhiteKelvin` for the Camera panel.
- The renderer gathers the references beside the exposure ones, adapts once per frame, and hands the
  matrix to the tone mapping pass (push constants grow to 64 bytes: the view id plus three vec4 rows).
- Debug views stay unbalanced; only the shaded image is balanced.

## Automated Verification

- D65 in, identity out (any degree); degree 0 gives identity for any white.
- The matrix maps the source white's Rec.709 colour to neutral (r = g = b) at D = 1.
- McCamy gives ~6504 K for D65 and ~2856 K for illuminant A; `PlanckianXy(6504)` is within 0.005 of
  D65's xy; the limit clamps 2000 K to 2800 K and leaves 5000 K alone.
- The estimate of a scene with only the virtual light is D65; a strong 3000 K sun pulls it toward
  3000 K; adaptation is frame-rate independent.

## Manual Acceptance (by image)

1. Sponza (sun colour 1, 0.95, 0.9): a little less warm than before.
2. Daylight atmosphere scene: essentially unchanged (sun plus sky is near D65).
3. A warm light scene (sun colour set to 3000 K): warm, but visibly less orange than with AWB off.
