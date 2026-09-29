# Sky and Atmosphere Design

## Goal

Give every scene a sky. A scene-level environment chooses between a physically based atmosphere
(Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique") and an
HDRI environment map, or keeps today's flat background. The atmosphere also tints the sun by its
transmittance and fogs distant geometry with aerial perspective, all in the engine's physical
units (lux, cd/m^2).

This is phase 2 of the image-based lighting roadmap in
`docs/design/2026-09-23-float-textures-design.md`. Phases 3 and 4 (diffuse and specular
IBL) will capture whichever sky is active into a cubemap; this phase draws the sky directly from
its source and captures nothing.

## Current State

- Frame: the directional shadow pass (outside the scene pass list), then Geometry, AoTrace,
  AoResolve, Lighting, Forward, ExposureHistogram, Tonemap (`scene_pass_order.cpp`). The
  forward-only comparison order is Forward, ExposureHistogram, Tonemap.
- The HDR target is `R16G16B16A16_SFLOAT` and holds raw radiance in cd/m^2. Pixels no geometry
  covers get `GetBackgroundRadiance(exposure)`, a constant fixed on screen (`scene_pass.h`): the
  lighting pass writes it where depth is 1, the forward pass clears to it.
- Set 0 (`VulkanFrameDescriptorSetLayout`) is one set per swapchain image: binding 0 the camera
  UBO (`CameraUniformData`, vertex, fragment and compute), binding 1 the cascaded shadow map,
  binding 2 the previous-model storage buffer. `VulkanUniformBuffer` is rebuilt, after
  `WaitForAllFrames`, whenever the scene's texture set changes.
- The shadow map is one device-lifetime image shared by every frame in flight; its render pass's
  external dependencies order the clear after the previous frame's reads. That is the precedent
  for images outside `RenderTargetLayoutTracker`.
- Lights: `LightComponent` with intensity in lux for directional lights. `SelectShadowCasterLight`
  picks the brightest directional light. Ambient lights sum into `ambientLuminance`.
- Scene files are YAML version 3 (`SerializedSceneData`: entities, lights, gizmo, selection).
- `TextureLoader::LoadRGBA32F` (phase 1) decodes `.hdr`/`.exr` to linear RGBA32F.
- World units are metres, +Y up; the default camera looks down -Z.

## Locked Decisions

1. **Scene environment.** `SerializedSceneData` gains an `environment` with a mode
   (`None`, `Atmosphere`, `Hdri`), `AtmosphereSettings` and `HdriSettings`. It is saved as an
   optional `environment` node in the scene YAML, which stays version 3: a file without the node
   loads as `None`, so existing scenes look exactly as before. New scenes (the startup scene and
   "New Scene") start in `Atmosphere`.
2. **The sun is the shadow-casting directional light** (`SelectShadowCasterLight`). In
   `Atmosphere` mode its `color * intensity` is the illuminance at the top of the atmosphere, and
   the light the shaders receive is that times the atmospheric transmittance from the camera
   toward the sun, computed on the CPU each frame. Other directional lights are untouched. With no
   directional light the atmosphere has no sun and the sky is black. In `None` and `Hdri` mode no
   light is tinted.
3. **Hillaire 2020, complete**, Earth defaults from the paper:

   | Quantity | Value |
   |---|---|
   | Planet / atmosphere radius | 6360 km / 6460 km |
   | Rayleigh scattering, scale height | (5.802, 13.558, 33.1) x 1e-3 /km, 8 km |
   | Mie scattering, extinction, scale height, g | 3.996e-3 /km, 4.40e-3 /km, 1.2 km, 0.8 |
   | Ozone absorption, profile | (0.650, 1.881, 0.085) x 1e-3 /km, tent from 10 km to 40 km peaking at 25 km |
   | Ground albedo | 0.3 grey |
   | Sun angular diameter | 0.545 degrees |

   Four products, all `R16G16B16A16_SFLOAT` storage images written by compute:

   | LUT | Size | Parameterisation | Recomputed |
   |---|---|---|---|
   | Transmittance | 256 x 64 | Bruneton (r, mu) with the horizon-distance mapping; 40 steps | when atmosphere settings change |
   | Multiple scattering | 32 x 32 | altitude, sun cos zenith; 64 directions, 20 steps, isotropic phase, `L2 / (1 - f_ms)` | when atmosphere settings change |
   | Sky-view | 192 x 108 | azimuth relative to the sun, non-linear latitude concentrating texels at the horizon; 30 steps | every frame |
   | Aerial perspective | 32 x 32 x 32 | screen xy, 32 view-distance slices of 1 km (times the aerial perspective scale); rgb in-scattered luminance, a mean transmittance | every frame |

   The planet centre sits at `(0, -6360, 0)` km, so world y = 0 is the ground and world metres
   divide by 1000 into atmosphere kilometres. The camera altitude used by the LUTs is clamped to
   [0.5 m, top of atmosphere - 1 km]; space views are out of scope.
4. **Exposed atmosphere settings:** ground albedo (colour), Rayleigh density scale, Mie density
   scale, Mie anisotropy g, ozone density scale, aerial perspective distance scale, sun disk
   angular diameter. Planet and atmosphere radii and the base coefficients stay fixed.
5. **One CPU atmosphere model.** `engine/renderer/atmosphere.{h,cpp}` builds the per-km
   coefficients from `AtmosphereSettings` and integrates transmittance the same way as the LUT
   shader. The renderer uses it for the sun tint of decision 2; the tests use it to check the
   physics. The GLSL and the C++ share constants by construction: the C++ fills the UBO the
   shaders read, so no coefficient is written twice.
6. **Atmosphere GPU resources are device-lifetime and shared by the frames in flight**, like the
   shadow map: one image per LUT, kept in `VK_IMAGE_LAYOUT_GENERAL`, never seen by
   `RenderTargetLayoutTracker`. `VulkanAtmosphere` records its compute work after the shadow pass
   and before the scene passes, behind a barrier whose first scope (every earlier submission)
   covers the previous frame's fragment reads, and ends with a compute-write to fragment-read
   barrier. In `None` and `Hdri` mode it records nothing.
7. **Set 0 grows four fragment-stage bindings:** 3 transmittance LUT, 4 sky-view LUT, 5 aerial
   perspective volume (all linear-clamp samplers over the device-lifetime images), 6 HDRI
   equirectangular map (a 1x1 black image when none is loaded). A changed HDRI rebuilds
   `VulkanUniformBuffer` through the existing content path, after `WaitForAllFrames`.
8. **`CameraUniformData` gains an environment block**, appended after `prevViewProj`: sun
   direction and environment mode, sun top-of-atmosphere illuminance and cos of the sun's angular
   radius, Rayleigh, Mie and ozone coefficients, ground albedo, radii, aerial perspective km per
   slice, HDRI intensity and rotation. Every member a `vec4`, with the same `static_assert`
   discipline as the rest of the block.
9. **The sky is drawn by the forward pass**, after its opaque and mask items and before its blend
   items, in both orders: a fullscreen triangle at depth 1 with `LESS_OR_EQUAL` depth test and no
   depth write, so it covers exactly the pixels no geometry did, and blend items composite over
   the sky. One `sky.frag` handles every mode: `None` writes `GetBackgroundRadiance(exposure)`
   (pushed as today), `Atmosphere` samples the sky-view LUT and adds the sun disk, `Hdri` samples
   the equirectangular map. The lighting pass keeps writing the background where depth is 1; the
   sky overwrites it.
10. **Sun disk:** luminance `E_sun / (2 pi (1 - cos(angular radius)))` times the transmittance
    along the view ray, with limb darkening `1 - 0.6 (1 - mu^0.5)` where mu is the cosine across the
    disk, clamped to 65504 before it is written so the fp16 target never holds infinity.
11. **Aerial perspective applies to every lit surface in `Atmosphere` mode:** `deferred_lighting.frag`
    and `triangle.frag` (forward-only opaque and every blend item) end with
    `color * T + L` from the volume at the pixel's view distance, emissive included. Blend items
    apply it before blending.
12. **HDRI.** Loaded with `LoadRGBA32F` on a background thread (`std::async`), uploaded as
    `R32G32B32A32_SFLOAT` when that format supports linear filtering, else packed to
    `R16G16B16A16_SFLOAT`, one mip level, sampler repeat in u and clamp in v. Until it is ready the
    previous sky stays. Radiance is `texel * intensity` in cd/m^2 per texel unit, default 1000.
    Orientation: direction d maps to `u = 0.5 + atan(d.x, -d.z) / (2 pi) + rotation / 360`,
    `v = acos(d.y) / pi`, so the image centre is -Z, the default camera's view. The HDRI stores
    `path` and `uuid` like a model reference.
13. **Editor:** an Environment section in the Scene panel: mode, the atmosphere settings of
    decision 4, and for `Hdri` an asset picker (`.hdr`/`.exr`), intensity and rotation. Edits
    mark the scene dirty like any scene edit.

## Amendments During Implementation

- **Startup scene:** it gains a directional light "Sun" (120000 lux, 35 degrees up behind the default
  camera) along with `Atmosphere`, since the atmosphere has no light without one.
- **Aerial perspective slices** are spread quadratically over 32 km (Hillaire's reference
  implementation), not 1 km apart; the first half slice fades in linearly from the camera.
- **HDRI changes** rewrite set 0 binding 6 after `WaitForAllFrames` instead of rebuilding
  `VulkanUniformBuffer`. Until the map is loaded, or after it fails, the frame renders as `None`
  rather than keeping the previous sky.
- **Scene dirty tracking** does not exist in the editor, so decision 13's "mark the scene dirty"
  does not apply; the environment is saved with the scene like everything else.
- **Exposure metering:** the histogram used to skip every pixel at depth 1, because the flat
  background is divided by the exposure. A physical sky is radiance, so it is now metered whenever
  the mode is `Atmosphere` or `Hdri`; without this, a frame of sky metered nothing and kept its
  previous EV. Measured noon sky: EV100 15.5.
- **Verification tooling:** `--capture <png>` writes the viewport after `--frames`, logging the
  EV100; `VulkanUploadBatch::Flush` now submits command-only batches, which the readback needs.
- **Measured cost** (RTX 4070 Laptop, Release): sky-view plus aerial perspective 0.035 ms per
  frame; 0.10 ms on a frame that also rebuilds the transmittance and multiple-scattering LUTs.
- **Known limits:** below the horizon the sky is nearly black, since the camera sits 0.5 m above an
  undrawn ground; interiors lit only by the fallback ambient look dark next to the physical sky
  and haze until diffuse IBL (phase 3) lights them from the sky. (Fixed 2026-09-28, see Ground.)

## Ground (2026-09-28)

The sky-view LUT holds only the air's in-scattering, so below the horizon the background showed
the few metres of air in front of the ground: black. Now:

- **The sky draws the ground** (`GroundLuminance`, `atmosphere_sampling.glsl`), in `SampleSky` and
  `SampleSkyForLighting` alike, so the background, the SH irradiance and the environment capture
  agree. The ground point is where the view ray meets the planet; its light reaches the camera
  through the transmittance between them, `T(ground -> space) / T(camera -> space)` along
  `-direction`, from the transmittance LUT, while the LUT's own in-scattering hazes it toward the
  horizon.
- **Shaded as the lighting pass shades the ground plane** (`GroundSurfaceRadiance`): a dielectric of
  roughness 0.9 with the default F0; the sun through Burley's diffuse under the Fresnel toward it
  plus the GGX lobe; the sky's illuminance diffused by what the DFG term's specular share leaves,
  and the SH sky around the mirror direction reflected by that share. At grazing angles the
  specular parts and Burley's retroreflection are most of the ground's light, so a Lambertian
  ground was 20% darker than the plane at the far plane, a visible band.
- **The sky's illuminance on level ground** comes out of `atmosphere_irradiance.comp`, which cannot
  read the buffer it writes: it sums the sky's cosine-weighted radiance over the upper hemisphere
  alongside the SH, and the SH of the ground's view transmittance times its diffuse share, and adds
  their product times albedo / pi after the reduction. The illuminance is stored in the w of the
  first three coefficients (`GroundSkyIrradiance`). The SH leaves out the ground's sky reflection,
  a few percent of its light away from grazing angles.
- **Ground plane** (`AtmosphereSettings::groundPlane`, scene key `ground_plane`, off by default, the
  "Ground plane" checkbox): an endless plane at world y = 0 in the geometry pass (`ground.frag`, a
  full-screen triangle writing the depth where each pixel's view ray meets the plane, after the
  opaque items and before the decals), so it takes the sun's shadows, AO, SSR, screen-space GI and
  aerial perspective; and in the DDGI rays (`IntersectGroundPlane`), lit as any other hit, its
  underside a single-sided back face. Past the far plane it is left to the sky's ground, which
  matches it (measured: 82 vs 83 in 8-bit sRGB across the seam, noon, camera 1.6 m). The flag
  travels in `groundAlbedo.w`, 1 only under `Atmosphere`. Not in the forward-only order, the CPU
  reference path tracer, picking or the scene bounds.

## Seamless Horizon (2026-09-30)

With the ground in the sky, the horizon is a seam: the blue sky above, a flat brown sunlit ground
below it (fog off), or a wall of fog (fog on). `AtmosphereSettings::seamlessHorizon` (scene key
`seamless_horizon`, the "Seamless horizon" checkbox) takes the ground out, so the air above and below
the horizon is one.

- **The sky below the horizon is the sky above it, mirrored.** A ray that would meet the ground reads
  the sky-view LUT along its mirror image across the horizon (`FoldAcrossHorizon`): in the plane through
  the ray and the vertical, the zenith angle theta becomes 2 theta_h - theta, theta_h being the
  horizon's own (sin theta_h = R / H, past 90 degrees above the ground), so the two sides agree where
  the ground would have begun. No ground light, no sun disk below the horizon.
- **The same in every reader of the sky**: the background and the lighting (`SampleSky`,
  `SampleSkyForLighting`, so the environment probe, the reflections and the DDGI misses follow), and the
  radiance SH (`atmosphere_irradiance.comp`), which has no skylight bounced off the ground either. The
  clouds' underside is lit by the sky, not by the ground. Fog on the sky treats a downward ray as its
  mirror image too (`abs(direction.y)`): a finite column, not the opaque wall the ground ends.
- **Not with the ground plane**, which is a surface, and whose far-plane match with the sky's ground
  is the reason the two were built together; the flag is off in the uniform block (`atmosphereRadii.w`)
  while the plane is on, and the checkbox is disabled. Off by default in the struct and for scenes
  saved without the key; on for new scenes.
- **Measured** on an empty scene at 100 degrees of vertical FOV, fog off: the largest row-to-row
  luminance step at the horizon falls from 0.063 to 0.008 (and no longer sits on the horizon). Fog on,
  the sky is symmetrical about the horizon. On the Spa grid view the sun glare on the tarmac is gone: it
  was the road reflecting the sunlit ground below the horizon.

## Components

### `engine/scene` and `engine/logic`

- `scene_environment.h`: `EnvironmentMode`, `AtmosphereSettings` (the decision 4 fields with
  their defaults), `HdriSettings { path, uuid, intensity, rotationDegrees }`,
  `SceneEnvironment { mode, atmosphere, hdri }`.
- `SerializedSceneData::environment`; `IEditorWorld` gets and sets it; `editor_scene.cpp` reads
  and writes the `environment` node and captures it with the scene.

### `engine/renderer/atmosphere.{h,cpp}` (pure, `engine_render_core`)

```cpp
struct AtmosphereParameters  // per-km coefficients and radii, what the UBO block carries
AtmosphereParameters BuildAtmosphereParameters(const AtmosphereSettings& settings);
// Transmittance from altitude (km above the ground) toward a direction with this cos zenith, to
// the top of the atmosphere; zero when the ray hits the ground.
glm::vec3 ComputeTransmittanceToSpace(const AtmosphereParameters& p, float altitudeKm, float cosZenith);
```

### `engine/renderer/vulkan/atmosphere.{h,cpp}` and shaders

- `VulkanAtmosphere`: the four images and views, samplers, a compute descriptor set, four compute
  pipelines, `Record(cmd, frameDescriptorSet, settingsChanged)`, and `GetSampledBindings()` for
  set 0.
- `shaders/vulkan/atmosphere_common.glsl` (density profiles, ray-sphere intersection, LUT
  parameterisations, phase functions), `atmosphere_transmittance.comp`,
  `atmosphere_multiscattering.comp`, `atmosphere_skyview.comp`,
  `atmosphere_aerial_perspective.comp`, `atmosphere_sampling.glsl` (sky-view lookup, aerial
  perspective, equirectangular lookup for the fragment shaders), `sky.frag`.

### Renderer

- Per frame: environment from the editor world; `BuildAtmosphereParameters` when it changed (then
  the transmittance and multiple-scattering LUTs are recomputed that frame); the sun tint;
  the UBO environment block; `VulkanAtmosphere::Record` before the scene passes.
- `VulkanForwardPass` gets the sky pipeline and records it between its opaque and blend items.
- HDRI loading and upload, and the set 0 rebuild on change.

## Failure Handling

- An HDRI that fails to load logs an error, and the sky falls back to `None` until a new path is
  chosen; the scene keeps the path.
- A device without linear filtering for `R32G32B32A32_SFLOAT` gets the RGBA16F HDRI (values
  clamp at 65504).
- Every LUT format is in Vulkan's mandatory storage and linear-filter lists, so the atmosphere
  has no fallback path.

## Automated Verification

1. `miniengine.atmosphere` (`tests/atmosphere_tests.cpp`):
   - Zenith transmittance from the ground equals `exp(-optical depth)` computed by hand from the
     defaults, (0.940, 0.868, 0.762) within 1%.
   - Transmittance falls monotonically as the sun lowers, and at 2 degrees of elevation blue is
     below green below red.
   - From the top of the atmosphere looking up it is 1; looking into the ground it is 0.
   - Density scales of 0 give a transmittance of 1.
2. Scene serialization: an environment round-trips through YAML; a file without the node loads as
   `None`; a new scene is `Atmosphere`.
3. `CameraUniformData` offsets checked by `static_assert`.
4. The existing suite, and zero validation messages in Debug.

## Manual and Measured Acceptance

1. Sun at 60 degrees elevation: blue sky, white sun disk, horizon brighter than zenith. Sun at 2
   degrees: orange horizon near the sun, the directional light visibly warmer and dimmer. Sun
   below the horizon: twilight, then dark.
2. Aerial perspective at distance scale 100 on Sponza hazes the far end of the hall toward the sky
   colour; at 1 it is invisible at this scale, as it should be.
3. HDRI: a Poly Haven `.hdr` and an `.exr` show as the background, rotate with the rotation
   setting, and the image centre faces the default camera.
4. `None` looks exactly as before; forward-only and deferred orders show the same sky.
5. GPU cost from a throwaway timestamp capture, RTX 4070 Laptop, 1080p: sky-view plus aerial
   perspective per frame, and the two static LUTs when settings change.
6. Zero validation messages while switching modes, editing settings and changing HDRIs.

## Out of Scope

- Capturing the sky into a cubemap, and any sky lighting of surfaces (phases 3 and 4); the
  constant ambient term stays.
- Views from space or above the atmosphere, clouds, volumetric shadows in the atmosphere, and a
  moon or stars.
- Physically calibrating an HDRI to a measured illuminance.
