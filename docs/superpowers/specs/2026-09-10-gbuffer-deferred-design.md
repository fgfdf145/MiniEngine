# G-Buffer and Deferred Shading Design

## Goal

Replace the single forward shading pass with a deferred path: a geometry pass
that writes surface and material data into a G-buffer, a full-screen lighting
pass that consumes it, a forward pass retained for alpha-blended materials,
and a tone mapping pass that produces the editor viewport image.

The delivery boundary is pixel equivalence. This design adds no new visual
effect. Its output is the foundation that clustered lighting, screen space
ambient occlusion, screen space reflections, ray traced shadows and temporal
antialiasing are later built on, plus the one editor capability that falls
out of a G-buffer for free: pixel-accurate entity picking.

This design supersedes sub-project P4 of
`2026-09-04-raytracing-hybrid-pipeline-design.md`. P4 specified an HDR
intermediate target, `SAMPLED_BIT` on depth, a multi-pass and barrier
abstraction extracted from `RecordSceneLayer`, and tone mapping moved out of
`triangle.frag`. All four are delivered here as phase one. P0 through P3 and
P5 of that document are unaffected and keep their stated ordering relative to
each other.

## Current Frame Structure

A frame records two render passes:

1. `VulkanSceneViewport`'s pass writes one color attachment and one depth
   attachment, iterating submeshes with one `vkCmdDrawIndexed` each.
2. `VulkanRenderPass` writes the swapchain image with ImGui draw data.

The facts that constrain this work:

- `shaders/vulkan/triangle.frag` performs material sampling, two-layer
  blending, TBN normal mapping, Cook-Torrance shading for five light types,
  and Reinhard tone mapping in a single 300-line entry point.
- The viewport color attachment is the swapchain's sRGB format. There is no
  HDR intermediate target, so tone mapping cannot be separated from shading
  without one.
- The viewport depth attachment lacks `VK_IMAGE_USAGE_SAMPLED_BIT`.
- `ObjectPushConstants` is exactly 128 bytes, the Vulkan minimum guarantee
  for `maxPushConstantsSize`. Within it, `nodeGraphFactors.z` and
  `nodeGraphFactors.w` are declared but read by no shader.
- `VulkanUniformBuffer` owns one descriptor set layout holding both the
  camera uniform buffer and thirteen combined image samplers, and allocates
  one set per swapchain image per material.
- `VulkanSceneViewport` owns the targets, the render pass, the framebuffers
  and the sampler ImGui reads from, as one unit.
- `engine/renderer/CMakeLists.txt` compiles exactly two hard-coded shader
  file names.
- `RecordSceneLayer` is one function containing the entire scene pass. The
  frame records zero pipeline barriers.
- Entity picking in `editor_viewport_panel.cpp` projects entity bounds
  centers to screen space and selects by hit radius and bounds padding. It is
  an approximation, not a per-pixel test.
- The projection matrix is `glm::perspectiveRH_ZO` with `[1][1]` negated, so
  depth is `0..1` and the Y flip lives in the projection matrix.
- Validation layers are enabled in Debug builds.

## Locked Decisions

1. **Pixel equivalence is the acceptance criterion.** No new visual effect
   ships here. Clustered lighting, ambient occlusion and every other effect a
   G-buffer enables are separate projects.
2. **Five color targets plus depth.** Albedo, normal, surface, emissive and
   entity id. Emissive gets a dedicated target rather than being written into
   the HDR target under additive blending, so every target has one owner and
   can be visualized alone.
3. **Octahedral normal encoding.** Two channels, higher precision per byte
   than a three-channel `10:10:10` store, at the cost of being unreadable in
   a capture without decoding.
4. **The entity id target is resident.** It is written every frame rather
   than only on the frame a click occurs. The 4 bytes per pixel buy
   pixel-accurate picking now and selection outlining later.
5. **Transient targets are allocated per frame in flight, not per swapchain
   image.** The G-buffer, depth and HDR targets are produced and consumed
   inside one command buffer and need `kMaxFramesInFlight` copies. Only the
   final low dynamic range target, which ImGui samples, stays indexed by
   swapchain image.
6. **A lightweight pass interface with an explicit resource table.** Each
   pass declares which targets it reads and writes; one helper turns those
   declarations into layout transitions. No automatic dependency inference,
   no resource aliasing. Pass order is a written list.
7. **Three delivery phases, each independently verifiable.** Phase one and
   phase three do not depend on phase two landing correctly.
8. **A forward comparison toggle ships with phase two.** The forward path
   must exist regardless, because alpha-blended materials cannot be deferred.
   Letting it also draw opaque geometry turns pixel equivalence from a
   judgment call into a switch the reviewer can flip.

