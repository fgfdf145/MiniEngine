# Cascaded DDGI

Asked for on 2026-09-27 ("全局光照设计"). The user's choices: DDGI directly (no baked lightmaps);
outdoor race tracks first, so camera-following cascades; the sun changes in real time and
vegetation and vehicles move, so the probes never stop updating.

## Why

What lights a surface indirectly today:

| Term | Now | Missing |
| --- | --- | --- |
| Sky, diffuse | one SH9 for the whole scene | a surface under a roof or in a tunnel sees the whole sky; VBAO darkens only 1.5 m around it |
| Bounce | `GiTrace`, one screen-space bounce within 3 m | light from anything off screen or farther; more than one bounce |
| Sky, specular | one 256^2 cube, plus SSR on screen | indoors, where SSR misses, the sky's reflection |

Dynamic Diffuse Global Illumination (Majercik et al. 2019) stores irradiance and visibility in a grid
of probes updated by rays every frame. It needs rays against the scene, which MoltenVK cannot trace
in hardware, so a compute shader walks a two-level BVH. Cascades follow Majercik et al. 2021
(*Scaling Probe-Based Real-Time Dynamic Global Illumination for Production*): several grids of the
same probe count, spacing doubled per level, centred on the camera, scrolled toroidally.

## Ray Tracing Scene

- **Bottom level** (`engine/renderer/ray_tracing_bvh.h`): one BVH per `MeshData`, built on the CPU on
  a worker thread when content uploads, cached by the mesh's address (the model cache shares one
  `MeshData` among its submeshes' users). Binned SAH (12 bins), at most 4 triangles per leaf,
  32-byte nodes (bounds, then first child or first triangle, then triangle count, 0 for an inner
  node, whose children are adjacent). Triangles are stored as v0, e1, e2 (three vec4, 48 bytes).
- **Top level**: one instance per render submesh: its world-to-object 3x4 matrix, its BVH's node and
  triangle offsets, its ray material. Rebuilt on the CPU every frame (instances number in the
  hundreds to thousands) and uploaded per frame slot; that is what lets vehicles move.
- **Ray materials**: the low-frequency material a probe ray sees, one per submesh: average albedo,
  average emission, coverage, flags. Base colour and emissive factors times their textures' averages,
  measured on the GPU once per upload (`ddgi_material_average.comp`, a 16 x 16 grid of samples at the
  mip nearest 16 texels; coverage is the share of samples at or above the Mask cutoff, 1 for Opaque).
  Flags: double-sided (never counts as a back face), transmissive or Blend (rays pass through
  weighted by 1 - alpha; transmission counts as clear).
- **Foliage**: a Mask hit is accepted when a hash of the ray and the triangle falls under the
  coverage. Tree cards then shadow and bounce light in proportion to their leaves without sampling
  textures in the trace.
- **Moving instances**: an instance whose matrix changed in the last 30 frames is left out of the
  probe rays (it still receives GI). Probes blend over many frames, so a passing car would leave a
  dark trail on the road behind it. Once it stands still it is traced again.
- **Traversal** (`ray_tracing_common.glsl`): one loop over both levels with a 32-entry stack, nearer
  child first; `TraceClosest` and `TraceAny` (shadow rays).
- **Check**: debug view 14 traces one primary ray per pixel and shows the hit's ray albedo lit by the
  sun through a traced shadow ray. It should match the Albedo view's shapes, and the rasterised
  shadows.

## Probes

- **Cascades**: 4 levels of 32 x 16 x 32 probes (x, y, z), spacing 1, 2, 4, 8 m: 32 to 256 m wide, 16
  to 128 m tall. Each level's grid origin is the camera's cell at that level's spacing minus one less
  than half the grid, so it reaches 15 cells across and 7 up and down around the camera wherever the
  camera is in its cell. (24 x 12 x 24, 2026-09-28, let an arcade's back wall fall to the 2 and 4 m
  levels from 11 m away; their probes stand outside it and lit it 1.5 to 3 times too bright. The
  visibility atlas became RG16F to pay for part of the 2.4 times as many probes: 94 to 137 MB.) (8 tall, first, reached 2 cells up: looking up at New Sponza's walls fell
  to the 8 m level and the SH sky; against the reference the Cornell box went 1.16 -> 1.02 with 12.)
  Settings can change the base spacing and the level count.
