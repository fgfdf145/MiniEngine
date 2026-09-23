# Clustered Lighting

## Goal

Lift the eight-light limit. Every pixel today loops over all of `ubo.lights[8]`, and a scene with
more non-ambient lights drops the rest (`SelectSceneLights`, `ReportDroppedLights`). This design
moves the lights into a storage buffer that holds up to 1024 of them and bins the local lights
(point, spot, area) into a view-space froxel grid, so each pixel evaluates only the lights whose
range reaches its cluster.

This is infrastructure: a scene with eight or fewer lights must render bit for bit as it does now.
It is also the base the later local-light shadows build on (a light in a storage buffer can carry a
shadow map index; a slot in the camera block cannot).

## Current State

- `CollectSceneLights` gathers every light into `GpuLightData` (5 x vec4, 80 bytes) on the CPU each
  frame. `SelectSceneLights` keeps at most `kMaxSceneLights = 8`: directional lights first,
  brightest first, then local lights ranked by intensity / distance^2. Ambient lights are folded
  into `ambientLuminance` and never take a slot.
- The selected lights live inside the std140 camera block, `CameraUniformData::lights[8]`, with the
  count in `sceneLightCount.x`. The `static_assert`s on the block's offsets all include
  `kMaxSceneLights * 80`.
- `ShadeSurface` (`pbr_common.glsl`) loops over every light. It is shared by `deferred_lighting.frag`
  and the forward `triangle.frag`, so both orders shade through one function. The shadow caster is
  `shadowParams.x`, an index into the light array; `SelectShadowCasterLight` makes it 0 or -1.
- Set 0 (`VulkanFrameDescriptorSetLayout`) has bindings 0-9. `VulkanUniformBuffer` owns one camera
  buffer and one previous-model storage buffer per swapchain image, host visible and written in
  `Update`.
- A light's contribution is exactly zero beyond its range: `RangeWindow` clamps to 0 at
  `distance >= range` for point and spot lights, and the area light applies it around its centre.

## Decisions

1. **Clusters are built on the CPU.** The renderer already gathers every light on the CPU each frame,
   and a CPU binning (as in Doom 2016) is pure code the unit tests can drive: no compute pass, no
   barriers, no atomics. A GPU culling pass is a later optimisation if a profile asks for it.
2. **A fixed 16 x 9 x 24 grid.** 16 x 9 screen tiles in NDC, whatever the viewport size, and 24 depth
   slices spaced exponentially between the camera's near and far planes:
   `slice = floor(log(viewDepth) * sliceScale + sliceBias)`, with
   `sliceScale = 24 / log(far / near)` and `sliceBias = -24 log(near) / log(far / near)`, clamped to
   [0, 23]. 3456 clusters.
3. **Bounding spheres are the light's range.** Point, spot and area lights are all binned as the
   sphere of radius `range` around their position (the area light's centre). That is exact for the
   range window and conservative for the spot cone and the area light's one-sided emission.
4. **Binning is conservative by construction.** A light goes into a cluster whenever its sphere
   might overlap it, never the other way round, so a skipped light is always one that contributes
   exactly zero. That is what makes the clustered and brute-force loops bit-identical.
5. **Lights move to a storage buffer.** Set 0 binding 10, `std430`, `GpuLightData` unchanged, up to
   `kMaxSceneLights = 1024`. Directional lights come first (the selection already orders them so), so
   the shadow caster stays index 0. `SelectSceneLights` keeps its ranking and simply works with the
   larger cap.
6. **The cluster grid is one storage buffer.** Set 0 binding 11: `uvec2 ranges[3456]`
   (offset, count into the index list), then `uint indices[]`, capacity `kLightClusterIndexCapacity =
   131072` (512 KiB). Within a cluster, indices ascend, so a cluster's lights are visited in the same
   order as the brute-force loop visits them.
7. **Overflow drops, and says so.** When the index list is full, the remaining (light, cluster)
   pairs are dropped and counted; the renderer logs the count when it changes, the way
   `ReportDroppedLights` does. At 1024 lights of modest range the capacity is far from reached.
8. **The camera block shrinks.** `lights[8]` and `sceneLightCount` are replaced by
   `uvec4 lightCounts` (x = directional count, y = total count, z = 1 for clustered, 0 for
   brute force) and `vec4 lightClusterSlices` (x = sliceScale, y = sliceBias). The offsets after them
   move and the `static_assert`s are rewritten.
9. **The lookup needs only the world position.** `ShadeSurface` finds the cluster from
   `ubo.proj * ubo.view * worldPosition`, so its signature and both callers stay as they are, and
   the lookup matches the CPU's binning because both use the same projection matrix (the render
   projection, Y flip included).
10. **A brute-force switch stays.** Graphics Debug gets "Clustered lighting" (on by default). Off, the
    shader loops over every light, as today. It is both the A/B switch for timing and the check that
    clustering changes no pixel.
11. **A heat map debug view.** "Light clusters" joins the viewport output list. The lighting pass
    writes a heat colour of the pixel's cluster light count instead of shading, divided by the
    exposure, and the tone mapping pass shows it multiplied back, without the operator. Blend
    surfaces are still shaded by the forward pass on top of it. The forward-only order has no
    lighting pass, so the view is off there like the G-buffer views.

## Data Flow

```
CollectSceneLights ──► SelectSceneLights (cap 1024) ──► selected GpuLightData
                                                         │
          camera view + render projection + near/far ────┤
                                                         ▼
                                          BuildLightClusters (CPU, pure)
                                                         │ ranges + indices + slice params
                                                         ▼
                           VulkanUniformBuffer::Update: binding 0 (camera block, counts, slices)
                                                         binding 10 (lights)
                                                         binding 11 (cluster grid)
                                                         ▼
                   ShadeSurface: directional lights, then the cluster's local lights
```

