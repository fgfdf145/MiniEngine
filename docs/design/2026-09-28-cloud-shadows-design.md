# Cloud Shadows

## Goal

The volumetric clouds (2026-09-28-volumetric-clouds-design.md) hide the sun's disk but not its
light: a car under a cloud stays in full sun. This design shades the sun with the clouds wherever
it lights a surface, so cloud shadows sweep over the track and the scene dims when a cloud crosses
the sun.

Scope: the shadow casting directional light (the sun) in the Atmosphere mode with clouds on, in
the deferred and forward lighting (`EvaluateDirectionalShadow`, so the ground plane, the scatter
pre-pass and every material get it) and in the DDGI probe rays, so the bounce light dims too.

## The map

A 512² RGBA16F image (`VulkanAtmosphere`, GENERAL like the LUTs; r used) over 16 km × 16 km of
ground centred on the camera, 31 m per texel. `cloud_shadow.comp` writes it every frame the
atmosphere renders, after the sky-view LUT: per texel, its point of the ground (world y = 0, taken
to the atmosphere's kilometres as `ToAtmosphereCameraPositionKm` does) marches 16 steps toward the
sun through the cloud shell (`CloudShellInterval`, up to 200 km so a low sun still crosses the
layer), reading the base shape without the detail, and stores `exp(-optical depth)`. The map is
cleared to 1 on creation and writes 1 when the clouds are off or the sun is down.

The centre is the camera's x, z snapped to whole texels (`CloudShadowMapCenter`), so the map slides
under a moving camera without its texels shimmering. The compute pass and every lookup derive it
from `ubo.cameraWorldPosition`, so no uniform is added.

## The lookup

`CloudShadow(worldPos)` in `cloud_shadow.glsl`, included by `atmosphere_sampling.glsl`:

```
ground = worldPos.xz - sun.xz * worldPos.y / max(sun.y, 0.05)
uv     = (ground - centre) / 16 km + 0.5
shadow = mix(1, map(uv).r, edgeWeight(uv))
```

The point's ray toward the sun and its ground point's ray are the same line, so they cross the
same clouds (true for anything below the cloud base). The sun's height is held at 0.05 so a sun at
the horizon does not throw every lookup off the map. Over the map's outer tenth the shadow fades to
none, so its edge never shows; beyond it the sun is unshadowed.

- `EvaluateDirectionalShadow` multiplies the cascades' result by it, and returns it alone past the
  last cascade: cloud shadows reach beyond the shadow distance.
- `ddgi_trace.comp` multiplies the shadow caster's irradiance at a probe ray's hit by it.

## Cost

One 512² dispatch of 16 steps of three texture taps each, about 12 M taps a frame; the lookup is
one tap per shaded sun sample. Not yet measured on a GPU.

## Tests

`tests/volumetric_clouds_tests.cpp`: the centre snaps to texels and ignores height; the ground under
the centre maps to uv 0.5; with the sun overhead height does not move the lookup; at 45° a point
100 m up looks 100 m back along the sun's azimuth; a sun at the horizon is held at the minimum
height; the edge weight is 1 inside, 0 at and past the edge, 0.5 half way through the fade.

A CPU port of the map at the defaults (coverage 0.45, sun 25° up) put 37 % of the ground in shadow;
inside a cumulus's shadow the sun's transmittance is ~0, so shadows are lit by the sky alone, about
a fifth of full sun, as real cumulus shadows are.

## Not in scope

- The exposure and white balance references still take the sun as unshadowed; the histogram half
  of each follows a shadowed view.
- The height fog's sun glow, the planet ground below the horizon and the sky SH ignore the clouds.
- Light the clouds transmit forward into their own shadow (a thin cloud's shadow is brighter than
  exp(-depth) of the direct sun alone suggests).