- **Toroidal storage**: a probe at world grid coordinate g is stored at g mod the grid size, so a
  scroll moves nothing. A per-probe state buffer holds the coordinate each slot last held, the
  relocation offset and a state (invalid, active, inactive). Each frame `ddgi_scroll.comp` marks
  slots whose coordinate changed as invalid.
- **Textures**, one 2D array layer per level, octahedral with a 1-texel border:
  irradiance 8 x 8 RGBA16F (rgb irradiance / pi, the ambient term's unit; a the cosine-weighted sky
  visibility, the share of rays that escape), visibility 16 x 16 RG16F (mean distance, mean squared
  distance). Irradiance per level is 2880 x 240 texels, visibility 5184 x 432.
- **Schedule**: the CPU picks at most 2048 probes a frame (settings), first every invalid probe of the
  finest levels, then hot probes (below), then round robin with level 0 twice as often as level 1,
  and so on. An invalid probe is not sampled until it is updated. Empty probes take one round robin
  turn in 16 (see Convergence and Updates).
- **Trace** (`ddgi_trace.comp`): 64 rays per scheduled probe, directions from a spherical Fibonacci
  set turned by a random rotation per frame. A miss returns the sky along the ray (the prefiltered
  cube at a middle mip under a physical sky, plus the Ambient and Hemisphere lights; the uniform
  ambient under None). A front-face hit returns emission plus albedo times the direct light
  (directional lights with a traced shadow ray; local lights through the shadow atlas where they
  have a tile, unshadowed otherwise, within their range) plus albedo times the probes' own
  irradiance at the hit: the infinite bounce. A back-face hit on a single-sided surface returns black
  and a negative distance.
- **Update** (`ddgi_update.comp`, one workgroup per probe and texture): each texel blends its
  cosine-weighted (irradiance) or power-weighted (visibility) average of the rays with hysteresis
  0.97, and writes its border copies. An invalid probe starts from the next coarser level sampled at
  its position and uses hysteresis 0.5 once, so a scroll does not fade in from black. A young probe
  averages its updates (the n-th keeps n / (n + 1)) until that exceeds the hysteresis. A probe whose
  light changed restarts that average (see Convergence and Updates).
- **Relocation and classification** (in the update): a probe moves inside its cell (at most 0.45
  spacing) away from the nearest front face it is too close to; with more than 25% back-face hits it
  is inside geometry and moves toward the farthest front face it sees, not through the nearest back
  face (Majercik et al. 2021 cross it): a probe on a wall's plane is as near the wall's far side, and
  crossing put probes outside the Cornell box, where the sunlit roof lit the ceiling below (step 6
  measured the box 2.1x too bright, 1.2x after). A probe seeing back faces within a spacing is
  inactive, but stays scheduled so a moved object can revive it.

## Convergence and Updates

Added 2026-10-06. Probes converged and followed changes slowly: a moved sun still showed 12% error
two seconds later and 5.5% after eight (`ddgi_track`, sun turned 30 degrees, view 15 against a run
converged under the new sun, at 60 fps).

- **Why**: 4 levels of 16 384 probes and 2048 updates a frame put 15 frames between a level 0
  probe's updates and 120 between a level 3 probe's, and once a level settles four times as many.
  The hysteresis of 0.97 is per update, so the 76 updates a probe needs to take 90% of a change
  were 19 s at level 0 and minutes at level 3. The fast hysteresis (0.85 for a second after the
  lighting changed) gave a level 0 probe four updates. And every ray scene install (each streamed
  cell, each model that finished loading) cleared every probe to black.
- **Change detection** (`ddgi_update.comp`): each probe keeps the running mean and mean square of
  its updates' average luminance (`DdgiProbeState::stats`). An update whose average lies beyond four
  standard deviations of that spread plus a tenth of the mean, after at least four updates, is a
  change: it keeps half of what the probe held and the young average starts over, so the next
  updates keep 2/3, 3/4 and so on. A change is rare by chance: in steady state about 30 a million
  updates.
- **Lighting epoch** (`DdgiLightingWatch`, replacing the fast hysteresis): a change in the sun, the sky
  or their count increments an 8-bit epoch; a probe that updates under another epoch than the one it
  recorded restarts the same way, whenever its turn comes. The lighting is compared with what it was
  when the epoch started, to 0.5% (about a third of a degree of the sun), so a sun the time of day
  moves a little each frame starts a new epoch every so often instead of never.
