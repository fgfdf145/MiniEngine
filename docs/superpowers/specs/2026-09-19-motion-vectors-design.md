# Motion Vectors Design

## Goal

Write a per-pixel screen-space motion vector for every opaque and mask surface into the G-buffer,
covering both camera motion and object motion, and show it as a viewport debug view. Nothing
consumes the vectors yet: TAA and temporal ambient occlusion are separate projects that will read
them. The shaded image must stay pixel-identical, in both the deferred and the forward-only order.

The approach was proven in the VBAO feasibility spike (branch `spike/vbao`, commit `f8c47e3`):
an `R16G16_SFLOAT` target written by the geometry pass, the previous frame's view-projection in the
camera block, and each draw's previous model matrix in a storage buffer indexed by the draw's
`firstInstance`. This design keeps those three decisions and replaces the spike's per-index
bookkeeping with keyed history.

## Current State

- `ObjectPushConstants` (`engine/renderer/material.h`) is exactly 128 bytes, the Vulkan guarantee:
  the model matrix plus `MaterialPushConstants`. There is no room for a second matrix.
- Set 0 (`VulkanFrameDescriptorSetLayout`) holds the camera uniform buffer at binding 0 and the
  shadow map at binding 1, one set per swapchain image, owned by `VulkanUniformBuffer`. That object
  is rebuilt on every content upload (`ApplyRenderContent`) and on descriptor recreation.
- The geometry pass writes four color targets (GB0-GB3) plus depth. `kMaxMaterialColorAttachments`
  is 4. Blend draw items never enter the geometry pass; the forward pass composites them.
- `RecordMaterialDrawItems` draws every item with `firstInstance = 0`. `triangle.vert` serves both
  the geometry and forward pipeline sets; the shadow pass has its own shaders.
- The camera block (`CameraBuffer`, `CameraUniformData`) ends with `invViewProj`, and static
  assertions pin its std140 layout.
- Render debug controls live in the Graphics Debug window (`DrawGraphicsDebugPanel`), whose
  "Viewport output" combo maps onto `GBufferDebugView`.

## Locked Decisions

1. **Camera and object motion.** Every draw carries its previous model matrix, so dragging an
   object with the gizmo produces correct vectors, not just camera movement.
2. **Previous model matrices live in a storage buffer at set 0 binding 2, indexed by
   `firstInstance`.** Push constants stay untouched, so the material system, the shadow pass and
   the phase two pixel-equivalence baseline are unaffected. Moving the material factors out of
   push constants, or moving both model matrices into a per-draw buffer, were rejected as changes
   far larger than motion vectors need.
3. **History is keyed, not indexed.** The previous matrix of a draw is found by the key
   (entity, submesh ordinal within that entity), so a content reload that reorders or replaces
   submeshes can never hand a draw another draw's matrix. A key with no history reports no motion.
4. **Encoding:** `velocity = currentUv - previousUv`, in UV units of the scene target, computed
   from unjittered clip positions (there is no jitter yet). A consumer finds the previous position
   as `uv - velocity`.
5. **Scope ends at the debug view.** No consumer, no jitter, no cross-frame render target.

## Render Target

| Id | Format | Usage | Written by | Read by |
|---|---|---|---|---|
| `GBufferVelocity` | `R16G16_SFLOAT` | Color attachment, Sampled | Geometry pass, location 4 | Tone mapping debug view |

It is appended to `RenderTargetId` after `GBufferEmissive`, is a color target to the layout
tracker, and is transient (one copy per frame slot) like the rest of the G-buffer. The entity id
target the G-buffer spec reserves for its phase three is appended after it when that phase lands.
Format selection goes through `ChooseFormat` with color attachment and sampled features;
`R16G16_SFLOAT` has no fallback because both features are mandatory for it in Vulkan.

Half precision is relative (about 2^-11): at 4K a 10-pixel motion is held to about 0.005 pixels
and a full screen width to about 2 pixels. Temporal consumers reject history at that speed
anyway, so the error lands where it cannot matter.

**Background pixels** (depth == 1) are cleared to 0 and hold no motion vector. The sky does move
under camera rotation; a consumer that needs it reprojects from depth and `prevViewProj`, which is
exact for a pixel at infinity. **Blend surfaces** do not write the G-buffer, so the vector under a
glass pane is the one of the opaque surface behind it.

## Data Flow

### Camera

`CameraUniformData` and `CameraBuffer` append `mat4 prevViewProj` after `invViewProj`: last
frame's `renderProjection * view`. The size assertion grows by 64 bytes and a new `offsetof`
assertion pins its position.

### Previous model matrices

- Set 0 gains binding 2: `readonly buffer PreviousModelBuffer { mat4 previousModels[]; }`,
  vertex stage only, declared in `triangle.vert` alone.
- `VulkanUniformBuffer` owns one host-visible, coherent storage buffer per swapchain image, next to
  the camera buffer, sized to `max(1, submeshCount)` matrices. Because the object is rebuilt on
  every content upload, the capacity always matches the submesh list and there is no fixed limit.
  `Update` writes the matrices along with the camera block.
