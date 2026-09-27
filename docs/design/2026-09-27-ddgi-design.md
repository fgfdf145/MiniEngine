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
  finest levels, then round robin with level 0 twice as often as level 1, and so on. An invalid probe
  is not sampled until it is updated.
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
  its position and uses hysteresis 0.5 once, so a scroll does not fade in from black. When the sun's
  direction or colour, or the sky, changes between frames, hysteresis drops to 0.85 for a second.
- **Relocation and classification** (in the update): a probe moves inside its cell (at most 0.45
  spacing) away from the nearest front face it is too close to; with more than 25% back-face hits it
  is inside geometry and moves toward the farthest front face it sees, not through the nearest back
  face (Majercik et al. 2021 cross it): a probe on a wall's plane is as near the wall's far side, and
  crossing put probes outside the Cornell box, where the sunlit roof lit the ceiling below (step 6
  measured the box 2.1x too bright, 1.2x after). A probe seeing back faces within a spacing is
  inactive, but stays scheduled so a moved object can revive it.

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
5. Specular sky visibility; moving instances; foliage coverage; adaptive hysteresis.
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
