# Visibility Bitmask Ambient Occlusion

## Goal

Darken the ambient term of every deferred surface by a screen-space ambient occlusion estimate, using
the visibility bitmask method (Therrien, Levesque, Gilet 2023, "Screen Space Indirect Lighting with
Visibility Bitmask"), ambient occlusion half only. No bent normal, no indirect light.

The approach was proven in the feasibility spike (tag `archive/spike-vbao`, commit `f8c47e3`): a
compute trace that builds a 32-sector bitmask per slice, then a compute resolve that runs a
depth-aware spatial filter and a temporal accumulation reprojected through the motion vectors. This
design keeps the spike's shaders nearly as they are and replaces its plumbing: the spike's mutable
per-pass history state, its debug-only toggles, and its env-var harness do not survive.

## Current State

- The deferred order is Geometry, Lighting, Forward (Blend only), ExposureHistogram, Tonemap
  (`scene_pass_order.cpp`). The forward-only comparison order skips Geometry and Lighting.
- `RenderTargetLayoutTracker` knows two kinds of target, Color and Depth, with write layouts
  `COLOR_ATTACHMENT_OPTIMAL` and `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`. It is frame-scoped: it resets
  at the head of every command buffer, so no target it tracks may carry contents across frames.
- Set 2 (`VulkanGBufferDescriptors`) samples GB0-GB3, depth and velocity, one set per frame slot.
  The lighting pass and the tone mapping debug views both bind it.
- Motion vectors exist (`GBufferVelocity`, `velocity = currentUv - previousUv`) with no consumer.
  The camera block already carries `prevViewProj` and `invViewProj`.
- `ShadeSurface` multiplies only the ambient term by the material occlusion `ao` (`pbr_common.glsl`).
- `RenderDebugSettings` is the editor-to-renderer channel for renderer switches, handed over by
  copy each frame and not persisted.

## Decisions

1. **AO only, applied to ambient.** The lighting pass multiplies the material occlusion by the
   screen-space result before `ShadeSurface`, so direct light and emissive are untouched. Blend
   surfaces are shaded by the forward pass and get no screen-space AO.
2. **Full resolution, two compute passes.** `AoTrace` writes the raw visibility, `AoResolve`
   filters it spatially and temporally into `SceneAo`. Half resolution is a later optimisation if
   measurements ask for it.
3. **Fixed algorithm choices.** Cosine-weighted sectors (the ambient integral is cosine-weighted)
   and the geometric normal for the hemisphere (the depth buffer does not follow the normal map, so
   the shading normal self-occludes). The spike's toggles for both are dropped.
4. **The pass order is static; content is per frame.** Both AO passes are always in the deferred
   order. With AO disabled the trace records nothing and the resolve writes 1.0 to `SceneAo`, so the
   lighting pass and the debug view never read undefined contents and need no flag of their own.
   This mirrors how `forwardFilter` travels in the frame context while the order decides only order.
5. **Temporal history lives outside the tracker.** Two history images, ping-ponged, owned by the
   resolve pass and always in `VK_IMAGE_LAYOUT_GENERAL`. Which one is read, which one is written
   and whether the read one is valid are decided by a pure `AoHistory` unit the renderer owns and
   hands over in the frame context. Passes stay free of per-frame state.
6. **Settings are renderer switches, not persisted.** `AoSettings` joins `RenderDebugSettings` and
   is edited in the Graphics Debug window. AO is on by default.
7. **Forward-only comparison.** The forward-only order runs neither AO pass, so with AO enabled the
   two orders differ by the AO term. Pixel equivalence between them holds with AO disabled, which
   is the setting the comparison is made in.

## Render Targets

| Id | Kind | Format | Usage | Written by | Read by |
|----|------|--------|-------|------------|---------|
| `AoRaw` | Storage | `R32_SFLOAT` | storage, sampled | AoTrace | AoResolve |
| `SceneAo` | Storage | `R32_SFLOAT` | storage, sampled | AoResolve | Lighting, Tonemap (debug view) |

Both are transient, one copy per frame slot, like every target but `SceneLdr`. `R32_SFLOAT` is in
the core list of formats that must support storage, so no `shaderStorageImageExtendedFormats`
feature is needed; the spike measured no reason to go narrower.

A new `RenderTargetKind::Storage` has write layout `VK_IMAGE_LAYOUT_GENERAL`. The renderer's
`AccessMaskForLayout` maps `GENERAL` to shader read and write, and `StageMaskForLayout` maps it to
the compute stage. Those two are the only places that turn a layout into barrier masks, so the
tracker needs no other change.

`SceneAo` is appended to `VulkanGBufferDescriptors::kInputs` (binding 6) and to
`gbuffer_inputs.glsl` as `sceneAo`, so the lighting pass and the tone mapping pass both see it. The
tone mapping pass adds it to its declared reads, as it already does for every other set 2 input.

### History images

Two `R16G16B16A16_SFLOAT` storage images at the scene extent (a core storage format), holding
`(ao, view distance, sample count, unused)`. The resolve pass creates them in its constructor and in
`OnTargetsRebuilt`, and destroys them with the pass. They are shared across frame slots: frames on
the one graphics queue execute in submission order, and the barrier below orders each frame against
the previous one.

At the head of `AoResolve::Record` one barrier covers both images, compute stage to compute stage:

- history invalid: `oldLayout = UNDEFINED`, `newLayout = GENERAL`. Discarding the contents is
  correct, since nothing will read them, and it is the transition a freshly created image needs.
- history valid: `GENERAL` to `GENERAL`, shader write to shader read and write, a memory-only
  barrier that makes last frame's write visible and orders this frame's write after last frame's
  read.

That is why the pass needs no "have I transitioned these yet" flag: every frame after creation or
reset reports invalid history first.

## Data Flow

```
Geometry --depth, GB normal--> AoTrace --AoRaw--> AoResolve --SceneAo--> Lighting
                                                    ^   |
                            velocity, depth --------+   +--> history[write]
                            history[read] ----------+
```

### `AoHistory` (pure, `engine/renderer/ao_history.h`)

```cpp
struct AoHistoryFrame
{
    uint32_t readIndex = 0;   // history image the resolve samples
    uint32_t writeIndex = 1;  // history image the resolve writes
    bool valid = false;       // readIndex holds last frame's accumulation
};

class AoHistory
{
  public:
    // Called once per recorded frame. accumulating is whether this frame's resolve writes history
    // (AO and the temporal filter both enabled).
    AoHistoryFrame Advance(bool accumulating);
    // The next Advance reports invalid history.
    void Reset();
};
```

Rules:

- The first `Advance`, and the first after `Reset`, is invalid.
- A frame that does not accumulate reports invalid and makes the next frame invalid too, so turning
  AO or the temporal filter off and on again starts from scratch rather than from stale history.
- Consecutive accumulating frames alternate `writeIndex` 0/1, and each reads what the previous one
  wrote.

The renderer calls `Reset` wherever it resets `MotionHistory` (scene target rebuild, swapchain
recreation), because those recreate the history images too. It calls `Advance` once per frame, in
`DrawFrame`, after the acquire can no longer bail out, so a frame that is never recorded never
advances the history.

### Frame context

`ScenePassFrameContext` gains `AoSettings ao`, `AoHistoryFrame aoHistory`, and `uint32_t
frameIndex`, a counter the renderer increments per recorded frame for the trace's noise. The
forward-only order forces `ao.enabled = false` the same way it forces the debug view off.

### Settings

```cpp
struct AoSettings
{
    bool enabled = true;
    float radius = 1.5f;     // world-space search radius, metres
    float thickness = 0.25f; // assumed thickness of every depth sample, metres
    int sliceCount = 2;
    int stepCount = 8;       // per side of each slice
    bool spatialFilter = true;
    bool temporalFilter = true;
};
```

The defaults are the spike's. The UI clamps radius to [0.1, 5] m, thickness to [0.01, 2] m, slices
to [1, 4] and steps to [2, 16]; the pass clamps again when it builds its push constants so a bad
value from anywhere cannot reach the shader.

### Shaders

- `vbao_common.glsl`: the push constant block (extent, inverse extent, radius, thickness, max pixel
  radius 256, slice and step counts, frame index, flags) and the view-space reconstruction helpers.
  Flags: spatial, temporal, history valid, enabled.
- `vbao_trace.comp`: the spike's trace with the two dropped branches removed (always cosine
  weighted, always the geometric normal). Sky and pixels whose projected radius is under one pixel
  write 1.0.
