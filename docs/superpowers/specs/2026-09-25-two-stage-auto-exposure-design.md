# Two-Stage Auto Exposure

## Goal

Auto exposure meters only the frame buffer and adapts one exponential toward it, so every frame ends
up "properly exposed": a dark interior seen in daylight is lifted to mid gray as fully as a night
street. GT7 (`docs/references/gt7-rendering-notes.md`, section 4) models the eye in two stages and
meters more than the view: a slow long-term adaptation (receptor sensitivity) that follows the
world's light, and a fast short-term one (pupil and neural gain) that follows the view but can only
move a few stops from the long-term state. The result is that a tunnel or an interior under a
daylight sky stays darker than a mid-gray exposure would make it, while an outdoor view is as before.

The user accepts this by the rendered image.

## Model

1. **References**, each a luminance a reflected-light meter would read, used when present:
   - the frame: today's histogram average (percentile window) in cd/m^2;
   - the sun: a gray card (18%) lit by the brightest directional light, `E * 0.18 / pi`, with E the
     illuminance that reaches the ground (after atmospheric transmittance in Atmosphere mode); left
     out below 0.001 lx (sun below the horizon);
   - the sky: HDRI mode, the SH's average radiance (L0 band); None mode, the ambient luminance.
     Atmosphere mode has no CPU-side sky average, so its sky reference is absent.
   Each becomes an EV100 (`log2(L) + 3`, K = 12.5) and the long-term target is their weighted mean,
   frame 0.5, sun 0.25, sky 0.25, renormalized over the references present, then compensation and
   the EV range apply as today.
2. **Long-term stage.** Snaps to its target on the first metered frame (as auto exposure does
   today), then adapts exponentially at 0.05/s toward brighter and 0.02/s toward darker (20 s and
   50 s time constants; GT7's tens of minutes are too slow for an editor where scenes are switched).
3. **Short-term stage.** Today's frame-metered target, clamped to the long-term EV +- 2.5 (GT7: 3-5
   EV of pupil and neural range), adapted at today's rates (3/s brighter, 1.5/s darker).
4. The references are the ones gathered while recording the previous frame, which is irrelevant at
   these rates.

## Decisions

- `exposure.h`: `ExposureReferences { optional frameLog2Luminance, sunIlluminanceLux,
  skyLuminance }`, `MeterLongTermTargetEv100(references, settings)`, `AutoExposureState
  { longTermEv100, initialized }`, `StepAutoExposure(state, currentEv100, frameTargetEv100,
  longTermTarget, deltaSeconds, settings) -> float`; `AutoExposureSettings` gains
  `shortTermRangeEv`, `longTermToBrighterPerSecond`, `longTermToDarkerPerSecond`, the three weights.
- `Camera` gains `adaptedLongTermEv100` (written by the renderer, shown in the Camera panel with the
  short-term range).
- The renderer keeps `AutoExposureState` and `ExposureReferences`; `m_hasMeteredExposure` becomes
  `state.initialized`, reset wherever it is reset today.

## Automated Verification

- Long-term target: weighted mean of the references present; frame only equals today's target;
  sun reference of 100 000 lx gives about EV 15.5; compensation and range apply.
- Short-term clamp: a frame target 5 stops under the long-term state lands 2.5 under.
- First step snaps both stages; later steps are frame-rate independent (two half steps equal one).

## Manual Acceptance (by image)

1. Outdoor daylight (emissive spheres, atmosphere): exposure within 0.1 EV of the previous build.
2. Sponza (interior, directional light, flat ambient): the exposure the log shows moves only as the
   long-term clamp requires; the image is as before or slightly darker, never brighter.
3. A dark view under a daylight sun (camera pointed down into shadow): darker than a mid-gray exposure.