- **Feedback and hot probes**: the update writes per scheduled probe whether it changed and whether it
  is empty, with its coordinate mod 256, into the frame slot's host-visible buffer. The CPU reads it
  when the slot's fence has signalled (`VulkanDdgi::TakeFeedback`, `DdgiProbeScheduler::ApplyFeedback`).
  A changed probe is updated in each of the next 4 frames, its six neighbours in the next 2; when a
  neighbour sees the change too, it passes it on. Hot probes take at most half the budget. A change
  does not unsettle the level: hot probes cover it.
- **Empty probes**: a probe that saw no surface within 2.5 spacings for three updates in a row lights
  no surface (a surface is lit by the corners of its cell, at most sqrt(3) spacings plus the
  relocation away) and is cold: it takes one round robin turn in 16. Outdoors most probes are empty
  (84% on `ddgi_track`, open air above the road). Probes inside geometry are not cold: their back-face
  evidence needs some 30 updates to fade when what buried them moves, and throttled, it darkened
  surfaces next to them in GTA SA. The settle estimate counts a cold probe as a sixteenth.
- **New geometry**: a ray scene install no longer clears the probes. It increments a 4-bit geometry
  epoch; a probe under another epoch judges inside and empty afresh, and the change detection
  restarts whichever probes the new content changes. A streamed cell away from the camera changes
  none of them.

Measured on `ddgi_track` (960 x 540, view 15, fixed exposure; mean relative error by luminance against
a run converged in the same state; two converged runs differ by about 1.6 to 2%). The app's
`--capture-at F1,F2,...` and `--turn-sun FRAME,PITCH,YAW,ROLL` make these runs.

| Frames after the sun turned | 30 | 60 | 120 | 240 | 480 | 1920 |
| --- | --- | --- | --- | --- | --- | --- |
| Before | 21.3% | 17.0% | 12.4% | 8.3% | 5.5% | 3.7% |
| After | 8.2% | 6.9% | 4.2% | 3.4% | 2.5% | 2.1% |

The camera driving at 15 m/s for 120 frames: 20.2% before, 12.7% after; 480 frames: 14.9% and 11.1%.
From a cold start it is about the same early (30 frames: 13.9% against 12.7%; 120: 6.1% against
5.6%) and lower later (1920: 0.95% against 1.3%). Frame to frame flicker once settled is 0.6 to 0.8%
(0.5 to 1.1% before); the GPU cost once settled is the same (0.15 ms on this scene: 512 probes a
frame). In GTA SA two converged runs of the same build differ by about 5% (which side of a wall
probes relocate to depends on the order content arrives in), so there it is checked for bias rather
than speed: the new build's converged images lie within that spread. Throttling probes inside geometry
as well did not: their surfaces stayed dark.

## Shading

- **Sampling** (`ddgi_common.glsl`): the eight probes around the point, trilinear weights times a
  back-face term times the Chebyshev visibility test, with the surface point biased along the normal
  and the view vector (0.2 spacing). The finest level containing the point is used; in its outermost
  cell it blends to the next, by the point's distance to the camera rather than to the grid's faces:
  those move a whole cell when the grid scrolls, and with them the blend, which showed as the whole
  image switching as the camera crossed a cell (13.7/255 on average across one, now as any 0.1 m). (Two cells, first, handed much of a 16 m room's ceiling to the 4 m and
  8 m levels, whose probes stand far below it or above the roof: 2.0x against the reference, 1.24x
  with one.) Outside every level, or with DDGI off, the SH sky as now.
- **Diffuse**: `EvaluateSkyAmbient` and `EvaluateUniformAmbient` take the DDGI irradiance in place of
  the SH sky and ambient lights, for N and, for diffuse transmission, -N. It holds the sky, the
  Ambient and Hemisphere lights and every bounce, so they are not added again. VBAO still multiplies,
  and `GiComposite` still adds the screen-space bounce: the AO removes the occluded share of the
  probe's average and the screen trace puts back the real radiance of those occluders.