## Render Targets

`SceneRenderTargets` replaces `VulkanSceneViewport`'s combined ownership. It
owns images, views and each target's current layout. Render passes and
framebuffers move to the passes that use them.

| Target | Format | Usage | Copies |
| --- | --- | --- | --- |
| GB0 Albedo | `R8G8B8A8_SRGB` | Color, Sampled | 2 |
| GB1 Normal | `R16G16_SFLOAT` | Color, Sampled | 2 |
| GB2 Surface | `R8G8B8A8_UNORM` | Color, Sampled | 2 |
| GB3 Emissive | `B10G11R11_UFLOAT_PACK32` | Color, Sampled | 2 |
| GB4 EntityId | `R32_UINT` | Color, Transfer source | 2 |
| Depth | `D32_SFLOAT` | Depth stencil, Sampled | 2 |
| HDR | `R16G16B16A16_SFLOAT` | Color, Sampled | 2 |
| Viewport | existing sRGB format | Color, Sampled | swapchain images |

Two copies means `VulkanCommandContext::kMaxFramesInFlight`, which is
currently private and must be promoted to the public section for
`SceneRenderTargets` to size itself against it. The viewport target keeps the
existing per-swapchain-image indexing because ImGui samples it after the
scene passes complete. The two indexing schemes coexist deliberately.

**Amended after phase one.** This originally claimed that separate accessors
mean "a caller cannot pass the wrong index". They do not. `GetImage` and
`GetView` call `.at()` on the requested target's own vector, which catches an
index that is out of range for that target and nothing else. With two
transient copies against typically three swapchain images, the confusion that
actually happens — a frame slot where an image index belongs — is in range
and passes silently. What prevents it is `ResolveIndex`: every caller holding
both an index and a slot routes through it instead of choosing one itself, so
the rule lives in the one class that decided it.

A phase-two consideration: making the two indices distinct types (rather than
both `uint32_t`) is the only option that also catches passing the wrong
`RenderTargetId` to `ResolveIndex`, since that call takes the target and the
index separately and cannot check them against each other. That mistake has
already occurred once during phase one and was caught by hand, not by the
compiler.

Format selection follows the existing `FindDepthFormat` pattern: query
`vkGetPhysicalDeviceFormatProperties` and fall back rather than assume.
`B10G11R11_UFLOAT_PACK32` falls back to `R16G16B16A16_SFLOAT`. The depth
format candidate list gains the requirement that the chosen format also
report `VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT`. `R32_UINT` is never sampled
through a sampler, only copied, so it needs no filtering support.

Memory cost is stated rather than hidden. At a 1600x900 viewport the
transient targets cost 32 bytes per pixel across two copies and the viewport
target 4 bytes per pixel across three, roughly 110 MB against the 35 MB the
current two targets use. The viewport is an ImGui panel and is usually
smaller than the window, but the ratio holds.

## Pass Interface and Barriers

```cpp
struct RenderPassIo
{
    std::span<const RenderTargetId> reads;
    std::span<const RenderTargetId> writes;
};

class IScenePass
{
  public:
    virtual ~IScenePass() = default;
    virtual RenderPassIo Io() const = 0;
    virtual void Record(
        VkCommandBuffer commandBuffer,
        SceneRenderTargets& targets,
        const FrameContext& frame) = 0;
    virtual void OnResize(const RenderExtent& extent) = 0;
};
```