### `LightClusters` (pure, `engine/renderer/light_clusters.h`)

```cpp
inline constexpr uint32_t kLightClusterTilesX = 16;
inline constexpr uint32_t kLightClusterTilesY = 9;
inline constexpr uint32_t kLightClusterSlices = 24;
inline constexpr uint32_t kLightClusterCount = 16 * 9 * 24;
inline constexpr uint32_t kLightClusterIndexCapacity = 131072;

struct LightClusterCamera
{
    glm::mat4 view;
    glm::mat4 projection; // the render projection the shaders use
    float nearPlane;
    float farPlane;
};

struct LightClusterSphere
{
    glm::vec3 worldCenter;
    float radius;
    uint32_t lightIndex; // index into the uploaded light array
};

struct LightClusterGrid
{
    std::vector<glm::uvec2> ranges;   // kLightClusterCount entries: offset, count
    std::vector<uint32_t> indices;    // at most indexCapacity entries
    float sliceScale = 0.0f;
    float sliceBias = 0.0f;
    uint32_t droppedCount = 0;        // (light, cluster) pairs left out for lack of capacity
};

LightClusterGrid BuildLightClusters(
    const LightClusterCamera& camera,
    std::span<const LightClusterSphere> spheres,
    uint32_t indexCapacity);

// The cluster a view-space position falls in: the shader's lookup, for the tests.
uint32_t FindLightCluster(const LightClusterGrid& grid, const glm::mat4& projection, const glm::vec3& viewPosition);
```

Binning, per sphere:

1. Transform the centre to view space. Depth range `[d - r, d + r]` with `d = -viewCenter.z`; skip
   the sphere when it lies wholly in front of the near plane or behind the far plane. The slice range
   is the slice formula at both ends, clamped.
2. Tile columns: the boundary between NDC x `a` is the plane through the eye
   `P00 x + (P20 + a) z = 0` (glm's column-major `P[0][0]`, `P[2][0]`), normalised. Column `i` is
   touched when the sphere reaches past its left boundary and short of its right one (signed
   distances against the radius). Rows likewise with `P11`, `P21`; the sign of `P11` carries the Y
   flip.
3. Every cluster in the box `columns x rows x slices` gets the light.

The grid is built as per-cluster lists and then flattened in cluster order into `ranges` and
`indices`, so every cluster's indices ascend.

### Shaders

`scene_common.glsl` declares binding 10 (`readonly buffer SceneLights { SceneLightData lights[]; }`),
binding 11 (`readonly buffer LightClusterGrid { uvec2 ranges[LIGHT_CLUSTER_COUNT]; uint indices[]; }`)
and the grid constants, and replaces `lights[]` / `sceneLightCount` in the camera block with
`lightCounts` and `lightClusterSlices`.

`pbr_common.glsl` gains `FindLightCluster(worldPosition)`, the same arithmetic as the C++ one.
`ShadeSurface` loops over the directional lights `[0, lightCounts.x)`, then either the cluster's
index range (clustered) or `[lightCounts.x, lightCounts.y)` (brute force). The shadow test stays
`i == shadowLightIndex`, which can only match a directional light.

`deferred_lighting.frag` gets a second push-constant vec4, `debug` (x = 1 for the heat map,
y = 1 / exposure). `tonemap.frag` gets `GBUFFER_VIEW_LIGHT_CLUSTERS`, which shows the HDR target
times the exposure, clamped, without the operator.

### Settings

`RenderDebugSettings` gains `bool clusteredLighting = true`. `GBufferDebugView` gains
`LightClusters = 8`. Neither is persisted, like the rest of `RenderDebugSettings`.

## Error Handling

- More than 1024 non-ambient lights: the selection drops the lowest-ranked ones and the existing
  `ReportDroppedLights` logs it, as it does at eight today.
- Index list full: `droppedCount` pairs are skipped; the renderer logs the count when it changes.
  A dropped pair can darken part of a light's range, so this is a warning, not a silent clamp.
- `near >= far` or non-positive `near` cannot happen with the editor's camera, but
  `BuildLightClusters` clamps `near` to at least 1e-4 and `far` to at least `near * 1.001`, so the
  logarithms stay finite.

## Automated Verification

`tests/light_clusters_tests.cpp`:

- **Conservative binning.** Random cameras (the real `Camera::GetProjectionMatrix` with Y flip and
  zero-to-one depth, random view matrices) and random spheres; for random points in the view
  frustum within a sphere's radius, `FindLightCluster` returns a cluster whose list contains that
  sphere's light.
- A sphere wholly behind the camera or beyond the far plane lands in no cluster.
- Indices ascend within every cluster; ranges tile the index list without gaps.
- The capacity is honoured, and `droppedCount` counts exactly what was left out.
- The slice formula maps `near` to slice 0 and just short of `far` to slice 23, monotonically.

The `CameraUniformData` `static_assert`s pin the new block layout.

## Manual Acceptance

1. **Nothing changes at eight lights or fewer.** `--capture` of Sponza with the default lights on
   `main` and on this branch produce identical PNGs.
2. **Clustered equals brute force.** A Sponza scene with 256 point lights (generated, not committed):
   captures with "Clustered lighting" on and off are identical.
3. **It is faster.** On the same scene, the frame time with clustering on is well below the
   brute-force one. Reported with numbers, not asserted.
4. **The heat map is plausible.** Clusters around lights read hot, empty space cold.

## Out of Scope

- Shadows for local lights (the next step; the light buffer is where their map index will go).
- GPU culling, tighter spot-cone or area-light bounds, per-cluster AABB refinement.
- Z-binning or tile-only (2.5D) variants.
- Surfacing cluster statistics in the editor UI beyond the log.
