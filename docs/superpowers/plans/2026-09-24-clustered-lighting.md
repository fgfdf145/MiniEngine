# Clustered Lighting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the eight-slot light array in the camera block with a 1024-light storage buffer and a CPU-built 16 x 9 x 24 froxel grid, so each pixel shades only the lights that reach it.

**Architecture:** A pure `BuildLightClusters` unit bins each local light's range sphere into clusters on the CPU every frame. `VulkanUniformBuffer` uploads the lights (set 0 binding 10) and the flattened grid (binding 11) per swapchain image. `ShadeSurface` finds the pixel's cluster from its world position and loops over the directional lights, then the cluster's lights. A debug switch keeps the brute-force loop for A/B checks.

**Tech Stack:** C++20, Vulkan 1.x (MoltenVK on macOS), GLSL 450 compiled by glslc, glm, CTest executables.

**Spec:** `docs/superpowers/specs/2026-09-24-clustered-lighting-design.md`

## Global Constraints

- Grid: `kLightClusterTilesX = 16`, `kLightClusterTilesY = 9`, `kLightClusterSlices = 24`, `kLightClusterCount = 3456`.
- `kMaxSceneLights = 1024`; `kLightClusterIndexCapacity = 131072`.
- Set 0 binding 10 = lights (`std430`, `GpuLightData` 80 bytes unchanged); binding 11 = cluster grid (`uvec2 ranges[3456]` then `uint indices[]`).
- Camera block: `lights[8]` + `sceneLightCount` become `uvec4 lightCounts` (x directional, y total, z clustered flag) + `vec4 lightClusterSlices` (x scale, y bias).
- Scenes with <= 8 lights render bit-identically to `main`; clustered and brute-force are bit-identical.
- Allman braces, 4-space indent, comments explain why (match surrounding code). Tests are plain executables with `Require`.
- Build: `cmake --build out/build/macos-debug`; tests: `ctest --test-dir out/build/macos-debug`. cmake lives in `~/.local/share/miniengine-tools/lib/python3.9/site-packages/cmake/data/bin`.

## Review Focus

- Camera inside a light's range (sphere contains the eye): every tile of the touched slices must get the light — covered by the random property test (spheres placed near the eye).
- Light straddling the near plane or far plane: clamp slice range, never skip — property test samples points at near and far depths.
- Viewport aspect ratios far from 16:9 (tall editor panes): tiles are NDC-based so this only stretches them — property test randomises aspect.
- Many overlapping big lights exhausting the index capacity: must drop and count, never write past the buffer — capacity test.
- Degenerate near/far (near 0, far <= near): must stay finite — degenerate camera test.

---

### Task 1: `LightClusters` pure unit

**Files:**
- Create: `engine/renderer/light_clusters.h`, `engine/renderer/light_clusters.cpp`
- Modify: `engine/renderer/CMakeLists.txt` (add both to `engine_render_core`)
- Test: `tests/light_clusters_tests.cpp`, `tests/CMakeLists.txt` (new `miniengine.light_clusters` test, linked to `engine_render_core`, same block shape as `miniengine_environment_brdf_tests`)

**Interfaces:**
- Produces: the constants above; `LightClusterCamera{view, projection, nearPlane, farPlane}`; `LightClusterSphere{worldCenter, radius, lightIndex}`; `LightClusterGrid{ranges, indices, sliceScale, sliceBias, droppedCount}`; `LightClusterGrid BuildLightClusters(const LightClusterCamera&, std::span<const LightClusterSphere>, uint32_t indexCapacity)`; `uint32_t FindLightCluster(const LightClusterGrid&, const glm::mat4& projection, const glm::vec3& viewPosition)`.

- [ ] **Step 1: Write the failing tests** — `tests/light_clusters_tests.cpp` with:
  - `ConservativeBinning`: 200 random cameras (`Camera` with random fov 30-90, near 0.05-1, far 20-500, `GetProjectionMatrix({w,h}, true, true)` with random aspect 0.3-3, random `lookAt` view), 40 random spheres each (some centred within 2 m of the eye, radius 0.2-15), 50 random points per sphere inside `0.98 * radius`, kept only when in the frustum (`near <= depth <= far`, |ndc| <= 1); `FindLightCluster` of the point's view position must list the sphere's light.
  - `CullsOutsideTheFrustumDepth`: a sphere wholly behind the eye and one wholly beyond far give an empty index list.
  - `IndicesAscendAndRangesTile`: offsets are the running sum of counts; indices ascend within each range.
  - `CapacityIsHonoured`: 20 spheres of radius 1000 around the eye with capacity 1000 give `indices.size() == 1000` and `droppedCount == 20 * 3456 - 1000`.
  - `SlicesAreMonotonic`: slice of `near` is 0, of `far * 0.999` is 23, non-decreasing in between.
  - `DegenerateCameraStaysFinite`: near 0, far 0 gives finite `sliceScale`/`sliceBias`.