- **Specular, coat, sheen** (`SceneSpecularEnvironment`): the environment along R becomes
  `sky(R) * V(R) + max(E(R) / pi - V(R) * S(R), 0)`, where V is the probes' sky visibility around R, E
  their irradiance around R and S the sky's own irradiance / pi around R (the SH sky and the ambient
  lights): E holds the sky's escaped share too, so it is taken out and only the surroundings' light
  remains. Indoors the sky's reflection gives way to the walls'. The probes are chosen for the surface
  (bias and back-face term along N) and read along R; choosing them along R let probes outside a
  tunnel's roof light the reflection of its upper walls. SSR still wins where it is trusted.
- **Coverage**: a probe inside geometry (inactive) still counts toward its level's coverage; only
  stale probes lower it. Counting inactive probes out handed their share to the coarser levels and
  the unoccluded SH sky, which lit walls with probes buried in them as if they stood in the open.
- **Where**: every surface `ShadeSurface` shades, deferred and forward.

## Passes and Resources

`VulkanDdgi` is device-lifetime like `VulkanAtmosphere` and `VulkanEnvironmentProbe`, keeps its images
in GENERAL and orders itself with barriers. It records after the environment probe (the trace samples
the prefiltered cube) and before the scene passes: scroll, trace, update. Set 0 gains binding 21
(irradiance array), 22 (visibility array) and 23 (probe states); the camera block gains the cascade
origins and parameters. The frame set's lights, shadow atlas and tiles become visible to compute.

Budget, to be measured on the user's MacBook Air: 2048 probes x 64 rays = 131k rays plus as many shadow
rays per frame.

## Settings and Views

Graphics Debug, "DDGI": enabled (on; off in the Khronos reference view), levels, base spacing, probes
per frame, rays per probe, hysteresis, strength. Debug views: 14 ray-traced scene, 15 DDGI irradiance
at each pixel, and a probe overlay drawing each probe as a small sphere of its irradiance.

## Verification

- Unit tests: BVH closest and any hit against brute force on random rays; toroidal addressing;
  the schedule; octahedral mapping round trips.
- Scenes (in `tests/fixtures/render_scenes`): `bounce_box.yaml` (the magnitude against the
  one-bounce result); a Cornell box (colour bleeding); `ddgi_track.yaml`, a generated outdoor track:
  a 400 m road and grass, a tunnel, a grandstand with a roof, tree cards and a car that moves.
- The reference (`engine/renderer/reference_path_tracer.h`, `--reference` in the app rather than a
  separate `tools/path_trace`, so it traces exactly the ray scene and ray materials the probes do): a
  CPU path tracer, checked against a furnace and a view factor, that computes each compared point's
  indirect irradiance / pi, and each nearby probe's along six axes, next to the probes' own. With the
  relocation and fade changes above (2026-09-27, 2048 paths a point), probes / reference by
  luminance and the median relative error: `bounce_box` 1.10, 7%; `cornell_box` 1.18, 21%; the box
  four times larger, camera low 1.11, 12%, camera high 1.25, 28%. What remains is the probes'
  resolution: a surface facing a bright patch is lit from probes nearer the patch than it is, and a
  0.3 m ball between 1 m probes is not resolved at all.

## Steps

1. BVH on the CPU, GPU ray scene, traversal, debug view 14.
2. Probe textures, one level, no scrolling: trace, update, diffuse sampling; view 15 and the overlay.
3. Relocation, classification, visibility weighting.
4. Cascades and scrolling, the schedule; the track scene with a moving camera.
5. Specular sky visibility; moving instances; foliage coverage; adaptive hysteresis (replaced by the
   lighting epoch, 2026-10-06).
6. The path tracer and the comparisons.

## Not Done: Occlusion Finer Than the Probes

Recorded 2026-09-28, to do together with hardware ray tracing (and the DLSS work that brings it).

- **Problem**: a space smaller than a level's spacing is lit as if open. Its probes stand outside it
  and see into it through the opening, so an arcade's alcove or a doorway is 1.5 to 3 times too
  bright once the camera is far enough away for a 2, 4 or 8 m level to take it. A 32 x 16 x 32 grid
  (the finest level to 14.5 m across) and a 3-cell fade (5e17e1b) moved where this starts, from 11 m
  to about 20 m in New Sponza. They did not remove it. VBAO's 1.5 m radius does not see occluders at
  that scale, and screen-space GI only sees what is on screen.
