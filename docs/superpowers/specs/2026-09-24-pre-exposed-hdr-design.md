# Pre-Exposed HDR Buffer

## Goal

The scene is rendered into fp16 targets in physical units (cd/m^2) and exposed only by the tone
mapping pass, so every radiance above 65504 is clipped where it is written. At daylight exposure
(EV 15.6) that ceiling is about 1.1 times sensor saturation: the sun disk, bright emissives and sharp
highlights all arrive at TAA and bloom already flattened. The bloom spec measured it (two emissive
spheres of 2e5 and 2e7 cd/m^2 at EV 15.6 gave the same halo) and left this as the follow-up.

Store pre-exposed values instead, in the unit the GT7 tone mapper takes (see
`docs/references/gt7-rendering-notes.md`, section 2.3): **1.0 in the HDR target is 100 cd/m^2 as
displayed**, so the fp16 range follows the viewer instead of the scene. The image must otherwise be
the one the renderer produces today: this is a change of units, not of look.

## Decisions

1. **Unit: GT7 frame-buffer units.** `PreExposureFromEv100(ev) = ExposureFromEv100(ev) *
   kFrameBufferUnitsPerExposed`, with `kFrameBufferUnitsPerExposed = 2.5` moved out of
   `gt7_tonemap.glsl` (where it is `kExposedToGt7FrameBuffer`) into a shared definition that C++ and
   GLSL both read. Physical radiance times the pre-exposure is what the tone mapper takes, so the
   tone mapping pass no longer multiplies by anything.
2. **This frame's exposure.** Auto exposure is metered and adapted on the CPU before the frame is
   recorded (`UpdateAutoExposure`), so the value the writers pre-expose with is the value every
   reader uses in the same frame. Nothing lags a frame, unlike engines that adapt on the GPU.
3. **Delivered in the camera block.** `CameraUniformData` gains a trailing `vec4 exposure` (x =
   pre-exposure, y = 1 / pre-exposure, zw unused), mirrored in `scene_common.glsl`. Every shader that
   binds set 0 reads it; `SceneFrameContext::exposure` becomes the pre-exposure for the passes that
   push it.
4. **Shading stays physical.** Lights, IBL, the atmosphere LUTs and the environment probe capture are
   unchanged and in cd/m^2, and shading runs in fp32. Only the value written to a render target is
   multiplied, once, at the end of the writer, after aerial perspective.
5. **The writers.**
   - `deferred_lighting.frag`: the lit color times the pre-exposure. GB3 emissive arrives
     pre-exposed (item 6), so it is multiplied by 1 / pre-exposure before `ShadeSurface` and aerial
     perspective, which keeps the shared shading function in physical units.
   - `triangle.frag` (forward and Blend): the output color times the pre-exposure.
   - `sky.frag`: HDRI and atmosphere radiance times the pre-exposure, then clamped to 65504 as
     today. The sun disk (about 1e9 cd/m^2) lands near 4e4 at EV 15.6, inside fp16.
6. **GB3 emissive is pre-exposed.** `gbuffer.frag` writes emissive times the pre-exposure.
   B10G11R11 has relative precision, so scaling costs nothing, and emissives above 65000 cd/m^2 no
   longer clip there either.
7. **The background is a constant.** `GetBackgroundRadiance(exposure)` is removed. The flat
   background of `EnvironmentMode::None` stands for no physical light, so it is written as
   `kViewportBackgroundFrameBuffer` (today's `kViewportBackgroundExposed` times 2.5), which the tone
   mapper turns into the same display color. The forward clear, the lighting pass and `sky.frag` use
   it. The comment tying `kMaxExposureEv100` to fp16 goes: the background no longer scales with EV.
8. **TAA rescales its history.** History holds the previous frame's pre-exposed values. The renderer
   keeps the pre-exposure the TAA history was written with and pushes
   `historyScale = current / previous` (1 when history is invalid); the resolve multiplies the
   history sample by it before clipping. Without it an exposure change flashes for one frame and
   ghosts for several. The AO history is unitless and unaffected.
9. **Weights see the same luma.** TAA's `1 / (1 + luma * k)` and bloom's Karis weights used exposed
   luma (`k = exposure`). Stored values are now exposed times 2.5, so `k = 1 / 2.5`: numerically the
   same weights, so the image does not move.
10. **The histogram meters physical luminance.** `exposure_histogram.comp` multiplies each sample by
    1 / pre-exposure before binning; the CPU metering in `exposure.cpp` is unchanged.
11. **Tone mapping and debug views.** `TonemapExposedRec709` becomes a function of frame-buffer
    units, without its internal times 2.5. The emissive view tone maps GB3 as stored. The light
    cluster heat map is written unscaled by the lighting pass (no more `1 / exposure`) and shown as
    today.

## Automated Verification

- `tonemap_tests`: `kViewportBackgroundFrameBuffer` round-trips through the operator to
  `kViewportBackgroundDisplayLinear`; the bit-exact comparison with the GT7 reference still passes.
- `exposure_tests`: `PreExposureFromEv100` is `ExposureFromEv100` times 2.5 across the EV range; at
  EV 15 and 18 a 1e9 cd/m^2 sun pre-exposes below 65504.
- `CameraUniformData`'s `exposure` member follows `specularAntiAliasing` with no padding (the
  existing `offsetof` static assertions pattern).

## Manual Acceptance

1. **No change:** Sponza (five lights), 3500 frames, deferred with TAA on, deferred with TAA off, and
   forward-only, each within run-to-run noise of the previous build (see the bloom spec's figures).
2. **Daylight headroom:** the bloom spec's two emissive spheres (2e5 and 2e7 cd/m^2) at EV 15.6 now
   give visibly different halos, and the sun disk in an atmosphere scene blooms.
3. **Exposure changes:** dragging the manual EV slider with TAA on shows no one-frame flash and no
   trail.
4. **Debug views:** emissive, light cluster and the other G-buffer views look as they do today.

## Out of Scope

- Bloom or glare strength driven by exposure (notes, section 3), HDR display output, a baked tone
  mapping LUT, dual-stage auto exposure: separate follow-ups listed in the notes.
- Changing the render target formats.
