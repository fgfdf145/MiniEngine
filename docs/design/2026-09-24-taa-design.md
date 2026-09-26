# Temporal Anti-Aliasing

## Goal

The renderer has no anti-aliasing: geometry edges stair-step and shimmer, and highlights narrower
than a pixel (a clearcoat at roughness 0.05 in sunlight) are missed outright. This design adds TAA:
the projection is jittered by a sub-pixel offset every frame, and a resolve pass blends each frame
into a history reprojected through the motion vectors, so a still image converges to many samples
per pixel.

With TAA switched off the renderer must produce the image it produces today.

## Current State

- The G-buffer carries motion vectors, `velocity = currentUv - previousUv`, computed in
  `gbuffer.frag` from `fragCurrClip` (this frame's `proj * view * world`) and `fragPrevClip`
  (`prevViewProj * previousModel * position`). `prevViewProj` comes from `MotionHistory`, which is
  fed `renderProjection * view`. Background and Blend pixels have no velocity.
- VBAO already accumulates temporally: `VulkanAoResolvePass` owns two history images in `GENERAL`
  layout outside the layout tracker, and the pure `AoHistory` unit decides which is read, which is
  written and whether the read one is valid. It is not AO-specific.
- The deferred order is Geometry, AoTrace, AoResolve, Lighting, Forward, ExposureHistogram,
  Tonemap; the forward-only order is Forward, ExposureHistogram, Tonemap. Both end by metering and
  tone mapping `SceneHdr`.
- `State().viewportMatrices.renderProjection` is also used by the editor (gizmos, picking); the
  renderer copies it into the camera block and the light cluster builder.

## Decisions

1. **Jitter at upload, not in the editor's matrices.** The renderer builds a jittered copy of the
   render projection for the camera block (`proj`, `invViewProj`) and the cluster builder. The
   editor's matrices, and `MotionHistory`, keep the unjittered one. Jitter is a Halton(2, 3)
   sequence of 8 sub-pixel offsets in (-0.5, 0.5) pixels, added to the projection's clip-space
   translation (`P[2][0]`, `P[2][1]`).
2. **Motion vectors stay unjittered.** The camera block gains `viewProjNoJitter`, appended so no
   earlier offset moves. `triangle.vert` computes `fragCurrClip` with it, so a still camera gives
   zero velocity whatever the jitter.
3. **A resolve pass after Forward, in both orders.** `ScenePassId::Taa`, a compute pass, reads
   `SceneHdr`, depth, velocity and last frame's history, and writes a new storage target
   `SceneTaa` (RGBA16F) and this frame's history. The exposure histogram and tone mapping read
   `SceneTaa` instead of `SceneHdr`. With TAA off, and always in the forward-only order (which has no
   motion vectors), the pass copies `SceneHdr` to `SceneTaa` unchanged and the projection is not
   jittered, so the image is today's.
4. **The resolve** (Karis 2014, with the refinements that became standard):
   - velocity from the nearest-depth pixel of the 3x3 neighbourhood, so edges reproject with the
     surface in front; background pixels reproject by camera motion alone, from their far-plane
     position through `prevViewProj`;
   - history sampled with a 5-tap Catmull-Rom filter, which keeps it from blurring frame over frame;
   - history clipped toward the neighbourhood mean in YCoCg, to the box `mean ± 1.0 sigma` of the
     3x3 current samples (variance clipping), which removes ghosting from disocclusion and lighting
     change;
   - blend 10% current, 90% history, with each weighted by `1 / (1 + luma)` of the exposed colour so
     one bright sample cannot dominate (the flicker HDR TAA otherwise shows);
   - current only where history is invalid or reprojects off screen.
5. **History bookkeeping reuses `AoHistory`**, renamed `TemporalHistory` (and `AoHistoryFrame`
   `TemporalHistoryFrame`), with one instance per effect. The TAA pass owns its two RGBA16F history
   images the way the AO resolve owns its own.
6. **A switch in Graphics Debug**, "Temporal anti-aliasing", on by default, in `RenderDebugSettings`
   like the other renderer switches, not persisted. Turning it on starts from invalid history.
7. **Known limits, accepted:** Blend surfaces write no velocity, so under camera motion they
   reproject with whatever is behind them and may smear; the result is not sharpened.

## Data Flow

```
frame index ─► TaaJitter (Halton 2,3 x 8) ─► jittered proj ─► camera block proj / invViewProj
                                                          └─► BuildLightClusters
unjittered proj * view ─► viewProjNoJitter (camera block) ─► triangle.vert fragCurrClip ─► velocity
                       └─► MotionHistory ─► prevViewProj

Geometry … Lighting ─► Forward ─► SceneHdr ─► Taa (+ depth, velocity, history[r]) ─► SceneTaa, history[w]
                                                                    ─► ExposureHistogram, Tonemap
```

### Pure units

- `engine/renderer/temporal_history.{h,cpp}`: the renamed `AoHistory`, behaviour unchanged.
- `engine/renderer/taa_jitter.{h,cpp}`:
  `glm::vec2 TaaJitterPixels(uint32_t frameIndex)` and
  `glm::mat4 JitterProjection(const glm::mat4& projection, glm::vec2 jitterPixels, glm::uvec2 extent)`.

## Automated Verification

- `TaaJitterPixels`: every offset in (-0.5, 0.5), eight distinct offsets, the cycle's mean within
  1/16 pixel of zero, frame n and n + 8 equal.
- `JitterProjection`: a projected point lands exactly the jitter's pixels away from where the plain
  projection puts it, at any depth; zero jitter is the identity.
- The renamed history tests pass unchanged.

## Manual Acceptance

1. **Off is today.** TAA off: Sponza (five lights) in both orders within run-to-run noise of the
   previous build.
2. **Edges.** TAA on, still camera: stair-stepped edges in Sponza become smooth.
3. **Sub-pixel highlights.** The clearcoat spheres at coat roughness 0.05, whose sun highlight TAA
   off misses, show it with TAA on.
4. **Motion.** With the camera orbiting (temporary instrumentation), TAA on shows no ghost trails
   behind columns and no smeared sky compared with TAA off.

## Amendments During Implementation

- **The compute plumbing is shared.** The AO passes' samplers, set layouts, pipeline and dispatch
  helpers, descriptor pool and ping-ponged history images moved to `compute_pass_util`
  (`HistoryImagePair`) in a separate refactor, and both AO passes and TAA use them.
- **Acceptance 3 failed: TAA does not recover sub-pixel highlights.** On the clearcoat spheres at coat
  roughness 0.05, the sun's reflection is about 0.1 pixel in radius, some 3% of a pixel; eight fixed
  jitter positions usually all miss it, and the measured peaks are the same with TAA off and on. This
  needs specular anti-aliasing (widening roughness by the pixel's normal variation), not more
  temporal samples. It is left as a follow-up.
- **Results** (Sponza with five lights, 3500 frames): TAA off differs from the previous build in
  0.74% of pixels deferred and 0.83% forward-only, by one 8-bit step (two runs of one build: 0.72%);
  with TAA requested, forward-only passes through (0.86%). TAA on, still camera: 172 pixels change by
  more than 60, all on arch and column edges, which lose their stair steps. With the camera turning
  0.3 degrees per frame: no ghost trails behind the columns, but texture detail softens in motion.
- **The acceptance scripts must pass environment through `env`.** Under `/bin/sh` in POSIX mode,
  `VAR=1 function` assignments persist after the call, which invalidated one batch of captures.

## Out of Scope

- Sharpening, TAA upscaling, per-object velocity for Blend surfaces.
- Jitter-aware mip bias for textures.
