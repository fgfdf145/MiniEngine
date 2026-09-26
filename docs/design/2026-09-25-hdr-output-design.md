# HDR Display Output

## Goal

The editor presents to an 8-bit sRGB swapchain, so everything the GT7 operator rolls off above SDR
paper white is lost on an HDR display. GT7 (`docs/references/gt7-rendering-notes.md`, section 2.3)
targets both from one curve: `initializeAsHDR(peakNits)` reshapes the same operator for the
display's peak, and the result is PQ-encoded. Add that output path: an HDR10 swapchain, the GT7 HDR
curve, and an editor UI that stays at its usual brightness on it.

The user accepts this by the rendered image, on this Mac's display (MoltenVK offers
A2B10G10R10 with `VK_COLOR_SPACE_HDR10_ST2084_EXT`, and macOS maps it through EDR).

## Design

1. **Swapchain.** `VK_EXT_swapchain_colorspace` is enabled when the instance offers it. With HDR
   output requested, the swapchain takes `A2B10G10R10_UNORM` (or `A2R10G10B10`) in
   `HDR10_ST2084`; if the surface has neither, it stays SDR and the request is reported once.
2. **One display encoding for UI and scene.** Today the scene's LDR image and ImGui's colours are
   both display-linear values that the sRGB swapchain encodes, 1.0 being SDR white. In HDR mode they
   stay display-linear Rec.709 relative to UI white, `kUiWhiteNits = 203` (ITU-R BT.2408's graphics
   white), and one ImGui fragment shader turns every output into PQ:
   `PQ(Rec709ToRec2020(c) * 203 nits)`. The UI therefore looks as it does in SDR, and the scene can
   exceed 1.0.
3. **Scene LDR target.** `R16G16B16A16_SFLOAT` in HDR mode (the swapchain's format otherwise), so
   values above UI white survive to ImGui.
4. **Tone mapping.** GT7's `initializeAsHDR(peak)`: the curve and chroma fade built for the display
   peak (250 to 10 000 nits, default 1000, editable), output in frame-buffer units (1.0 = 100 nits),
   written as `out * 100 / 203`. The white balance and exposure are unchanged. The peak is a setting
   because Vulkan does not report the display's peak.
5. **Switching.** `RenderDebugSettings::hdrOutput` (default off) and `hdrPeakNits`, in the Graphics
   Debug panel's Output section. A change recreates the swapchain and everything built on its format
   (scene targets when the LDR format changes, ImGui's pipeline).
6. **Captures** of an fp16 LDR target clip at UI white and sRGB-encode, so `--capture` still writes
   an SDR PNG.

## Automated Verification

- `Gt7InitializeAsHdr` + `Gt7ApplyToneMapping` match the vendored reference's `initializeAsHDR` for
  random inputs at several peaks (the same bit-exact harness as SDR).
- PQ: 0 nits encodes to ~0, 100 nits to ~0.508, 10 000 nits to 1.
- The HDR curve at a 250-nit peak equals the SDR curve times 2.5 (GT7 builds SDR as a 250-nit HDR
  output scaled into [0, 1]).

## Manual Acceptance (by image)

1. HDR off: unchanged.
2. HDR on: the editor UI at its usual brightness; the daylight emissive sphere and bright sky
   visibly brighter than UI white; no clipping to flat white inside the sphere's glow.

## Out of Scope

- GT7's narrower glare on HDR displays, HDR metadata (`VK_EXT_hdr_metadata`), scRGB output,
  Rec.2020 scene colours beyond Rec.709 (the LDR target stays Rec.709).

## Amendments During Implementation

- This Mac's MoltenVK reports A2B10G10R10 and A2R10G10B10 in `HDR10_ST2084`, plus RGBA16F in extended
  linear sRGB; the HDR10 path is used. A run with HDR output on selects it (no fallback warning),
  renders and captures without errors. The on-screen result could not be captured here (the HDR
  swapchain is not what `--capture` reads, and a desktop screenshot would record other windows), so
  it is left to the user to judge on the display.
- `pre_exposure.glsl`, used since the pre-exposed HDR change, was missing from the shader include
  list the build tracks; it is listed now with `hdr_output.glsl`.