- `vbao_resolve.comp`: when the enabled flag is clear, writes 1.0 to `SceneAo` and returns. Otherwise
  the spike's 5x5 depth-aware spatial filter, then exponential accumulation up to 16 samples,
  reprojected through `uv - velocity` and rejected when the history's stored view distance differs
  from the expected one by more than 5%. The expected distance assumes the surface did not move, so
  a moving object rejects more history than a static one; its AO is then noisier while it moves, and
  settles within 16 frames once it stops.
- `deferred_lighting.frag`: `float ao = surface.b * texture(sceneAo, fragTexCoord).r;`.
- `tonemap.frag`: a new debug view `GBufferDebugView::AmbientOcclusion = 7` showing `SceneAo`.

### Passes

`VulkanAoTracePass` and `VulkanAoResolvePass` in `engine/renderer/vulkan/ao_pass.{h,cpp}`, each an
`IScenePass` with its own compute pipeline, sampler and per-slot descriptor sets (set 0 the camera
block, set 1 its inputs), following `VulkanExposureHistogramPass`.

| Pass | Reads | Writes |
|------|-------|--------|
| AoTrace | SceneDepth, GBufferNormal | AoRaw |
| AoResolve | AoRaw, SceneDepth, GBufferVelocity | SceneAo |

The resolve pass's descriptor sets are indexed by frame slot and read-history index, since the
history image it samples and the one it stores to swap each frame.