- A draw's slot is its index in `m_renderSubmeshes`. `VulkanDrawItem` carries it as `motionSlot`
  and `RecordMaterialDrawItems` passes it as `firstInstance`; `triangle.vert` reads
  `previousModels[gl_InstanceIndex]`. Direct draws accept any `firstInstance` without a feature.
- `triangle.vert` outputs the current and previous clip positions; `gbuffer.frag` divides both per
  fragment and writes `(currNdc - prevNdc) * 0.5`. `triangle.frag` ignores the extra outputs.

### History: `MotionHistory`

A pure unit in `engine_render_core` (`engine/renderer/motion_history.h`), compiled into a unit
test without Vulkan:

```cpp
struct MotionKey
{
    uint32_t entity = 0;
    uint32_t submeshOrdinal = 0;
};

struct MotionFrame
{
    glm::mat4 previousViewProjection{1.0f};
    std::vector<glm::mat4> previousModels; // parallel to the models passed in
};

class MotionHistory
{
  public:
    // Returns last frame's matrices for this frame's draws, then remembers this frame's.
    MotionFrame Advance(const glm::mat4& viewProjection, std::span<const MotionKey> keys, std::span<const glm::mat4> models);
    // The next Advance reports no motion for the camera or any draw.
    void Reset();
};
```

Rules:

- A key present last frame gets last frame's model; a key absent last frame gets its current
  model, which is zero motion. Keys absent this frame are forgotten.
- The first `Advance`, and the first after `Reset`, returns the current view-projection.
- Keys must be unique within a frame; `Advance` throws `std::invalid_argument` otherwise, since a
  duplicate would silently give one draw the other's history.
- The renderer calls `Reset` whenever the scene targets are rebuilt: a new extent is a new
  projection, and without the reset the first frame after a resize would report the projection
  change as full-screen motion.

The renderer computes each `RenderSubmesh`'s `MotionKey` once, in `ApplyRenderContent`: the entity
from `entt::to_integral`, and the ordinal by counting earlier submeshes of the same entity in list
order.

## Debug View

`GBufferDebugView::MotionVectors = 6`, listed in the Graphics Debug window as
"G-buffer: motion vectors". `tonemap.frag` converts the vector to pixels and shows
`rg = clamp(0.5 + pixels / 32, 0, 1)`, `b = 0.5`: mid-grey is still, red grows with rightward
motion, green with downward motion, and the scale saturates at 16 pixels per frame. The view is
forced off in the forward-only order like every other G-buffer view.

Set 2 (`VulkanGBufferDescriptors::kInputs`, `gbuffer_inputs.glsl`) gains `gbufferVelocity` at
binding 5. The tone mapping pass declares it among its reads; the lighting pass already declares
all of `kInputs`, and sampling nothing from it costs nothing.

## Failure Handling

- No `R16G16_SFLOAT` color attachment support: `ChooseFormat` throws with the target named, as for
  every other target. The format is mandatory, so this is a broken driver, not a fallback case.
- Storage buffer allocation failures surface through `CheckVulkan`, as for the camera buffer.
- Duplicate motion keys throw from `MotionHistory::Advance` (a renderer bug, not a runtime state).

## Automated Verification

1. `miniengine.motion_history` (new): first frame and post-reset frames report no motion; a
   second frame reports the previous matrices; a new key reports none; a reordered key list keeps
   each key's own history; a removed and re-added key reports none; duplicate keys throw.
2. `miniengine.scene_pass`: `GBufferVelocity` is a color target with the color attachment write
   layout.
3. Compile-time: the `CameraUniformData` size and offset assertions.
4. Debug build with validation layers: zero messages while moving the camera and objects.

A throwaway capture harness on a local branch (never merged) additionally checks:

5. **Pixel equivalence:** the shaded image before and after this change is identical, in both
   orders, at a fixed camera over Sponza.
6. **Object motion:** with the camera still and one object translated by a known world distance
   per frame, the vector over that object matches `distance * |proj[1][1]| * height / (2 * depth)`
   pixels, and the rest of the frame reads zero.
7. **Camera motion:** a translating camera gives a smooth field that grows with inverse depth.

## Manual GUI Acceptance

1. Graphics Debug -> Viewport output -> "G-buffer: motion vectors": a still camera shows uniform
   mid-grey.
2. Moving or rotating the camera (WASD, right mouse) colors the frame, nearer surfaces more
   strongly for translation.
3. Dragging an object with the gizmo while the camera is still colors only that object.
4. Resizing the viewport produces no full-screen flash of motion.
5. "Shaded" output looks exactly as before; forward-only still works and disables the view.

## Out of Scope

- Any consumer: TAA, temporal AO, motion blur.
- Projection jitter and the jittered/unjittered split it would need.
- Velocity for Blend surfaces and for background pixels.
- Skinned or morph-target animation (the engine has neither).
- Render targets that persist across frames, and the layout tracker changes they require.