- **Idea**: per pixel, where the answer comes from a level coarser than the finest, trace a few short
  rays (length about that level's spacing) through the ray scene around the normal. Scale the probes'
  irradiance by the share that escapes, as an ambient occlusion matched to the probe spacing. The
  finest level needs none. Half resolution, with a temporal and bilateral filter as the AO has, would
  keep it to a ray or two a pixel.
- **Cost**: with the software BVH in `ray_tracing_common.glsl`, about a million rays a frame at half
  of 2167 x 1767 is estimated at over 10 ms (the probes' 260 000 rays take about 3 ms), too much.
  With `VK_KHR_ray_query` on an RTX GPU it should be 1 to 2 ms. MoltenVK has no ray queries, so the
  Mac keeps the software path and would go without it.
- **Check**: the dolly toward the alcove at the far end of the ground floor (camera `x,1.7,0,180,0`,
  x from 12 to -4, `--debug-view 15`, centre pixel). Converged, the wall reads 13 to 23 out to 20 m and
  49 at 24 m. With the fix, 24 m and beyond should match the near values.

### What It Needs

**Hardware ray tracing groundwork.** Most of the work. DDGI's probe traces, reflections and shadows can
reuse it. Today the device enables only the swapchain extension.

1. Device: `VK_KHR_acceleration_structure`, `VK_KHR_ray_query` and `VK_KHR_deferred_host_operations`,
   plus `bufferDeviceAddress` (Vulkan 1.2). That moves `VulkanDevice`'s feature setup from
   `VkPhysicalDeviceFeatures` to a `VkPhysicalDeviceFeatures2` chain. Without them, the path stays
   off.
2. Buffers: vertex and index buffers get device addresses and acceleration-structure build-input
   usage. The position-only stream the shadow passes read (`VulkanBuffer::GetPositionHandle`) is the
   build input.
3. Bottom levels: one per mesh, built on the GPU when content uploads, then compacted. Uncompacted,
   New Sponza's 10.8 million triangles may take several hundred MB; compaction should roughly halve
   that, to be measured. This also retires the CPU hierarchy build (`ray_tracing_bvh.cpp`,
   2.7 s in Release).
4. Top level: rebuilt or refit each frame from the instances' matrices. It keeps the ray scene's rules:
   Blend surfaces and moving instances are skipped (`kRayInstanceSkip`), through instance masks.
5. Alpha: masked and covered surfaces (foliage, ivy) are non-opaque. The ray query's candidate loop
   reads the ray material's coverage, as `ray_tracing_common.glsl` does.

**The occlusion pass.**

6. A compute pass after the geometry pass, at half resolution. Per pixel it finds the level the DDGI
   lookup answers from, with the same fade as `DdgiIrradianceAlong`. Only where that is not the
   finest level, it traces one or two cosine-distributed rays about the normal, up to that level's
   spacing, and writes the share that escapes.
7. Filtering: the half-resolution joint bilateral upsample and temporal accumulation the AO and
   screen-space GI resolves already use (`vbao_common.glsl`'s `HalfResSourcePixel`).
8. Shading: it scales only the probes' diffuse irradiance (`SceneDiffuseAmbient`). The rays start at
   VBAO's radius (1.5 m), so the two do not occlude the same thing twice. Whether the specular
   environment (`SceneSpecularEnvironment`) takes it too is decided by looking.

**Switches, fallback and checks.**

9. A Graphics Debug switch, saved in capture state files. Off, with no pass recorded, on a device
   without ray queries. As far as known MoltenVK has none, so the Mac goes without; confirm against
   the MoltenVK version in use first.
10. Checks:
    - the alcove dolly above;
    - `--reference` on `cornell_x4` (1.15, 26% median error before);
    - validation;
    - the pass's GPU time, 1 to 2 ms the target.

**Also gained.** DDGI's probe traces (`ddgi_trace.comp`, 3.3 ms in the Sponza capture on the software
BVH) move to ray queries, likely under 1 ms. Rays can also fill in reflections that SSR misses.

**Size.** The groundwork is about as large as DDGI's first step (BVH, ray scene, traversal). The pass
is about as large as the half-resolution AO and GI change. Then switches, checks and tuning.

**With DLSS.** DLSS is its own piece: NVIDIA Streamline or the NGX SDK, a new dependency. The motion
vectors and TAA's jitter it needs already exist. It and this pass work only on NVIDIA GPUs, so plan
them together.
