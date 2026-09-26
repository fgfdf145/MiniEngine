# Viewport at the Display's Pixel Resolution

## Goal

Render the scene at the viewport's physical pixel size. Found while explaining how the viewport's
resolution is chosen: on a Retina display (2 pixels per point) the scene rendered at the panel's size
in points, a quarter of its pixels, and was stretched to fill it. The Khronos comparison was unfair for
the same reason: the browser draws the Sample Viewer at 2 pixels per point.

## Current State

- `BuildViewportExtent` rounds the viewport panel's content region, `ImGui::GetContentRegionAvail()`,
  which is in points; that becomes `requestedViewportExtent`, the scene targets' size.
- The swapchain and the UI already use pixels (`SDL_GetWindowSizeInPixels`; ImGui maps points to
  pixels through `DisplayFramebufferScale`).
- Effects are resolution-aware: AO and SSR distances are in metres, glare bands follow the viewport
  height, TAA's jitter the target size.

## Decisions

1. **Extent** = the panel's size in points x pixels per point (`io.DisplayFramebufferScale`) x the
   render scale, rounded, at least 1 x 1 (`ScaleViewportExtent`, a pure function in
   `render_types.h`). Aspect ratio and every UI coordinate (gizmos, picking) stay in points; only the
   render targets change.
2. **Render scale**: `RenderDebugSettings::renderScale`, a Graphics Debug slider from 25 to 100 %,
   default 100 %, to trade sharpness for speed (a Retina viewport at 100 % has four times the pixels
   it had). Not persisted, like the other Graphics Debug settings.
3. `--viewport-size WxH` stays in pixels and overrides both.
4. **The comparison** captures at the viewer's pixel count: `capture_engine.sh`'s default size becomes
   1334x1082, the viewer's canvas at 2 pixels per point, and the viewer captures are kept at that
   size instead of being scaled to 667x541.

## Automated Verification

- `ScaleViewportExtent`: 1 pixel per point is the point size; 2 doubles both axes; a render scale of
  0.5 halves them; fractional sizes round; zero or negative sizes give 1 x 1.

## Manual Acceptance (by image)

1. A render scene (`iridescence.yaml`) in the editor on the Retina display: sharper than before, same
   framing and exposure; the Render Size readout shows twice the panel's point size.
2. The Khronos comparison at 1334x1082 against full-resolution viewer captures: the mean differences
   stay at or below their 667x541 values; `IORTestGrid`'s magnified refraction sharpens.

## Acceptance (2026-09-26)

- The 40 comparison scenes at 1334x1082 against native viewer captures: every mean difference at or
  below its 667x541 value (`IORTestGrid` 5.8).
- `IORTestGrid`'s magnified refraction did not sharpen: it was never the resolution. The viewer
  magnifies its scene copy with nearest filtering and the engine's specular AA widens the small
  spheres' reflections (the transmission and volume spec's amendment).
