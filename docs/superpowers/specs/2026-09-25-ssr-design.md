# Screen-Space Reflections and Specular Occlusion

## Goal

The only specular ambient the renderer has is the environment: the prefiltered sky cubemap, or the
uniform ambient luminance under `EnvironmentMode::None`. Every glossy surface reflects the sky or a
flat grey, a polished floor shows no columns, and crevices shine as brightly as open ground because
the specular term is darkened only by the diffuse AO. GT7 (`docs/references/gt7-rendering-notes.md`,
section 5) fixed exactly this with ray tracing while keeping the split sum: the DFG table still
weights the specular lobe, and only the prefiltered-radiance term is replaced by radiance found along
GGX-sampled rays, reconstructed with sample reuse, temporal and bilateral filters. Do the same in
screen space, on every platform (MoltenVK has no ray queries), and add specular occlusion for what
the screen cannot see.

The user accepts this by the rendered image.

## Design

1. **Trace** (`SsrTrace`, compute, full resolution, deferred order, after the AO resolve): per pixel
   one ray, its direction the reflection of V about a GGX half vector sampled from the visible
   normal distribution (Heitz 2018) at the pixel's roughness, with interleaved gradient noise that
   changes every frame. The ray is marched in screen space between its projected start and end
   (at most 30 m, clipped to the near plane), 48 steps with a per-pixel jittered start and view
   depth interpolated as 1/z, then 6 binary-search steps. A hit is a step whose ray depth passes
   behind the depth buffer by less than a thickness of 0.1 m + 3% of the distance; the sky, a
   backfacing hit and leaving the screen are misses.
2. **Colour** of a hit: the previous frame's anti-aliased image, TAA's history before bloom, at the
   hit point reprojected through `prevViewProj`, scaled by `taaHistoryScale` into this frame's
   pre-exposure. Reflections of reflections arrive a frame later, as GT7's single-bounce trace with
   shading at the hit does. Without valid TAA history (TAA off, first frame) nothing is traced.
3. **Confidence** (alpha): hit, times a fade over the last 10% of the screen at each edge, times a
   roughness fade from 1 at 0.4 to 0 at the maximum roughness (0.6 by default: rougher lobes are the
   environment's), times a fade over the last 20% of the ray length.
4. **Resolve** (`SsrResolve`): a 5x5 depth- and normal-aware spatial filter whose footprint grows
   with roughness (a mirror keeps its single sample), then temporal accumulation reprojected through
   the surface's motion vector with the history clipped to the current 3x3 neighbourhood, 1/8
   current. Output `SceneReflections` (RGBA16F): pre-exposed radiance and confidence.
5. **Shading.** `ShadeSurface` takes the reflection. The specular lobe's radiance becomes
   `mix(environment * specularOcclusion, reflection / preExposure, confidence)`; diffuse keeps the AO
   as before. Specular occlusion is Lagarde and de Rousiers 2014:
   `saturate(pow(NdV + ao, exp2(-16 roughness - 1)) - 1 + ao)`. The forward path (Blend items and the
   forward-only order) passes no reflection and gets the occlusion alone. Coat and sheen keep the
   environment.
6. **Controls:** Graphics Debug, "Screen-space reflections" (default on) and "Max roughness"; a
   "Reflections" G-buffer view shows `SceneReflections`.

## Automated Verification

- `ssr_common.glsl` (compiled into a C++ test as the other shared GLSL is): specular occlusion is 1
  with no AO, never above AO + something, 0 at ao 0 for a rough surface; the edge fade is 1 inside
  and 0 at the border; the roughness fade is 1 below 0.4 and 0 at the maximum; the VNDF sample is a
  unit vector in the upper hemisphere and equals the normal at roughness 0.
- Pass order: `SsrTrace`, `SsrResolve` between `AoResolve` and `Lighting` in the deferred order only.
- The new targets are storage targets.

## Manual Acceptance (by image)

1. Sponza: the floor reflects the columns and arches; crevices and the undersides of arches lose
   their flat grey specular sheen.
2. The forward-only order and SSR off: only the specular occlusion differs from the previous build.
3. No streaks or holes along screen edges; reflections fade out rather than cut off.

## Out of Scope

- Hi-Z tracing, half-resolution tracing, ray-traced reflections, reflections on Blend surfaces,
  coat and sheen reflections.