New deferred order: Geometry, AoTrace, AoResolve, Lighting, Forward, ExposureHistogram, Tonemap.

### Editor

The Graphics Debug window gains an "Ambient occlusion" section: Enabled, Radius, Thickness, Slices,
Steps, Spatial filter, Temporal filter, and a Reset button. The viewport output combo gains
"Ambient occlusion". The section is disabled while forward only is on, like the debug views.

## Error Handling

- Storage-image format support is checked by `ChooseFormat` like every other target. `R32_SFLOAT`
  and `R16G16B16A16_SFLOAT` storage support is mandatory, so a failure there is a broken driver and
  throws, as for the velocity target.
- A target appended to `RenderTargetId` without a description still throws where it is created.
- Pipeline and descriptor creation failures surface through `CheckVulkan`, and each pass unwinds its
  own handles as the existing passes do.

## Automated Verification

1. `miniengine.ao_history` (new): the first frame and the first after `Reset` are invalid;
   consecutive accumulating frames alternate indices and are valid from the second; a
   non-accumulating frame invalidates itself and the next.
2. `miniengine.scene_pass`: `AoRaw` and `SceneAo` are storage targets with the `GENERAL` write
   layout; a storage write then a read produces a `GENERAL` to `SHADER_READ_ONLY_OPTIMAL`
   transition; the deferred order places AoTrace and AoResolve between Geometry and Lighting; the
   forward-only order contains neither.
3. Shader compilation of the three new or changed shaders as part of the build.
4. Debug build with validation layers: zero messages with AO on and off, with the temporal filter
   toggled, while resizing the viewport, and while flipping forward only.

## Manual Acceptance

On Sponza at 1920x1080:

- Viewport output "Ambient occlusion" shows contact darkening in corners, under arches and around
  columns, and no full-screen haloing behind thin geometry such as the banners.
- Toggling AO changes only ambient-lit areas; directly lit surfaces keep their brightness.
- Moving the camera leaves no ghosting trails after a second, and disocclusion edges resolve within
  16 frames.
- AO off, deferred and forward only match (the existing comparison).
- GPU cost of AoTrace plus AoResolve reported from a throwaway timestamp capture that is not
  committed, together with the frame time with AO off and on.

## Out of Scope

Bent normals, screen-space indirect light, half-resolution tracing, TAA jitter, persisting AO
settings, AO on Blend surfaces, and using AO to occlude direct light.