Reads resolve to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`. Writes resolve
to `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`, or to
`VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL` for the depth target. A
target may not appear in both spans of one pass.

The frame function becomes a loop: for each pass, apply the transitions its
declaration requires, then record it.

Layout tracking is separated from Vulkan command recording so it can be
tested. `RenderTargetLayoutTracker` holds the current layout of every target
and, given a `RenderPassIo`, returns the list of transitions needed to
satisfy it. It calls no Vulkan entry point. The caller turns each returned
transition into a `vkCmdPipelineBarrier` and reports the result back to the
tracker.

A target already in the required layout produces no transition. Applying the
same declaration twice in a row produces transitions the first time and none
the second.

Adding a pass later means writing one class and inserting one line in the
pass list. It does not mean reasoning about layouts again.

## G-Buffer Encoding

| Channel | Contents |
| --- | --- |
| GB0.rgb | Albedo, with two-layer blending, vertex color and base color factor already resolved. Linear values are written; the `_SRGB` format encodes and decodes them in hardware. |
| GB0.a | Unused. Write `1.0`. |
| GB1.rg | Shading normal, octahedral encoded. |
| GB2.r | Metallic, after the surface factor multiply and clamp. |
| GB2.g | Roughness, after the surface factor multiply and clamp to `[0.04, 1]`. |
| GB2.b | Occlusion, after the occlusion strength mix. |
| GB2.a | Reserved. Write `0`. |
| GB3.rgb | Emissive, already multiplied by the emissive factor. |
| GB4.r | `entt::to_integral(entity)`. |

GB2.a is reserved and unused on purpose. Alpha mode does not need to survive
into the lighting pass: Mask fragments are discarded during the geometry
pass, and Blend fragments never enter the G-buffer.

Encoding and decoding functions live in `shaders/vulkan/gbuffer_common.glsl`.
The C++ format table carries a comment pointing at that file. There is no
reflection; the two sides are kept aligned by convention and by the manual
capture check in acceptance.

### Entity id transport

The geometry pass needs the entity id per draw, and the push constant block
is full. The id travels in `nodeGraphFactors.z`, recovered in the shader with
`floatBitsToUint`. This matches the packing already used for `alphaCutoff`,
which occupies the emissive vector's fourth component for the same reason.
`ObjectPushConstants` stays at 128 bytes and its existing static assertions
stand; a new assertion pins the id's offset.

GB4 clears to `0xFFFFFFFF`, which is the integral value of `entt::null` for
the default 32-bit entity type. A value read back from GB4 is converted with
`entt::entity{raw}` and validated against the registry before use.

### Background pixels

The lighting pass runs over every pixel, including those no geometry wrote.
A pixel whose depth equals `1.0` outputs the background radiance
`{0.086957, 0.111111, 0.190476}`, with alpha `1.0`.

**Amended after phase one.** This section originally said `{0.08, 0.1, 0.16}`,
the pre-phase-one clear color. That was correct only while the clear reached
the display unmodified. It now lands in the HDR target and is tone mapped
with everything else, so phase one pre-divided it: `c / (1 - c)` is the
inverse of Reinhard, and those are the radiance values whose Reinhard result
is `{0.08, 0.1, 0.16}`. The values above are what
`VulkanForwardPass::Record` already clears to, and the lighting pass must
output the same ones. Writing the displayed color here would reproduce
exactly the bug phase one fixed, with the spec as its alibi.

Preserving the displayed color and alpha is part of pixel equivalence: the
viewport image's alpha channel is consumed by ImGui and has already been the
subject of one fix.

Shaded pixels also output alpha `1.0`. Opaque and Mask fragments are fully
covered by definition, so no alpha value needs to survive the G-buffer.

### World position reconstruction

World position is reconstructed from depth, not stored. The camera uniform
buffer gains `invViewProj`.

```text
ndc   = vec3(uv * 2.0 - 1.0, depth)
world = invViewProj * vec4(ndc, 1.0)
world /= world.w
```

`uv` is the standard texture coordinate with its origin at the top left. No
Y flip is applied here: the projection matrix already negates `[1][1]`, so
the image's top row is `ndc.y == -1` and this expression is consistent with
it. Depth is `0..1` because the projection is `perspectiveRH_ZO`.

## Shader Decomposition

`triangle.frag` splits at the boundary between resolving material values and
evaluating lighting.

| File | Origin and role |
| --- | --- |
| `gbuffer.frag` | The material half: sampling, two-layer blending, TBN normal mapping, alpha mask discard. Writes five targets. Keeps the `kAlphaMask` specialization constant. |
| `deferred_lighting.frag` | The lighting half: `EvaluateSceneLight`, `EvaluateBRDF`, ambient accumulation, emissive addition. Full-screen. Outputs linear HDR. Performs no tone mapping. |
| `triangle.frag` | Retained for Blend materials. Full original logic minus the Reinhard operator. Outputs linear HDR. |
| `tonemap.frag` | Full-screen. Samples the HDR target, applies Reinhard, writes the viewport target. Also serves the G-buffer debug views, selected by a push constant mode. |
| `fullscreen.vert` | Generates a full-screen triangle from `gl_VertexIndex`. No vertex buffer, no vertex input state. |
| `pbr_common.glsl` | The BRDF and attenuation helpers, shared by `deferred_lighting.frag` and `triangle.frag`. |
| `gbuffer_common.glsl` | G-buffer encode and decode. |

The Reinhard operator moves file but not form. Phase one keeps it
byte-identical so that the phase one acceptance criterion is genuine pixel
equivalence rather than an approximate match.

Shaders including other shaders requires
`#extension GL_GOOGLE_include_directive : require`.

