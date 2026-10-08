# Display Calibration (PS5 / GT7 style)

## Problem

The user reports that the HDR settings "do nothing". Measured on this machine (Windows 11, HDR on,
one 3840x2160 display that reports 600 nits peak, 600 nits full frame, 0.005 nits black):

- Windows shows SDR content at **480 nits** (SDR content brightness `SDRWhiteLevel` 6000). The
  editor's HDR path puts UI white at a fixed 203 nits (BT.2408) and GT7's paper white at 250 nits,
  so turning HDR output on made the whole editor and the scene about half as bright as the SDR
  swapchain it replaced.
- The peak setting reshapes only GT7's shoulder, which starts at 0.444 x peak: midtones are the
  same at every peak, so moving the slider changes little that is visible.
- HDR output was off by default and had to be found in Graphics Debug; nothing said whether the
  display was in HDR or what it reported.

## Goal

A calibration screen in the way of the PS5 system's Adjust HDR, followed by GT7's Display Settings
assistant (its online manual, Tips 01), and output that uses every value it sets:

| Output | Steps |
| --- | --- |
| HDR | 1. Maximum full-frame luminance, 2. maximum luminance (10 % window), 3. minimum luminance (black), 4. exposure and saturation over sample images, 5. review |
| SDR | 1. Bright section correction, 2. dark section correction, 3. exposure and saturation, 4. review |

## Design

1. **What the display reports** (`engine/platform/display/display_hdr.h`). On Windows, DXGI's
   `IDXGIOutput6::GetDesc1` for the monitor the window is on gives whether HDR is on
   (`G2084_NONE_P2020` colour space) and the max, max full-frame and min luminance (EDID, or the
   Windows HDR Calibration app's profile). `DisplayConfigGetDeviceInfo(SDR_WHITE_LEVEL)` gives the
   SDR content brightness. A background thread polls once a second, because Windows sends no event
   when the user moves the SDR brightness slider. Other platforms report nothing.
2. **Settings** (`DisplaySettings`, group `display` in the render settings, so captures replay
   them): output mode (Auto follows the system as GT7 follows the console's HDR setting, HDR, SDR),
   whether a calibration has been done, the three luminances, exposure (EV), saturation, the SDR
   white and black signal levels, and a UI white override (0 follows Windows). Until a calibration
   is done the luminances are the display's reported ones. The old `hdr_output` and `hdr_peak_nits`
   keys are dropped.
3. **UI white follows Windows.** In HDR the editor's ImGui shader places UI white at Windows' SDR
   white level, so the editor looks the same with HDR output on or off and beside other windows.
   The value is a specialization constant patched into the shader's SPIR-V; a change rebuilds the
   ImGui backend with the swapchain (rare: only when the Windows slider or the override moves).
4. **Paper white follows Windows too** (added after the user found HDR still darker than SDR): GT7
   puts its SDR paper white at an absolute 250 cd/m^2, while Windows shows SDR content (and the
   engine in SDR) at 480 here, so HDR midtones were 0.94 EV darker than the same scene in SDR. The
   scene is now scaled by paper white / 250 before the curve, the paper white following the UI white
   unless set (250 gives GT7's absolute scale), and glare's headroom is the peak over it. Measured:
   HDR / SDR scene luminance 1.00-1.035 in every band from 20 to 480 cd/m^2; highlights reach 572.
5. **HDR output path** (tonemap.frag): exposure and saturation, GT7's HDR curve for the calibrated
   max luminance, then the BT.2390 black-level lift in PQ, `E' = E + b (1 - E)^4` with
   `b = PQ(min luminance)`, so the darkest scene detail lands at the display's floor instead of in
   its crush, then divided by the UI white. The full-frame luminance and the peak go to the display
   as HDR metadata (`VK_EXT_hdr_metadata`: MaxCLL, MaxFALL, mastering min and max) where the driver
   offers it.
6. **SDR output path**: exposure, saturation and GT7's SDR curve, then the signal remap
   `s' = black + s (white - black)` on the sRGB-encoded value: lowering white brings near-white
   detail back on a display that clips, raising black brings shadow detail out of one that crushes.
7. **Patterns** come from the tonemap pass in place of the scene, in absolute nits for HDR (written
   relative to the UI white, which the ImGui shader undoes), with the editor fullscreen so the
   full-frame pattern covers the screen:
   - full frame: the whole screen at the trial level, a ring at 10 000 nits; raise until the ring
     disappears (the display clips at that level);
   - 10 % window: black, a centred square of 10 % of the screen at the trial level, the ring at
     10 000 nits inside it;
   - black: the screen at the trial level, the ring at 0 nits; raise from 0 until the ring just
     shows;
   - SDR bright / dark: checkerboards at signal 1.0 / 0.96 and 0.0 / 0.04 through the trial
     correction; adjust until the checkers can barely be seen;
   - exposure and saturation: the live scene and two synthetic test cards (a step wedge from 1/64 to
     64x paper white with colour patches, and a sky gradient with a sun disc), switched with Q / E
     as GT7 switches them with L1 / R1, all through the full output path.
   Trial levels step evenly in PQ (HDR) or signal (SDR), with the arrow keys or the slider.
8. **Editor.** View > Display Calibration... and a button in Graphics Debug's Output section, which
   also shows what the display reports, what is in use, and the output mode. Cancel restores the
   previous values; Finish keeps them (saved with the engine settings).

## Verification

- Unit tests: the PQ round trip, the black lift (0 maps to the floor, bright values unchanged, monotonic),
  the SDR remap, the pattern levels, the SPIR-V spec-constant patch, the settings round trip.
- On the user's display: the desktop's composited image read back as FP16 scRGB (Desktop
  Duplication) shows the patterns at the requested nits and the editor's UI white at Windows' SDR
  white.