- [ ] **Step 2: Run** `cmake --build out/build/macos-debug --target miniengine_light_clusters_tests` — expected: fails to compile (header missing).
- [ ] **Step 3: Implement** `light_clusters.{h,cpp}`: slice params from the clamped near/far; boundary planes `normalize(vec3(P[0][0], 0, P[2][0] + a))` for `a = -1 + 2i/16`, rows `normalize(vec3(0, P[1][1], P[2][1] + b))`; per sphere, depth cull, slice range, touched columns (`dot(plane_i, c) >= -r && dot(plane_{i+1}, c) <= r`) and rows, then emit `(cluster, light)` pairs; counting sort by cluster (stable, so input order is kept within a cluster), truncating at capacity. `FindLightCluster` is the shader's arithmetic.
- [ ] **Step 4: Run** the test executable — expected: `light clusters tests passed`.
- [ ] **Step 5: Commit** `feat(renderer): CPU light cluster binning`.

### Task 2: GPU light buffer, cluster grid and shader loop

**Files:**
- Modify: `engine/renderer/vulkan/uniform_buffer.{h,cpp}` — `kMaxSceneLights = 1024`; camera block members; `static_assert`s; bindings 10/11 in `VulkanFrameDescriptorSetLayout` (fragment stage); per-image host-visible light and grid buffers; pool storage-buffer count `imageCount * 4`; `Update` takes `LightUpload{lights, directionalCount, const LightClusterGrid*, clustered}`.
- Modify: `engine/renderer/vulkan/renderer.cpp` — build spheres from the selected local lights, `BuildLightClusters` with `view`, `renderProjection`, camera near/far; log `droppedCount` changes (`ReportDroppedClusterLights`, next to `ReportDroppedLights`); pass `renderDebug.clusteredLighting`.
- Modify: `engine/renderer/render_types.h` — `bool clusteredLighting = true` in `RenderDebugSettings`.
- Modify: `shaders/vulkan/scene_common.glsl`, `shaders/vulkan/pbr_common.glsl` — buffers, constants, `FindLightCluster`, `ShadeSurface` loops.

**Interfaces:**
- Consumes: Task 1's `BuildLightClusters`, `LightClusterGrid`, constants.
- Produces: `RenderDebugSettings::clusteredLighting`; GLSL `FindLightCluster(vec3 worldPosition)` and `LightClusterLightCount(uint cluster)` (for Task 3's heat map).

- [ ] **Step 1:** Capture the baseline on `main`: `miniengine_app --scene <sponza scene> --frames 90 --capture baseline.png` (scratchpad).
- [ ] **Step 2:** Make the changes above. The shader loop:

```glsl
uint directionalCount = ubo.lightCounts.x;
for (uint i = 0u; i < directionalCount; ++i) { ... shadow when i == shadowLightIndex ... }
if (ubo.lightCounts.z != 0u)
{
    uvec2 range = lightClusters.ranges[FindLightCluster(worldPosition)];
    for (uint k = 0u; k < range.y; ++k)
        directAccum += EvaluateSceneLight(sceneLights.lights[lightClusters.indices[range.x + k]], ...);
}
else
{
    for (uint i = directionalCount; i < ubo.lightCounts.y; ++i) { ... }
}
```

- [ ] **Step 3:** Build, run all tests, capture again; `cmp baseline.png after.png` — expected identical.
- [ ] **Step 4: Commit** `feat(renderer): clustered lighting`.

### Task 3: Graphics Debug switch and heat map

**Files:**
- Modify: `engine/renderer/render_types.h` (`GBufferDebugView::LightClusters = 8`), `engine/editor/ui/editor_misc_panels.cpp` (checkbox "Clustered lighting", combo entry "Light clusters"), `engine/renderer/vulkan/lighting_pass.cpp` (second push-constant vec4 `debug`: x heat map on, y 1 / exposure), `shaders/vulkan/deferred_lighting.frag`, `shaders/vulkan/tonemap.frag` (`GBUFFER_VIEW_LIGHT_CLUSTERS`: `min(hdr * exposure, 1)`).

- [ ] **Step 1:** Heat colour: `count / 32` through a blue → green → yellow → red ramp, black for 0.
- [ ] **Step 2:** Build, test, capture with the view forced on through a temporary local change and inspect it; revert the temporary change.
- [ ] **Step 3: Commit** `feat(editor): clustered lighting switch and heat map view`.

### Task 4: Acceptance (nothing committed)

- [ ] Generate a Sponza scene with 256 point lights (scratchpad YAML).
- [ ] Capture with clustering on and off (temporary default flip); `cmp` — identical.
- [ ] Time both: frame time from the app's log or a `--frames` run, report numbers.
- [ ] Capture the heat map; inspect.