`engine/renderer/CMakeLists.txt` replaces its two hard-coded file names with
a list. Included `.glsl` files are listed explicitly in each compile
command's `DEPENDS` rather than generated through `glslc -MD` and CMake's
`DEPFILE`, whose Visual Studio generator support is version-dependent. Three
include files do not justify that risk.

## Descriptor Sets

`VulkanUniformBuffer`'s single layout splits by update frequency.

| Set | Contents | Consumers |
| --- | --- | --- |
| 0 | Camera uniform buffer, extended with `invViewProj` | Geometry pass, forward pass, lighting pass |
| 1 | Thirteen material combined image samplers | Geometry pass, forward pass |
| 2 | GB0 through GB3 plus depth, as combined image samplers | Lighting pass |
| 0 (tone mapping pass's own layout) | HDR target sampler | Tone mapping pass |

Set 0 and set 1 are allocated per swapchain image as today, set 2 and the
tone mapping pass's set per frame in flight, matching their targets' copy
counts.

**Amended after phase one.** This table originally locked the tone mapping
pass's HDR sampler at set 3 and gave set 0's consumers as "Every pass". The
shipped `VulkanTonemapPass` declares its sampler at set 0 of its own pipeline
layout and binds no camera set: that pass consumes no camera data and no
material data, so reaching index 3 would mean three filler set layouts for
nothing. Set indices are per pipeline layout, not global, so nothing forces
the two to agree.

Phase two's tone mapping debug views change this. They are selected per
G-buffer target, so that pass will want set 2 — the G-buffer inputs — and a
pipeline layout covering 0 through 2 with a filler at 1, because Vulkan
requires every set index below the highest used one to be declared. Moving
the HDR sampler off set 0 at that point is a larger change than adding set 2
beside it, so it stays at 0.

`ObjectPushConstants` is unchanged and shared by the geometry and forward
passes. The lighting and tone mapping passes use their own small push
constant blocks.

This split is the single most error-prone step in the project, which is why
it lands in phase one while the forward path is still the only path and any
mistake is immediately visible.

## Forward Comparison Toggle

`RendererSharedState` gains a boolean, surfaced as an editor checkbox. It
selects between two pass orders:

```text
deferred:     GBuffer, Lighting, ForwardBlend, Tonemap
forward only: ForwardAll, Tonemap
```

`ForwardAll` and `ForwardBlend` are the same pass with a different draw
filter: all non-Blend and Blend items, or Blend items only. Both orders end
in the same tone mapping pass, so a comparison isolates shading differences
and never confounds them with tone mapping.

`BuildScenePassOrder` is a free function over a pass id enum, independent of
any Vulkan object, so the two orders are unit testable.

## Entity Id Picking

GB4 carries `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`. When the cursor is inside the
viewport, the frame records a `vkCmdCopyImageToBuffer` of a single 1x1 region
at the cursor position into a host-visible staging buffer, one per frame in
flight. Copying one pixel rather than the image keeps the cost negligible.

The value is read after that frame slot's existing fence is waited on, which
the frame loop already does before reusing the slot. Results therefore lag by
`kMaxFramesInFlight` frames. No additional synchronization and no GPU stall
is introduced. Two frames of latency is imperceptible for click selection.

A cursor outside the viewport records no copy and leaves the previous value
untouched; the reader treats a stale-slot value as no hit. A slot that has
never been submitted holds no readback at all, which is the only case the
reader must guard before the existing fence wait applies.

Picking becomes hybrid, not replaced. Lights are drawn as gizmo overlays and
never enter the G-buffer, so the existing projected-center path in
`editor_viewport_panel.cpp` is retained for light entities. Mesh entities
resolve through GB4, light gizmos through projection.

A light gizmo hit takes priority over a GB4 hit. Gizmos are overlays drawn
over the scene, so what the user sees on top is what a click selects, and an
occluded light stays selectable exactly as it is today. This rule avoids
comparing a gizmo's screen-space depth against a G-buffer depth, which would
require copying a second pixel and reconciling two depth conventions for no
behavioral gain.

## Failure Handling

- A target format with no supported fallback aborts renderer creation with a
  clear message, following the existing `FindDepthFormat` failure.
- Target creation is all-or-nothing per resize: new images are built before
  old ones are released, and a failure leaves the previous set live.
- A pass declaring a target in both `reads` and `writes` is a programming
  error and throws `std::runtime_error`, in every configuration. Amended
  after phase one: this originally said it asserts in Debug builds. An
  assert is untestable, and a Release build would go on to record a
  contradictory declaration in silence. Throwing also lets the rule be
  covered by a unit test, which `TargetInBothSpansIsRejected` does.
- An entity id read back from GB4 that fails `registry.valid()` is treated as
  no hit, not as a selection change. Entity recycling between the recording
  frame and the reading frame is the expected cause.
- A staging readback whose fence has not been signalled is skipped rather
  than waited on.
- Shader compilation failure remains a build-time failure; no runtime shader
  fallback is introduced.
- The existing in-flight-frame wait before destroying descriptor and pipeline
  resources applies unchanged to the new descriptor sets.

## Delivery Phases

| Phase | Scope | Acceptance |
| --- | --- | --- |
| 1 | Generalized shader compilation, `SceneRenderTargets`, `RenderTargetLayoutTracker`, the pass interface, the HDR target, tone mapping as its own pass, the descriptor set split | Pixel equivalence with the current image; the forward path is still the only path |
| 2 | Geometry pass, lighting pass, forward pass reduced to Blend materials, the comparison toggle | Opaque and Mask geometry matches phase one; toggling the comparison switch produces no visible change |
| 3 | GB4, the staging readback, hybrid picking | Mesh selection agrees with the previous approximation and is correct at silhouette edges where the approximation was not |

Phase two is a code move: the arithmetic in `deferred_lighting.frag` is the
arithmetic already in `triangle.frag`. Any color difference it produces is a
transport error in the G-buffer encoding, the descriptor bindings or the
position reconstruction, not a design error.

## Automated Verification

Add `miniengine_scene_pass_tests`, registered as `miniengine.scene_pass`. It
covers both pure-logic units introduced here, `RenderTargetLayoutTracker` and
`BuildScenePassOrder`, neither of which calls a Vulkan entry point. Tests
cover:

1. A target starting undefined and declared as a write transitions to the
   attachment layout appropriate to its kind.
2. A target declared as a read after being written transitions to shader read
   only.
3. A target already in the required layout produces no transition.
4. Applying one declaration twice produces transitions once.
5. The depth target resolves to the depth stencil attachment layout, not the
   color attachment layout.
6. A declaration naming a target in both spans is rejected.
7. The deferred and forward-only pass orders contain the expected passes in
   the expected sequence, and both end in tone mapping.

Final automated verification:

1. Configure and build the complete `vs2026-x64-debug` preset.
2. Run all CTest targets with failure output.
3. Run `miniengine_app.exe --frames 60` with validation layers enabled and
   confirm zero validation messages.
4. Run `scripts/check-format.ps1`.
5. Run `git diff --check` and inspect the final changed-file scope.

Builds, CTest and the smoke run do not establish visual correctness.

## Manual GUI Acceptance

The user validates at least these cases:

1. Phase one: the viewport image is indistinguishable from the pre-change
   image on the default scene, including background color and panel edges.
2. Phase two: toggling the forward comparison switch produces no visible
   change on a scene containing Opaque, Mask and Blend materials.
3. Blend materials still composite back to front as the camera moves, and
   still test against opaque depth without writing it.
4. Mask cutouts retain correct silhouettes and depth occlusion.
5. All five light types produce the same result as before on the same scene.
6. Each G-buffer debug view displays the expected channel, and the normal
   view decodes to a recognizable normal field rather than noise.
7. Resizing the viewport panel, including to a degenerate size and back,
   leaves no artifacts and emits no validation errors.
8. Phase three: clicking a mesh selects it at silhouette edges where the
   previous center-projection approximation missed, and light gizmos remain
   selectable.
9. Loading a different scene, deleting entities and undoing leaves selection
   and picking consistent.

The feature is not visually accepted until the user confirms these checks.

## Out of Scope

- Clustered or tiled light culling. The eight-light limit in
  `kMaxSceneLights` stands.
- Screen space ambient occlusion, reflections, and every other effect the
  G-buffer enables.
- Image based lighting, a skybox, and specular ambient.
- Motion vectors, camera jitter, history buffers and temporal antialiasing.
  Motion vectors additionally require caching per-entity previous transforms
  and handling entity creation and destruction, which is its own state
  management problem and does not belong in this one.
- Multisampling.
- Tone mapping operators other than the existing Reinhard, and exposure
  control.
- Selection outlining, though GB4 is chosen partly to enable it.
- Bindless materials, VMA, acceleration structures and ray queries. These
  remain sub-projects P0 through P3 and P5 of the ray tracing design.
- Frustum culling, draw batching and instancing.
- A deferred or pass abstraction in `rhi/backend.h`. This work lands only in
  the Vulkan backend.
- A general render graph with automatic dependency inference or resource
  aliasing.
